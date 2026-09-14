#include "ir_exp.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "vl53lx_api.h"
#include "vl53lx_core.h" // VL53LX_compute_histo_merge_nb
#include "vl53lx_core_support.h" // VL53LX_calc_pll_period_mm
#include "vl53lx_register_map.h"
#include "vl53lx_port.h"

#include "TCA9535.h" // i2c_bus_handle
#include "gpio_task.h" // xI2CBusMutex
#include "polycast5_macros.h" // POLYCAST5_DEBUG, POLYCAST5_USE_PSRAM_BSS

#define TAG "VL53L4CX"

// Validity gate: outside it, "no target" rather than a guess. Datasheet minimum linear response is 10 mm, and a
// tighter floor silently discards valid close readings - on a tape measure, exactly where people look.
#define VL53_TRUSTED_MIN_MM 10
// Crosstalk exclusion floor. Package crosstalk - emitter to receiver, no cover glass anywhere on this module -
// resolves as its own target a few mm out at a near-constant 1100-1190 kcps; measured 12-26 mm before continuous
// smudge correction and 3-9 mm after, so 40 mm is ~4x the worst residual. Not redundant with taking the strongest
// return: at long range the ghost OUTSHINES the echo (a logged 2985 mm frame had 117 kcps against its 1140). Nearer
// targets are considered only when nothing else valid was found, where the two have merged into one peak anyway.
#define VL53_XTALK_FLOOR_MM 40
// The datasheet's 6 m headline, so this gate never caps the range - not a claim that 6 m works (that figure is an
// 88% white target in the dark at a 50% detection rate; ST's practical guidance is 4 m for LONG). Not load bearing
// against aliasing either: a wrapped target arrives as status 7 and the status gate rejects it. Best confirmed here
// is 2985 mm at 117 kcps and 17 mm sigma; extrapolating 1/d^2 from 1531 mm at 968 kcps puts 6 m at ~63 kcps, under
// the 140-203 kcps indoor ambient floor, so expect 3.5-4.5 m in practice.
#define VL53_TRUSTED_MAX_MM 6000
// Backstop only - the histogram post-processing already rejects a smeared return with status 1, so tightening past
// the driver's own threshold throws away readings the sensor considers good. Sigma arrives as FixPoint1616.
#define VL53_MAX_SIGMA_MM 40

// Integration time per measurement. ST allows 8..200 ms and defaults to 33; longer buys precision and range, and
// this is the documented maximum - about five readings a second, well ahead of what the panel can usefully show.
// ST's "extended range" UWR unwrap reaches past LONG's ~4 m and is on by default, but vl53lx_api.c disables it
// unless the CURRENT and PREVIOUS measurement each found exactly one target, so crosstalk resolving as its own
// target suppresses it; continuous smudge correction in vl53l4cx_init() is what keeps the ghost merged.
#define VL53_TIMING_BUDGET_US 200000

// ST's whole context: 9,440 bytes, the only large allocation here, and safe in PSRAM - it is reached only from
// ir_exp_task under xI2CBusMutex, never from an ISR or with the cache disabled. All fixed-point (the C5 has no FPU).
static POLYCAST5_USE_PSRAM_BSS VL53LX_Dev_t s_dev_data;
// One measurement's targets, out of line so they do not land on ir_exp_task's stack once per reading
static POLYCAST5_USE_PSRAM_BSS VL53LX_MultiRangingData_t s_results;
// Detail snapshot scratch, 200 bytes; ST's post-processing already peaks around 3.4 kB on that stack
static POLYCAST5_USE_PSRAM_BSS VL53LX_AdditionalData_t s_add;

// Which RangeData[] row vl53l4cx_read_mm() last settled on, so the detail view can mark it; -1 when the frame
// produced nothing the gate would pass.
static int8_t s_chosen_idx = -1;

static VL53LX_DEV s_dev = NULL;

#ifdef POLYCAST5_DEBUG
// Diagnostics are rate limited to roughly one line a second so they stay readable
static TickType_t s_diag_next = 0;

static bool vl_diag_due(void)
{
    const TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - s_diag_next) < 0) {
        return false;
    }
    s_diag_next = now + pdMS_TO_TICKS(1000);
    return true;
}
#endif

// Caller holds xI2CBusMutex for every function here. ST's core sleeps inside its own poll loops and the port hands
// the mutex back across those sleeps - but only while it has been told the bus is held, so every entry point raises
// this before calling into the API and lowers it afterwards.
#define VL_ST_CALL(expr) \
    ({ \
        vl53lx_port_set_bus_held(true); \
        const VL53LX_Error vl_st_err_ = (expr); \
        vl53lx_port_set_bus_held(false); \
        vl_st_err_; \
    })

esp_err_t vl53l4cx_init(uint32_t scl_hz)
{
    if (i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_dev != NULL) { // Already up
        return ESP_OK;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = VL53L4CX_I2C_ADDR,
        .scl_speed_hz = scl_hz,
    };
    i2c_master_dev_handle_t bus_dev = NULL;
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &bus_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    memset(&s_dev_data, 0, sizeof(s_dev_data));
    s_dev_data.i2c = bus_dev;
    s_dev = &s_dev_data;

    // With no XSHUT the part keeps whatever the previous session left behind, and a cold boot can find it still
    // ranging with its interrupt latched. Raw writes, not VL53LX_StopMeasurement, which wants a live context.
    (void)VL_ST_CALL(VL53LX_WrByte(s_dev, VL53LX_SYSTEM__MODE_START, 0x00));
    (void)VL_ST_CALL(VL53LX_WrByte(s_dev, VL53LX_SYSTEM__INTERRUPT_CLEAR, 0x01));

    VL53LX_Error st = VL_ST_CALL(VL53LX_WaitDeviceBooted(s_dev));
    if (st != VL53LX_ERROR_NONE) {
        // XSHUT is not routed on this module and the 3V3 rail is latched on, never cycled, so a soft reset is the
        // only recovery there is: without it a part that came up wedged stays dead until the battery is pulled. It
        // must run BEFORE VL53LX_DataInit(), since the reset returns the device to power-on defaults and that is
        // only consistent with ST's VL53LX_LLDriverData_t shadow while the shadow is still the zeroed one above.
        ESP_LOGW(TAG, "Firmware never reported ready (ST error %d); trying a soft reset",
                (int)st);

        // 0x0000 is SYSTEM__SOFT_RESET; ST's sequence is assert, pause, de-assert, then wait for the boot. Both
        // waits use the port's VL53LX_WaitUs rather than vTaskDelay: at CONFIG_FREERTOS_HZ=100 a pdMS_TO_TICKS
        // under 10 ms rounds to zero ticks and would not wait at all.
        (void)VL_ST_CALL(VL53LX_WrByte(s_dev, VL53LX_SOFT_RESET, 0x00));
        (void)VL53LX_WaitUs(s_dev, 100);
        (void)VL_ST_CALL(VL53LX_WrByte(s_dev, VL53LX_SOFT_RESET, 0x01));
        (void)VL53LX_WaitUs(s_dev, 1000); // Boot is ~1.2 ms; the poll below covers the rest

        st = VL_ST_CALL(VL53LX_WaitDeviceBooted(s_dev));
        if (st != VL53LX_ERROR_NONE) {
            ESP_LOGE(TAG, "Still not ready after a soft reset (ST error %d)", (int)st);
            ret = ESP_ERR_TIMEOUT;
            goto fail;
        }
        ESP_LOGI(TAG, "Rangefinder recovered by soft reset");
    }

    // Reads the NVM, loads the tuning parameters and puts the part in the histogram preset
    st = VL_ST_CALL(VL53LX_DataInit(s_dev));
    if (st != VL53LX_ERROR_NONE) {
        ESP_LOGE(TAG, "VL53LX_DataInit failed (ST error %d)", (int)st);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }

    // LONG is the preset whose two VCSEL periods are far enough apart to resolve the phase aliasing that folds the
    // range past ~1 m (distance walks backwards while status, sigma and SPAD count all stay clean). ST detects the
    // wrap by ranging in PAIRS at two pulse-repetition intervals and comparing phases host-side over full
    // histograms, so a scalar register-poke path cannot see it however it is configured.
    st = VL_ST_CALL(VL53LX_SetDistanceMode(s_dev, VL53LX_DISTANCEMODE_LONG));
    if (st != VL53LX_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to select long distance mode (ST error %d)", (int)st);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }

    st = VL_ST_CALL(VL53LX_SetMeasurementTimingBudgetMicroSeconds(s_dev, VL53_TIMING_BUDGET_US));
    if (st != VL53LX_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to set the %u us timing budget (ST error %d)",
                (unsigned)VL53_TIMING_BUDGET_US, (int)st);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }

    // Attack the optical crosstalk at source: VL53LX_DataInit() selects VL53LX_SMUDGE_CORRECTION_NONE, so without
    // this the package's emitter-to-receiver leak survives into the target list as a ghost. CONTINUOUS estimates it
    // from live ranging data, needing no calibration run unlike VL53LX_PerformXTalkCalibration; the two calls cost
    // 256 bytes and the Perform*Calibration entry points stay stripped. It moved the ghost in from 12-26 mm to 3-9 mm
    // without removing it, so neither is fatal - VL53_XTALK_FLOOR_MM and the read_mm selection are the real guards.
    VL53LX_Error aux = VL_ST_CALL(VL53LX_SetXTalkCompensationEnable(s_dev, 1));
    if (aux != VL53LX_ERROR_NONE) {
        ESP_LOGW(TAG, "Crosstalk compensation refused (ST error %d)", (int)aux);
    }
    aux = VL_ST_CALL(VL53LX_SmudgeCorrectionEnable(s_dev, VL53LX_SMUDGE_CORRECTION_CONTINUOUS));
    if (aux != VL53LX_ERROR_NONE) {
        ESP_LOGW(TAG, "Continuous smudge correction refused (ST error %d)", (int)aux);
    }

#ifdef POLYCAST5_DEBUG
    uint8_t rev_major = 0;
    uint8_t rev_minor = 0;
    (void)VL_ST_CALL(VL53LX_GetProductRevision(s_dev, &rev_major, &rev_minor));
    ESP_LOGI(TAG, "VL53L4CX up at %lu Hz (ST bare driver, product rev %u.%u, "
            "context %u B in PSRAM)", (unsigned long)scl_hz,
            (unsigned)rev_major, (unsigned)rev_minor, (unsigned)sizeof(s_dev_data));
#endif
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(bus_dev);
    s_dev_data.i2c = NULL;
    s_dev = NULL;
    return ret;
}

void vl53l4cx_deinit(void)
{
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev_data.i2c);
        s_dev_data.i2c = NULL;
        s_dev = NULL;
    }
}

bool vl53l4cx_is_present(void)
{
    return s_dev != NULL;
}

esp_err_t vl53l4cx_start(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (VL_ST_CALL(VL53LX_StartMeasurement(s_dev)) != VL53LX_ERROR_NONE) {
        return ESP_FAIL;
    }

    // Discard the synchronisation interrupt, as ST's own integration does right after StartMeasurement: back-to-back
    // ranging raises one interrupt as it starts whose data means nothing (VL53LX_RANGESTATUS_SYNCRONISATION_INT,
    // "Ignore data."). The status gate would reject it anyway; clearing it costs one transaction and saves a wasted
    // cycle plus a "no targets found" line at every session start.
    return (VL_ST_CALL(VL53LX_ClearInterruptAndStartMeasurement(s_dev)) == VL53LX_ERROR_NONE)
            ? ESP_OK : ESP_FAIL;
}

esp_err_t vl53l4cx_park_cold(uint32_t scl_hz)
{
    // A teardown the part refused still drops the handle, leaving it ranging at 19-21 mA with nothing able to address
    // it. This neither boots the firmware nor runs DataInit: just the pair of raw writes init already uses to force a
    // known state, which ST's own integration also does before its context exists.
    if (s_dev != NULL) { // A session owns the handle; use the ordinary path
        return vl53l4cx_stop();
    }

    if (i2c_bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = VL53L4CX_I2C_ADDR,
        .scl_speed_hz = scl_hz,
    };
    i2c_master_dev_handle_t bus_dev = NULL;
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &bus_dev);
    if (ret != ESP_OK) {
        return ret;
    }

    // A borrowed context: only the i2c handle is touched, and s_dev goes back to NULL on the way out so
    // vl53l4cx_is_present() keeps meaning "a session owns it" and the next real init starts from scratch.
    memset(&s_dev_data, 0, sizeof(s_dev_data));
    s_dev_data.i2c = bus_dev;
    s_dev = &s_dev_data;

    const VL53LX_Error a = VL_ST_CALL(VL53LX_WrByte(s_dev, VL53LX_SYSTEM__MODE_START, 0x00));
    (void)VL_ST_CALL(VL53LX_WrByte(s_dev, VL53LX_SYSTEM__INTERRUPT_CLEAR, 0x01));

    s_dev = NULL;
    s_dev_data.i2c = NULL;
    i2c_master_bus_rm_device(bus_dev);

    return (a == VL53LX_ERROR_NONE) ? ESP_OK : ESP_FAIL;
}

esp_err_t vl53l4cx_stop(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Reaches SW STANDBY: 4-9 uA against 19-21 mA while ranging. HW STANDBY (XSHUT low, not routed on this module)
    // is 3-7 uA, so the missing pin costs a couple of microamps, not the milliamps this API's shape suggests.
    return (VL_ST_CALL(VL53LX_StopMeasurement(s_dev)) == VL53LX_ERROR_NONE)
            ? ESP_OK : ESP_FAIL;
}

esp_err_t vl53l4cx_read_mm(int32_t *dist_mm)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dist_mm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // GPIO1 is tied off on this module, so data-ready is polled rather than interrupt driven
    uint8_t ready = 0;
    if (VL_ST_CALL(VL53LX_GetMeasurementDataReady(s_dev, &ready)) != VL53LX_ERROR_NONE) {
        return ESP_FAIL;
    }
    if (!ready) {
        return ESP_ERR_NOT_FINISHED;
    }

    // The histogram post-processing runs here, host-side - not a bare register fetch
    const VL53LX_Error st = VL_ST_CALL(VL53LX_GetMultiRangingData(s_dev, &s_results));

    // Ranging stays halted until the interrupt is acknowledged, so this happens even if the read above failed
    const VL53LX_Error clr = VL_ST_CALL(VL53LX_ClearInterruptAndStartMeasurement(s_dev));

    if (st != VL53LX_ERROR_NONE) {
        return ESP_FAIL;
    }

    // Two passes, and the order matters - see VL53_XTALK_FLOOR_MM for why neither rule works alone. Pass 1 takes the
    // STRONGEST target beyond the crosstalk floor; nearest is wrong because ST sorts by distance, so the ghost lands
    // at RangeData[0] once it resolves as its own peak and pins the reading to ~1.5 cm past about a metre. Pass 2
    // runs only if pass 1 found nothing, dropping to the part's own minimum so close-up readings still work. Status 7
    // (wrapped) and 1 or 2 (smeared, starved) are skipped by both.
    uint8_t found = s_results.NumberOfObjectsFound;
    if (found > VL53LX_MAX_RANGE_RESULTS) { // Documented range, but this indexes an array
        found = VL53LX_MAX_RANGE_RESULTS;
    }

    int32_t chosen = VL53L4CX_NO_TARGET;
    s_chosen_idx = -1;
    for (int pass = 0; pass < 2 && chosen == VL53L4CX_NO_TARGET; ++pass) {
        const int32_t floor_mm = (pass == 0) ? VL53_XTALK_FLOOR_MM : VL53_TRUSTED_MIN_MM;
        FixPoint1616_t best_signal = 0;

        for (uint8_t i = 0; i < found; ++i) {
            const VL53LX_TargetRangeData_t *t = &s_results.RangeData[i];
            if (t->RangeStatus != VL53LX_RANGESTATUS_RANGE_VALID) {
                continue;
            }
            if ((t->SigmaMilliMeter >> 16) >= VL53_MAX_SIGMA_MM) {
                continue;
            }

            const int32_t d = (int32_t)t->RangeMilliMeter;
            if (d < floor_mm || d > VL53_TRUSTED_MAX_MM) {
                continue;
            }
            if (chosen == VL53L4CX_NO_TARGET || t->SignalRateRtnMegaCps > best_signal) {
                best_signal = t->SignalRateRtnMegaCps;
                chosen = d;
                s_chosen_idx = (int8_t)i; // Which row the detail view should highlight
            }
        }
    }

#ifdef POLYCAST5_DEBUG
    if (vl_diag_due()) {
        if (found > 0) {
            // Every target, not just the first: which one gets picked is the whole question, and a single-target line
            // cannot show a ghost being rejected. Rates are MCPS in 16.16, widened before scaling to avoid overflow.
            char line[160];
            int n = 0;
            for (uint8_t i = 0; i < found && n >= 0 && n < (int)sizeof(line); ++i) {
                const VL53LX_TargetRangeData_t *t = &s_results.RangeData[i];
                n += snprintf(&line[n], sizeof(line) - (size_t)n,
                        "[%d mm st%u sig%u kcps sigma%u] ", (int)t->RangeMilliMeter,
                        (unsigned)t->RangeStatus,
                        (unsigned)(((uint64_t)t->SignalRateRtnMegaCps * 1000u) >> 16),
                        (unsigned)(t->SigmaMilliMeter >> 16));
            }
            // Stack headroom, because ST's histogram post-processing runs on the CALLING task's stack: measured peak
            // ~3.4 kB, which is why ir_exp_task's stack was raised from 4 kB. No unit conversion - IDF's
            // uxTaskGetStackHighWaterMark returns BYTES, not words, so scaling it over-reports the headroom fourfold.
            ESP_LOGI(TAG, "targets %u %s-> reported %d (ambient %u kcps, spads %u, "
                    "stack free %u B)",
                    (unsigned)found, line, (int)chosen,
                    (unsigned)(((uint64_t)s_results.RangeData[0].AmbientRateRtnMegaCps
                            * 1000u) >> 16),
                    (unsigned)(s_results.EffectiveSpadRtnCount >> 8),
                    (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else {
            ESP_LOGI(TAG, "no targets found");
        }
    }
#endif

    *dist_mm = chosen;
    return (clr == VL53LX_ERROR_NONE) ? ESP_OK : ESP_FAIL;
}

esp_err_t vl53l4cx_recalibrate(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Stopping and restarting re-runs the sensor's internal self-calibration, which cancels its 1.3 mm/C offset
    // drift. NOT one of ST's VL53LX_Perform*Calibration entry points: crosstalk is characterised in NVM and there is
    // no cover glass here for a system-level run, so a failed run would only store ST's fallback shape over the
    // factory value. No software range offset either.
    esp_err_t ret = vl53l4cx_stop();
    if (ret != ESP_OK) {
        return ret;
    }
    return vl53l4cx_start();
}

esp_err_t vl53l4cx_get_detail(ir_exp_tof_detail_t *out)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    // Straight out of the block vl53l4cx_read_mm() already filled - no driver call, no bus time. Every target is
    // reported, rejected ones included: showing the crosstalk ghost being turned down is the point.
    uint8_t n = s_results.NumberOfObjectsFound;
    if (n > IR_EXP_TOF_MAX_TARGETS) {
        n = IR_EXP_TOF_MAX_TARGETS;
    }
    out->n_targets = n;
    out->chosen = (s_chosen_idx < (int8_t)n) ? s_chosen_idx : -1;
    out->spads = (uint16_t)(s_results.EffectiveSpadRtnCount >> 8);

    for (uint8_t i = 0; i < n; ++i) {
        const VL53LX_TargetRangeData_t *t = &s_results.RangeData[i];
        out->target[i].dist_mm = t->RangeMilliMeter;
        out->target[i].min_mm = t->RangeMinMilliMeter;
        out->target[i].max_mm = t->RangeMaxMilliMeter;
        out->target[i].sigma_mm_q8 = (uint16_t)(t->SigmaMilliMeter >> 8); // 16.16 -> 8.8
        // Rates are MCPS in 16.16; widen before scaling so a bright near target cannot overflow the multiply
        out->target[i].signal_kcps =
                (uint32_t)(((uint64_t)t->SignalRateRtnMegaCps * 1000u) >> 16);
        out->target[i].ambient_kcps =
                (uint32_t)(((uint64_t)t->AmbientRateRtnMegaCps * 1000u) >> 16);
        out->target[i].status = t->RangeStatus;
    }

    // VL53LX_GetAdditionalData() is a memcpy out of the driver's own RAM - it issues no I2C, so no VL_ST_CALL bracket
    if (VL53LX_GetAdditionalData(s_dev, &s_add) != VL53LX_ERROR_NONE) {
        return ESP_FAIL;
    }

    const VL53LX_histogram_bin_data_t *h = &s_add.VL53LX_p_006;
    for (int i = 0; i < IR_EXP_TOF_HIST_BINS; ++i) {
        out->bin[i] = h->bin_data[i];
        if (h->bin_data[i] > out->bin_max) {
            out->bin_max = h->bin_data[i];
        }
    }

    out->ambient_bins = h->number_of_ambient_bins;
    out->ambient_lvl = h->VL53LX_p_028; // Per-bin ambient floor
    out->zero_phase = h->zero_distance_phase;
    // Millimetres per phase LSB in 17.15, the same scale ST's own wrap-distance maths uses
    out->pll_mm_q15 = VL53LX_calc_pll_period_mm(h->VL53LX_p_015);

    uint8_t merge = 0;
    if (VL53LX_compute_histo_merge_nb(s_dev, &merge) != VL53LX_ERROR_NONE || merge == 0) {
        merge = 1; // Never report a zero the plot would divide by
    }
    out->merge_nb = merge;

    return ESP_OK;
}


