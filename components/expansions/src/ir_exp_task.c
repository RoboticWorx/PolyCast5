#include "ir_exp_task.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs.h"

#include "TCA9535.h" // i2c_bus_handle
#include "gpio_task.h" // xI2CBusMutex
#include "polycast5_macros.h" // POLYCAST5_PRIORITY_MEDIUM, POLYCAST5_DEBUG

#define TAG "IR_EXP"

#define BUS_WAIT_MS         1000 // Bounded: a wedged bus must not hang this task forever
#define LOOP_TICK_MS         10  // One FreeRTOS tick at CONFIG_FREERTOS_HZ=100
#define HEARTBEAT_MS         500 // Publish even without a new frame, so the side panel stays live

// How long a sensor may go without a reading before its last one is withdrawn; each clock starts at that sensor's
// FIRST good reading. Several times the natural cadence: expiring early flickers, expiring late lies.
#define IMAGER_STALE_MS      2000
#define TOF_STALE_MS         2000
#define MEDICAL_STALE_MS     5000

// Longer where a sensor answered the probe but never produced a reading: ~205 ms to first data, ~1 s of spot-sensor pairing
#define FIRST_READING_MS     10000
#define MEDICAL_POLL_MS      100 // The spot sensor only produces a pair about once a second

#define IMAGER_STABILIZE_MS  180000 // Absolute accuracy needs this long after a cold wake
#define PARK_ACK_WAIT_MS     2000  // Bounded wait for the pre-sleep park acknowledgement
#define PARK_EXIT_WAIT_MS    1000  // After an unanswered park, how long to wait for the producer to tear down
#define PARK_EXIT_POLL_MS      20  // Two ticks at CONFIG_FREERTOS_HZ=100
#define CREATE_EXIT_WAIT_MS  2500  // Wait for a previous teardown: the three chained BUS_WAIT_MS takes in
                                   // session_stop() plus a half-seated module's per-transfer I2C timeouts
#define CREATE_EXIT_POLL_MS    20  // Two ticks at CONFIG_FREERTOS_HZ=100

#define SCL_HZ_FAST 400000
#define SCL_HZ_SLOW 100000

// Centre 2x2 of the 32x24 grid - averaging four pixels halves the crosshair noise
#define CENTRE_A ((MLX90642_ROWS / 2 - 1) * MLX90642_COLS + (MLX90642_COLS / 2 - 1))
#define CENTRE_B (CENTRE_A + 1)
#define CENTRE_C (CENTRE_A + MLX90642_COLS)
#define CENTRE_D (CENTRE_C + 1)

#define TOF_AVG_N       3   // The 100 ms timing budget does most of the smoothing now

// Drop the averaging history as soon as the reading really moves, or the display trails the target - which reads as the range running backwards
#define TOF_RESET_MM    40

// Re-run the rangefinder self-calibration once ambient has moved this far (1.3 mm/C)
#define TOF_RECAL_DELTA_C100 300

QueueHandle_t xIrExpStatusQueue = NULL;
QueueHandle_t xIrExpCtrlQueue = NULL;
QueueHandle_t xIrExpDoneQueue = NULL;
QueueHandle_t xIrExpTofDetailQueue = NULL;

// Double-buffered frames in PSRAM, allocated on START and freed on STOP
static int16_t *s_frame[2] = { NULL, NULL };
static uint8_t s_write_idx = 0;

// Per-pixel exponential average: ~0.21 C RMS of noise at 8 Hz, and alpha = 0.5 is one add and one shift for ~42% of it
static int16_t *s_ema = NULL;
static bool s_ema_seeded = false;

static uint32_t s_seq = 0;

// Staged here rather than on the stack: 196 bytes on top of ST's ~3.4 kB histogram post-processing peak
static ir_exp_tof_detail_t s_detail;

// When the array was last woken; absolute accuracy needs up to 180 s of thermal settling after that
static TickType_t s_imager_wake_tick = 0;

const int16_t *ir_exp_get_frame(uint8_t idx)
{
    return (idx < 2) ? s_frame[idx] : NULL;
}

// Bring the array up, preferring 400 kHz: a frame read is ~35 ms there, ~139 ms at 100 kHz - past the 8 Hz frame period, so
// the slow path also slows the sensor. The rate is set BOTH ways: it lives in sensor EEPROM. Returns the clock, 0 if absent.
static uint32_t negotiate_imager(void)
{
    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) != pdTRUE) {
        return 0;
    }

    uint32_t scl = 0;
    const esp_err_t fast = mlx90642_init(SCL_HZ_FAST);
    if (fast == ESP_OK) {
        scl = SCL_HZ_FAST;

#ifdef POLYCAST5_DEBUG
        // Diagnostic only: does NOT demote the clock. init() verifies ~36 clock edges where every frame is ~13,800, so
        // the frame read is the transfer worth measuring - but a demotion writes the 100 kHz code into sensor EEPROM.
        if (s_frame[0] != NULL) {
            const esp_err_t probe = mlx90642_read_frame(s_frame[0], NULL);
            ESP_LOGI(TAG, "400 kHz frame probe: %s", esp_err_to_name(probe));
        }
#endif
    } else if (fast == ESP_ERR_NOT_SUPPORTED) {
        // A configuration refusal, not signal integrity. The retry below would read the same EEPROM word and fail
        // identically, and its warning would send someone after the pull-ups instead of the line init() just logged.
        mlx90642_deinit();
    } else {
        // Most likely signal integrity - the module's 10k pull-ups are weak for fast mode. Fall back rather than run unreliably.
        ESP_LOGW(TAG, "Imager did not verify at 400 kHz, falling back to 100 kHz");
        mlx90642_deinit();
        if (mlx90642_init(SCL_HZ_SLOW) == ESP_OK) {
            scl = SCL_HZ_SLOW;
        }
    }

    if (scl != 0) {
        // 2 Hz at 100 kHz, 8 Hz otherwise. A ~139 ms frame read is one indivisible transfer, so at 4 Hz it would
        // hold xI2CBusMutex 56% of the time on a bus gpio_task needs every 20 ms; 2 Hz is 28%, parity with fast.
        const uint8_t want = (scl >= SCL_HZ_FAST) ? MLX90642_REFRESH_8HZ : MLX90642_REFRESH_2HZ;
        const esp_err_t r = mlx90642_set_refresh(want);
        if (r != ESP_OK) {
            // Not fatal: the frame period follows whatever rate the sensor reports, so this costs frame rate, not correctness
            ESP_LOGW(TAG, "Could not set refresh code %u (%s); running at %u ms frames",
                    want, esp_err_to_name(r), mlx90642_frame_period_ms());
        }
        s_imager_wake_tick = xTaskGetTickCount();
    }

    xSemaphoreGive(xI2CBusMutex);
    return scl;
}

// Free the streaming buffers (the frames are only needed while the page is open)
static void free_buffers(void)
{
    heap_caps_free(s_frame[0]);
    heap_caps_free(s_frame[1]);
    heap_caps_free(s_ema);
    s_frame[0] = s_frame[1] = NULL;
    s_ema = NULL;
    s_ema_seeded = false;
}

static bool alloc_buffers(void)
{
    const size_t bytes = MLX90642_PIXELS * sizeof(int16_t);
    if (s_frame[0] == NULL) {
        s_frame[0] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_frame[1] == NULL) {
        s_frame[1] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_ema == NULL) {
        s_ema = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (s_frame[0] == NULL || s_frame[1] == NULL || s_ema == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM frame buffers");
        free_buffers();
        return false;
    }

    // heap_caps_malloc hands back whatever the last owner left, so anything reaching the page before the first good frame is black
    memset(s_frame[0], 0, bytes);
    memset(s_frame[1], 0, bytes);
    memset(s_ema, 0, bytes);

    s_ema_seeded = false;
    return true;
}

// Read, filter and measure one frame into the buffer that is not currently published.
// Returns false if the array had nothing usable yet. Caller must hold xI2CBusMutex.
static bool process_frame(ir_exp_status_t *st)
{
    const uint8_t widx = (uint8_t)(s_write_idx ^ 1u);
    int16_t *dst = s_frame[widx];
    int16_t die = 0;

    if (mlx90642_read_frame(dst, &die) != ESP_OK) {
        return false;
    }

    // Count the sentinels rather than bailing on the first: -273.16 C on EVERY pixel is warmup and must never reach the
    // auto-range, but a few are unresolved pixels, or ones permanently defective below firmware 1.18.0 - and dropping
    // those frames would freeze the image for the life of the session.
    int bad = 0;
    for (int i = 0; i < MLX90642_PIXELS; ++i) {
        if (dst[i] == MLX90642_INVALID_RAW) {
            ++bad;
        }
    }
    if (bad >= MLX90642_PIXELS / 4) {
        return false; // Warming up, or too little of the frame resolved to be worth showing
    }

    // Which crosshair pixels resolved THIS frame; after the substitution below they are indistinguishable from real readings
    uint8_t centre_ok = 0;
    if (dst[CENTRE_A] != MLX90642_INVALID_RAW) centre_ok |= 1u;
    if (dst[CENTRE_B] != MLX90642_INVALID_RAW) centre_ok |= 2u;
    if (dst[CENTRE_C] != MLX90642_INVALID_RAW) centre_ok |= 4u;
    if (dst[CENTRE_D] != MLX90642_INVALID_RAW) centre_ok |= 8u;

    // Substitute before anything else reads dst: the EMA, min/max and centre average all need a plausible number. The
    // stand-in is this frame's mean of the valid pixels - history would pin a never-resolved pixel to the first scene
    // for the session. The count test above leaves 3/4 valid, so the divisor is never zero.
    if (bad > 0) {
        int32_t sum = 0;
        for (int i = 0; i < MLX90642_PIXELS; ++i) {
            if (dst[i] != MLX90642_INVALID_RAW) {
                sum += dst[i];
            }
        }
        const int16_t fill = (int16_t)(sum / (MLX90642_PIXELS - bad));
        for (int i = 0; i < MLX90642_PIXELS; ++i) {
            if (dst[i] == MLX90642_INVALID_RAW) {
                dst[i] = fill;
            }
        }
    }

    // Seed on the first good frame so the average does not ramp up from zero
    if (!s_ema_seeded) {
        memcpy(s_ema, dst, MLX90642_PIXELS * sizeof(int16_t));
        s_ema_seeded = true;
    }

    int32_t mn = INT16_MAX;
    int32_t mx = INT16_MIN;
    for (int i = 0; i < MLX90642_PIXELS; ++i) {
        const int32_t f = ((int32_t)s_ema[i] + (int32_t)dst[i]) >> 1; // alpha = 0.5
        s_ema[i] = (int16_t)f;
        dst[i] = (int16_t)f;
        if (f < mn) mn = f;
        if (f > mx) mx = f;
    }

    st->buf_idx = widx;
    st->min_c50 = (int16_t)mn;
    st->max_c50 = (int16_t)mx;
    st->t_die_c100 = die;
    // Only the pixels that genuinely resolved: a substituted one would fold the scene mean into the number being pointed at
    {
        int32_t csum = 0;
        int cn = 0;
        if (centre_ok & 1u) { csum += dst[CENTRE_A]; ++cn; }
        if (centre_ok & 2u) { csum += dst[CENTRE_B]; ++cn; }
        if (centre_ok & 4u) { csum += dst[CENTRE_C]; ++cn; }
        if (centre_ok & 8u) { csum += dst[CENTRE_D]; ++cn; }
        if (cn == 0) {
            csum = (int32_t)dst[CENTRE_A] + dst[CENTRE_B] + dst[CENTRE_C] + dst[CENTRE_D];
            cn = 4;
        }
        st->center_c50 = (int16_t)(csum / cn);
    }

    s_write_idx = widx; // Published; the next frame goes to the other buffer
    return true;
}

static uint32_t s_scl_hz = 0;
// The task exists only while the page is open; NULL is the authoritative "no session" test ir_exp_task_park() branches on
static TaskHandle_t s_task = NULL;

static int32_t s_tof_hist[TOF_AVG_N];
static uint8_t s_tof_n = 0;
static uint8_t s_tof_head = 0;

static void tof_reset(void)
{
    s_tof_n = 0;
    s_tof_head = 0;
}

// Average the last few valid ranges, dropping the history on a large step so the reading snaps when the user re-aims
static int32_t tof_push(int32_t mm)
{
    if (s_tof_n > 0) {
        const int32_t last = s_tof_hist[(s_tof_head + TOF_AVG_N - 1) % TOF_AVG_N];
        int32_t delta = mm - last;
        if (delta < 0) delta = -delta;
        if (delta > TOF_RESET_MM) {
            tof_reset();
        }
    }

    s_tof_hist[s_tof_head] = mm;
    s_tof_head = (uint8_t)((s_tof_head + 1) % TOF_AVG_N);
    if (s_tof_n < TOF_AVG_N) {
        s_tof_n++;
    }

    int32_t sum = 0;
    for (uint8_t i = 0; i < s_tof_n; ++i) {
        sum += s_tof_hist[i];
    }
    return sum / s_tof_n;
}

static bool imager_park(void); // Defined below; session_stop() parks on the way out
void ir_exp_cold_park_all(void); // Also below; the retry path for a refused teardown

static void session_start(ir_exp_done_t *done)
{
    memset(done, 0, sizeof(*done));
    done->cmd = IR_EXP_CMD_START;

    // Every early return below can leave the array awake: mlx90642_init() sends the wake byte before it verifies anything
    if (!alloc_buffers()) {
        // Said explicitly rather than inferred from done->imager: this runs BEFORE the probe, so imager reads false on a cold entry
        done->no_mem = true;
        done->imager = mlx90642_is_present();
        return;
    }

    // Present with no recorded clock means a teardown gave up partway. mlx90642_init() short-circuits on an existing
    // handle, so without dropping it the reported speed is not the one in use and the companions get 0 Hz and refuse.
    if (mlx90642_is_present() && s_scl_hz == 0) {
        if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
            mlx90642_deinit();
            xSemaphoreGive(xI2CBusMutex);
        }
    }

    // A handle here means this session already probed the array, so it is at the negotiated clock already
    if (!mlx90642_is_present()) {
        s_scl_hz = negotiate_imager();
    }
    done->scl_hz = s_scl_hz;
    done->imager = mlx90642_is_present();

    if (!done->imager) { // No array means no page
        free_buffers();
        return;
    }

    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
        (void)mlx90642_wake(); // Harmless if it never slept
        done->medical = (mlx90632_init(s_scl_hz) == ESP_OK);
        done->tof = (vl53l4cx_init(s_scl_hz) == ESP_OK);
        xSemaphoreGive(xI2CBusMutex);
    }

    if (done->tof) {
        // Nothing to restore: the part runs on its own NVM crosstalk calibration.
        if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
            const esp_err_t r = vl53l4cx_start();
            xSemaphoreGive(xI2CBusMutex);
            if (r != ESP_OK) {
                ESP_LOGE(TAG, "Rangefinder failed to start: %s", esp_err_to_name(r));
            }
        }
    }

    tof_reset();

    // Drop the previous visit's descriptor: the status queue is length 1 and written with xQueueOverwrite, so with
    // s_seq restarting at 0 the page would accept a stale frame and follow its buf_idx into a fresh, unfilled buffer.
    if (xIrExpStatusQueue != NULL) {
        xQueueReset(xIrExpStatusQueue);
    }

    // Same for the detail snapshot, peeked with no sequence number to vet it: the TOF flag goes up as soon as the rangefinder answers
    if (xIrExpTofDetailQueue != NULL) {
        xQueueReset(xIrExpTofDetailQueue);
    }
    s_seq = 0;

    done->ok = true;

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Session up: imager=%d tof=%d medical=%d at %lu Hz",
            done->imager, done->tof, done->medical, (unsigned long)s_scl_hz);
#endif
}

// Stop the rangefinder and the spot sensor, then release their handles. Returns false if either could not be parked -
// busy bus or refusal - which the caller must retry through ir_exp_cold_park_all(); they may still draw ~21 mA.
static bool sensors_park(void)
{
    if (!vl53l4cx_is_present() && !mlx90632_is_present()) {
        return true; // Already down
    }

    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) != pdTRUE) {
        return false; // Nothing tried
    }

    // The handle is released even on a refusal: vl53l4cx_init() short-circuits on a live one, so a reseated module would skip DataInit
    bool refused = false;

    if (vl53l4cx_is_present()) {
        if (vl53l4cx_stop() != ESP_OK) {
            ESP_LOGW(TAG, "Rangefinder refused the stop; releasing it anyway");
            refused = true;
        }
        vl53l4cx_deinit();
    }
    if (mlx90632_is_present()) {
        if (mlx90632_set_continuous(false) != ESP_OK) {
            ESP_LOGW(TAG, "Spot sensor refused the stop; releasing it anyway");
            refused = true;
        }
        mlx90632_deinit();
    }

    xSemaphoreGive(xI2CBusMutex);
    return !refused;
}

static void session_stop(void)
{
    const bool sensors_down = sensors_park();

    free_buffers();

    // Parked here rather than on a timer: nothing is left running to service a deferred shutdown, and the tick stops in light sleep
    const bool imager_down = imager_park();

    // Anything refused, or untried on a busy bus, gets one more attempt through fresh handles, or the parts keep drawing up to ~49 mA all sleep
    if (!sensors_down || !imager_down) {
        ESP_LOGW(TAG, "Teardown incomplete (sensors=%d array=%d); retrying cold",
                (int)sensors_down, (int)imager_down);
        ir_exp_cold_park_all();
    }
}

// Drop the array to ~2 uA. Returns false if the bus was busy and nothing was done.
// Used by session teardown and by the synchronous pre-sleep park.
static bool imager_park(void)
{
    if (!mlx90642_is_present()) {
        s_scl_hz = 0;
        return true; // Already down
    }

    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) != pdTRUE) {
        return false;
    }

    // The result is reported, not discarded: a refused sleep leaves the array at ~28 mA, so the caller must retry cold
    const esp_err_t r = mlx90642_sleep();
    mlx90642_deinit();
    xSemaphoreGive(xI2CBusMutex);

    if (r != ESP_OK) {
        ESP_LOGE(TAG, "Array refused the sleep command: %s", esp_err_to_name(r));
    }

    // Cleared even on a refusal: the handle is gone either way, and the other two sensors refuse a 0 Hz clock.
    s_scl_hz = 0;
    return r == ESP_OK;
}

void ir_exp_cold_park_all(void)
{
    if (i2c_bus_handle == NULL) {
        return;
    }

    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Bus busy; could not quiesce the expansion module");
        return;
    }

    // 100 kHz: nothing here needs speed, and the negotiated rate is unknown on every path that reaches this
    const esp_err_t a = mlx90642_park_cold(SCL_HZ_SLOW);
    const esp_err_t m = mlx90632_park_cold(SCL_HZ_SLOW);

    // The rangefinder has nothing to park at a cold boot (SW standby, microamps); this is for a teardown it refused,
    // which drops the handle and leaves it ranging at 19-21 mA. The write NACKs harmlessly when idle or absent.
    const esp_err_t t = vl53l4cx_park_cold(SCL_HZ_SLOW);
    (void)a; // All three are only read by the debug log below
    (void)m;
    (void)t;

    xSemaphoreGive(xI2CBusMutex);

#ifdef POLYCAST5_DEBUG
    if (a == ESP_OK || m == ESP_OK || t == ESP_OK) {
        ESP_LOGI(TAG, "Cold-parked the expansion module (array=%s spot=%s range=%s)",
                esp_err_to_name(a), esp_err_to_name(m), esp_err_to_name(t));
    }
#endif
}

static void ir_exp_task(void *arg)
{
    (void)arg;

    // The queues exist before this task does, so a START queued right after ir_exp_task_create() cannot race this point
    bool running = false;
    bool teardown = false;
    ir_exp_status_t st;
    memset(&st, 0, sizeof(st));
    st.dist_mm = VL53L4CX_NO_TARGET;
    st.medical_c100 = IR_EXP_MEDICAL_NONE;

    TickType_t next_frame = 0;
    TickType_t last_publish = 0;
    TickType_t next_medical = 0;
#ifdef POLYCAST5_DEBUG
    TickType_t dist_diag_next = 0;
#endif
    int16_t amb_c100 = INT16_MIN; // Latest ambient from the spot sensor
    int16_t recal_ref_c100 = INT16_MIN; // Ambient when the ToF last self-calibrated
    // Last good reading per sensor; 0 until one arrives. Nothing here has a presence pin and is_present() only tests the
    // handle, so a module pulled mid-session shows up only as reads that stop succeeding.
    TickType_t last_frame_tick = 0;
    TickType_t last_tof_tick = 0;
    TickType_t last_med_tick = 0;
    TickType_t session_tick = 0; // When the current session started, for the above

    while (1) {
        // Between sessions this task exists only long enough to be told to stop, so the idle block is unbounded.
        const TickType_t wait = running ? pdMS_TO_TICKS(LOOP_TICK_MS) : portMAX_DELAY;

        ir_exp_cmd_t cmd;
        if (xQueueReceive(xIrExpCtrlQueue, &cmd, wait) == pdTRUE) {
            ir_exp_done_t done;
            memset(&done, 0, sizeof(done));
            done.cmd = cmd.type;

            switch (cmd.type) {
            case IR_EXP_CMD_START:
                if (!running) {
                    session_start(&done);
                    running = done.ok;
                    
                    if (running) {
                        next_frame = xTaskGetTickCount();
                        last_publish = next_frame;
                        amb_c100 = INT16_MIN;
                        recal_ref_c100 = INT16_MIN;
                        last_frame_tick = 0;
                        last_tof_tick = 0;
                        last_med_tick = 0;
                        session_tick = next_frame;
                        memset(&st, 0, sizeof(st));
                        st.dist_mm = VL53L4CX_NO_TARGET;
                        st.medical_c100 = IR_EXP_MEDICAL_NONE;
                        // WARMING starts SET, cleared only by the first good frame: IMAGER alone would claim a live image
                        st.flags = (uint8_t)(IR_EXP_FLAG_WARMING |
                                             (done.imager ? IR_EXP_FLAG_IMAGER : 0) |
                                             (done.tof ? IR_EXP_FLAG_TOF : 0) |
                                             (done.medical ? IR_EXP_FLAG_MEDICAL : 0));
                    }
                } else { // Already streaming; report the live state so the page never blocks
                    done.imager = mlx90642_is_present();
                    done.tof = vl53l4cx_is_present();
                    done.medical = mlx90632_is_present();
                    done.scl_hz = s_scl_hz;
                    done.ok = true;
                }
                xQueueSend(xIrExpDoneQueue, &done, 0);
                break;

            case IR_EXP_CMD_STOP:
                if (running) {
                    session_stop();
                    running = false;
                } else {
                    // A failed start can still leave the array awake: the wake byte goes out before verification
                    ir_exp_cold_park_all();
                }
                done.ok = true;

                // Retired BEFORE the ack: the page runs the instant it reads it, and a re-entry must not see a producer on its way out
                s_task = NULL;

                // Only acknowledged once every sensor is parked - the page blocks on this
                xQueueSend(xIrExpDoneQueue, &done, 0);
                teardown = true; // Leaving the page frees the 8 kB stack
                break;

            case IR_EXP_CMD_PARK:
                // About to sleep, which stops the tick a deferred teardown would be measured in: park the array now
                if (running) {
                    session_stop(); // Also parks the ToF and the spot sensor
                    running = false;
                } else {
                    ir_exp_cold_park_all(); // Same failed-start case as STOP above
                }

                // Nothing left to park: session_stop() retried what it could not, the no-session branch cold-parks.
                // A second park pair here hits the "already down" early returns and reports success for a refusal.
                done.ok = true;
                s_task = NULL; // Before the ack, for the reason given under STOP
                xQueueSend(xIrExpDoneQueue, &done, 0);
                teardown = true; // The page is gone; do not hold the stack through sleep
                break;

            default:
                break;
            }

            if (teardown) {
                break; // Frees itself below
            }
        }

        if (!running) {
            continue;
        }

        const TickType_t now = xTaskGetTickCount();

        // From the rate the sensor reports, not the bus clock: the two disagree when a rate change was refused
        const TickType_t frame_period = pdMS_TO_TICKS(mlx90642_frame_period_ms());
        bool published = false;

        /* =============== Thermal frame =============== */
        if ((int32_t)(now - next_frame) >= 0) {
            if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
                bool ready = false;
                if (mlx90642_frame_ready(&ready) == ESP_OK && ready) {
                    if (process_frame(&st)) {
                        st.flags &= (uint8_t)~IR_EXP_FLAG_WARMING;
                        st.flags |= IR_EXP_FLAG_IMAGER; // Re-arm if a fault withdrew it
                        last_frame_tick = now;
                        next_frame = now + frame_period;
                        published = true;
                    } else {
                        st.flags |= IR_EXP_FLAG_WARMING;
                    }
                }
                xSemaphoreGive(xI2CBusMutex);
            }
        }

        /* =============== Rangefinder =============== */
        if (vl53l4cx_is_present()) {
            if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
                int32_t mm = VL53L4CX_NO_TARGET;
                const esp_err_t r = vl53l4cx_read_mm(&mm);
                // Taken inside the same bus window, while the driver's result block still holds this frame. A copy
                // out of driver RAM, so it costs the bus nothing and no other task is inside ST's unlocked state.
                const bool have_detail = (r == ESP_OK)
                        && (vl53l4cx_get_detail(&s_detail) == ESP_OK);
                xSemaphoreGive(xI2CBusMutex);
                if (have_detail && xIrExpTofDetailQueue != NULL) {
                    xQueueOverwrite(xIrExpTofDetailQueue, &s_detail);
                }
                if (r == ESP_OK) {
                    // Answered, target or not - that is what makes the reading current.
                    last_tof_tick = now;
                    st.flags |= IR_EXP_FLAG_TOF; // Re-arm if a fault withdrew it
                    if (mm != VL53L4CX_NO_TARGET) {
                        st.dist_mm = tof_push(mm);
#ifdef POLYCAST5_DEBUG
                        // Sensor reading beside the averaged value the panel shows: a display fault reads apart from a sensor fault
                        if ((int32_t)(xTaskGetTickCount() - dist_diag_next) >= 0) {
                            dist_diag_next = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
                            ESP_LOGI(TAG, "range: sensor %d mm -> panel %d mm",
                                    (int)mm, (int)st.dist_mm);
                        }
#endif
                    } else { // Failed the validity gate: show nothing rather than a guess
                        tof_reset();
                        st.dist_mm = VL53L4CX_NO_TARGET;
                    }
                }
            }
        }

        /* =============== Medical spot temperature =============== */
        // Throttled: the part only publishes a new pair about once a second, so polling every loop tick is bus chatter
        if (mlx90632_is_present() && (int32_t)(now - next_medical) >= 0) {
            next_medical = now + pdMS_TO_TICKS(MEDICAL_POLL_MS);
            if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
                float t_obj = 0.0f;
                float t_amb = 0.0f;
                const esp_err_t r = mlx90632_read_object(&t_obj, &t_amb);
                xSemaphoreGive(xI2CBusMutex);
                if (r == ESP_OK) {
                    st.medical_c100 = (int16_t)(t_obj * 100.0f);
                    amb_c100 = (int16_t)(t_amb * 100.0f);
                    last_med_tick = now;
                    st.flags |= IR_EXP_FLAG_MEDICAL; // Re-arm if a fault withdrew it
                }
            }
        }

        /* =============== Cancel range drift as ambient moves =============== */
        if (amb_c100 != INT16_MIN && vl53l4cx_is_present()) {
            if (recal_ref_c100 == INT16_MIN) {
                recal_ref_c100 = amb_c100;
            } else {
                int32_t d = (int32_t)amb_c100 - recal_ref_c100;
                if (d < 0) d = -d;
                if (d > TOF_RECAL_DELTA_C100) {
                    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(BUS_WAIT_MS)) == pdTRUE) {
                        (void)vl53l4cx_recalibrate();
                        xSemaphoreGive(xI2CBusMutex);
                    }
                    recal_ref_c100 = amb_c100;
                    tof_reset();
                }
            }
        }

        /* =============== Withdraw readings whose sensor has gone quiet =============== */
        // Clearing the flag is what reaches the user: the page renders "--" and holds the last image rather than showing
        // it as live. Each sensor runs on its own clock once it has answered, on the session clock until then - a zero
        // clock never expires, so a module unplugged between visits would sit in WARMING UP forever.
        const bool imager_gone = (last_frame_tick != 0)
                ? (int32_t)(now - last_frame_tick) >= (int32_t)pdMS_TO_TICKS(IMAGER_STALE_MS)
                : (int32_t)(now - session_tick) >= (int32_t)pdMS_TO_TICKS(FIRST_READING_MS);
        const bool tof_gone = (last_tof_tick != 0)
                ? (int32_t)(now - last_tof_tick) >= (int32_t)pdMS_TO_TICKS(TOF_STALE_MS)
                : (int32_t)(now - session_tick) >= (int32_t)pdMS_TO_TICKS(FIRST_READING_MS);
        const bool med_gone = (last_med_tick != 0)
                ? (int32_t)(now - last_med_tick) >= (int32_t)pdMS_TO_TICKS(MEDICAL_STALE_MS)
                : (int32_t)(now - session_tick) >= (int32_t)pdMS_TO_TICKS(FIRST_READING_MS);

        // Gated on the flag still being up, which also makes each test idempotent once withdrawn.
        if ((st.flags & IR_EXP_FLAG_IMAGER) && imager_gone) {
            st.flags &= (uint8_t)~IR_EXP_FLAG_IMAGER;
        }
        if ((st.flags & IR_EXP_FLAG_TOF) && tof_gone) {
            st.flags &= (uint8_t)~IR_EXP_FLAG_TOF;
            st.dist_mm = VL53L4CX_NO_TARGET;
        }
        if ((st.flags & IR_EXP_FLAG_MEDICAL) && med_gone) {
            st.flags &= (uint8_t)~IR_EXP_FLAG_MEDICAL;
            st.medical_c100 = IR_EXP_MEDICAL_NONE;
        }

        // FAULT tracks the ARRAY alone, as a pure function of its flag so it cannot latch on: a held image looks
        // exactly like a live one, while a quiet rangefinder or spot sensor already blanks its own value to "--".
        if ((st.flags & IR_EXP_FLAG_IMAGER) == 0) {
            st.flags |= IR_EXP_FLAG_FAULT;
        } else {
            st.flags &= (uint8_t)~IR_EXP_FLAG_FAULT;
        }

        /* =============== Publish =============== */
        if (s_imager_wake_tick != 0 &&
                (int32_t)(now - s_imager_wake_tick) < (int32_t)pdMS_TO_TICKS(IMAGER_STABILIZE_MS)) {
            st.flags |= IR_EXP_FLAG_STABILIZING;
        } else {
            st.flags &= (uint8_t)~IR_EXP_FLAG_STABILIZING;
        }

        // Every new frame, plus a slow heartbeat so range and spot still refresh while the array is warming up
        if (published || (int32_t)(now - last_publish) >= (int32_t)pdMS_TO_TICKS(HEARTBEAT_MS)) {
            st.seq = ++s_seq;
            xQueueOverwrite(xIrExpStatusQueue, &st);
            last_publish = now;
        }
    }

    // Reached only after an acknowledged STOP or PARK: sensors parked, bus mutex free, s_task already retired. The
    // queues stay alive so the page and the sleep path can always send; nothing below may touch state a re-entry owns.
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Session over, freeing the task (high water %u B)",
            (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    vTaskDelete(NULL);
}

// Called on every entry to the page, not once at boot: most units have no expansion module, and 8 kB is a lot to reserve
bool ir_exp_task_create(void)
{
    // Created here, not in the task, so a START queued the instant this returns cannot race the task's first statement.
    //
    // Each gated on its OWN handle. Gating all four on the status queue skipped the whole block on every later visit
    // once that one existed, so a queue that failed on the first visit could never be retried: the composite check
    // below kept failing and the page reported "did not respond" for the rest of the boot, with the queues that did
    // come up stranded.
    if (xIrExpStatusQueue == NULL) {
        xIrExpStatusQueue = xQueueCreate(1, sizeof(ir_exp_status_t));
    }
    if (xIrExpCtrlQueue == NULL) {
        xIrExpCtrlQueue = xQueueCreate(4, sizeof(ir_exp_cmd_t));
    }
    if (xIrExpDoneQueue == NULL) {
        xIrExpDoneQueue = xQueueCreate(2, sizeof(ir_exp_done_t));
    }
    // Separate from the status queue: 196 bytes against a couple of dozen, peeked into a stack local every frame
    if (xIrExpTofDetailQueue == NULL) {
        xIrExpTofDetailQueue = xQueueCreate(1, sizeof(ir_exp_tof_detail_t));
    }

    if (xIrExpStatusQueue == NULL || xIrExpCtrlQueue == NULL ||
            xIrExpDoneQueue == NULL || xIrExpTofDetailQueue == NULL) {
        ESP_LOGE(TAG, "Failed to create queues");
        return false;
    }

    // A task still up here is NOT one to reuse: every page exit stops the producer and waits for the ack, so this is
    // a teardown whose ack the page gave up on, and it will dequeue its STOP and delete itself, never the START.
    for (int i = 0; s_task != NULL && i < CREATE_EXIT_WAIT_MS / CREATE_EXIT_POLL_MS; ++i) {
        vTaskDelay(pdMS_TO_TICKS(CREATE_EXIT_POLL_MS));
    }
    if (s_task != NULL) {
        // Still there: the producer is wedged, likely on a bus a half-seated module is NACKing. Report it rather than queue a START nothing reads
        ESP_LOGE(TAG, "Previous session has not finished tearing down");
        return false;
    }

    // Both queues are emptied unconditionally before every spawn: a stale ack would satisfy this visit's START
    // handshake, and a leftover STOP would be the first thing the new task dequeues, tearing it back down.
    xQueueReset(xIrExpDoneQueue);
    xQueueReset(xIrExpCtrlQueue);

    // 8 kB: ST's driver runs histogram post-processing on THIS stack inside vl53l4cx_read_mm(), measured peak ~3.4 kB
    if (xTaskCreate(ir_exp_task, "ir_exp_task", 1024 * 8, NULL,
            POLYCAST5_PRIORITY_MEDIUM, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start ir_exp_task");
        s_task = NULL;
        return false;
    }

    return true;
}

bool ir_exp_task_is_running(void)
{
    return s_task != NULL;
}

// The park went unanswered. The caller is about to hold xI2CBusMutex for the whole sleep, so returning quietly would block
// the producer on its next take and leave the array measuring at ~28 mA all sleep. Likeliest cause is a bring-up inside
// ST's DataInit: give it a bounded chance to exit, then cold-park regardless.
static void park_timed_out(void)
{
    ESP_LOGE(TAG, "Expansion did not acknowledge the pre-sleep park");

    for (int i = 0; i < PARK_EXIT_WAIT_MS / PARK_EXIT_POLL_MS; ++i) {
        if (s_task == NULL) {
            break; // It finished and tore itself down; its own teardown parked the parts
        }
        vTaskDelay(pdMS_TO_TICKS(PARK_EXIT_POLL_MS));
    }

    if (s_task != NULL) {
        ESP_LOGE(TAG, "Producer still up; cold-parking the module from under it");
    }

    // Cheap and idempotent; a producer wedged holding the bus makes this give up after BUS_WAIT_MS and log
    ir_exp_cold_park_all();
}

void ir_exp_task_park(void)
{
    // No task means no session owns a handle, which is NOT the same as nothing being powered: a module merely plugged
    // in has both Melexis parts running from power-on, about 29 mA, with every is_present() reading false.
    if (s_task == NULL || xIrExpCtrlQueue == NULL || xIrExpDoneQueue == NULL) {
        ir_exp_cold_park_all();
        return;
    }

    // A live task with no handles is no proof either: a bring-up that failed partway leaves the array measuring at ~28 mA
    if (!mlx90642_is_present() && !vl53l4cx_is_present() && !mlx90632_is_present()) {
        ir_exp_cold_park_all();
        return;
    }

    ir_exp_cmd_t cmd = { .type = IR_EXP_CMD_PARK, .arg = 0 };
    if (xQueueSend(xIrExpCtrlQueue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        // A full command queue means a producer that is not draining it; the caller then sleeps holding the bus with the module measuring
        ESP_LOGE(TAG, "Could not queue the pre-sleep park; parking cold instead");
        park_timed_out();
        return;
    }

    // Wait for OUR acknowledgement: a late ack from a caller that gave up would let the bus mutex go mid-teardown
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PARK_ACK_WAIT_MS);
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - deadline) >= 0) {
            park_timed_out();
            return;
        }

        ir_exp_done_t done;
        if (xQueueReceive(xIrExpDoneQueue, &done, deadline - now) != pdTRUE) {
            park_timed_out();
            return;
        }
        if (done.cmd == IR_EXP_CMD_PARK) {
            return;
        }
    }
}
