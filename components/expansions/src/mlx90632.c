#include "ir_exp.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "TCA9535.h" // i2c_bus_handle
#include "polycast5_macros.h" // POLYCAST5_DEBUG

#define TAG "MLX90632"

#define I2C_TIMEOUT_MS 100

// EEPROM calibration constants. These are WORD addresses - 0x4003/0x4004/0x4005 are consecutive - while the MLX90642 on the
// same module uses BYTE addresses stepping by 2, so never share a helper between the two drivers.
// 32-bit values occupy two words, least significant word first.
#define MLX_EE_PRODUCT_CODE 0x2409
// The datasheet names 0x240B in its memory map but never documents the word's contents - it does not mention a
// DSP revision at all. The low-byte-is-the-revision reading comes from the two reference drivers, which agree:
// Melexis's own mlx90632-library (MLX90632_EE_VERSION / MLX90632_DSPv5) and the Linux IIO driver
// (MLX90632_DSP_MASK GENMASK(7,0), MLX90632_DSP_VERSION 5).
#define MLX_EE_VERSION      0x240B // EEPROM version word; low byte is the DSP revision
#define MLX_DSP_V5          0x05   // The revision the section 11.1 maths is written against
#define MLX_EE_P_R          0x240C // 32-bit
#define MLX_EE_P_G          0x240E // 32-bit
#define MLX_EE_P_T          0x2410 // 32-bit
#define MLX_EE_P_O          0x2412 // 32-bit
#define MLX_EE_EA           0x2424 // 32-bit
#define MLX_EE_EB           0x2426 // 32-bit
#define MLX_EE_FA           0x2428 // 32-bit
#define MLX_EE_FB           0x242A // 32-bit
#define MLX_EE_GA           0x242C // 32-bit
#define MLX_EE_GB           0x242E // 16-bit
#define MLX_EE_KA           0x242F // 16-bit
#define MLX_EE_HA           0x2481 // 16-bit, customer calibration
#define MLX_EE_HB           0x2482 // 16-bit, customer calibration

// Volatile registers
#define MLX_REG_CONTROL     0x3001 // Measurement mode; POR-loaded from EEPROM
#define MLX_REG_STATUS      0x3FFF // new_data / cycle_pos / brown_out
#define MLX_RAM_4           0x4003 // RAM_4 .. RAM_9 are six consecutive words

// MLX_REG_CONTROL fields
#define MLX_CTRL_MODE_MASK      (3u << 1)
#define MLX_CTRL_MODE_SLEEP_STEP (1u << 1) // Sleeping step: ~1.5 uA
#define MLX_CTRL_MODE_CONTINUOUS (3u << 1) // Free-running: ~1 mA
#define MLX_CTRL_MEAS_SEL_MASK  (0x1Fu << 4) // 0 = medical measurement

// MLX_REG_STATUS fields
#define MLX_STAT_NEW_DATA       (1u << 0)
#define MLX_STAT_CYCLE_POS_MASK (0x1Fu << 2)
#define MLX_STAT_CYCLE_POS_SHIFT 2
#define MLX_STAT_BROWN_OUT      (1u << 8)

// Object emissivity. Not stored on-chip, so it is the application's to choose, and it divides straight into the
// object-temperature solve. An assumed 1.0 against real skin at 0.98 reads about 0.16 C LOW at body temperature
// with the die near 28 C, and over 0.3 C low in a cool room - the bias grows as the die cools away from the
// object. This is the -DCB medical part, calibrated +/-0.2 C over 35-42 C, so at room temperature the old 1.0
// exceeded the entire spec budget on its own.
#define MLX_EMISSIVITY 0.98f // Human skin; flat at 0.98 across this part's 2-14 um passband (Steketee 1973)

// Every function in this file requires the caller to hold xI2CBusMutex.

// Per-device handle on the shared bus; NULL until init succeeds
static i2c_master_dev_handle_t s_dev = NULL;

// Cached factory calibration, already scaled into engineering units
static struct {
    float P_R, P_G, P_T, P_O;
    float Ea, Eb, Fa, Fb, Ga;
    float Gb, Ka, Ha, Hb;
} s_cal;

// Medical mode alternates between two measurement cycles and the ambient slots come from
// different ones, so no object temperature is valid until both have been observed once.
static uint8_t s_seen_cycles = 0;

// Read one 16-bit word. Address goes out MSByte first, as does the data.
static esp_err_t mlx_read_word(uint16_t reg, uint16_t *out)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    uint8_t buf[2];
    esp_err_t ret = i2c_master_transmit_receive(s_dev, addr, sizeof(addr), buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        *out = ((uint16_t)buf[0] << 8) | buf[1];
    }
    return ret;
}

// Read n consecutive words into a caller buffer (the device auto-increments).
static esp_err_t mlx_read_words(uint16_t reg, uint16_t *out, size_t n)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    uint8_t buf[16];
    if (n * 2 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = i2c_master_transmit_receive(s_dev, addr, sizeof(addr), buf, n * 2, I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        for (size_t i = 0; i < n; ++i) {
            out[i] = ((uint16_t)buf[i * 2] << 8) | buf[i * 2 + 1];
        }
    }
    return ret;
}

// Write one 16-bit volatile register. Never used on EEPROM addresses - this part is rated for
// only ten EEPROM re-writes in its whole life.
static esp_err_t mlx_write_word(uint16_t reg, uint16_t val)
{
    uint8_t tx[4] = {
        (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
        (uint8_t)(val >> 8), (uint8_t)(val & 0xFF),
    };
    return i2c_master_transmit(s_dev, tx, sizeof(tx), I2C_TIMEOUT_MS);
}

// Read a 32-bit constant. The LOW half sits at the lower address, so the two words are swapped
// relative to the obvious big-endian reading.
static esp_err_t mlx_read_i32(uint16_t reg, int32_t *out)
{
    uint16_t w[2];
    esp_err_t ret = mlx_read_words(reg, w, 2);
    if (ret == ESP_OK) {
        *out = (int32_t)(((uint32_t)w[1] << 16) | w[0]); // w[0] = LSW, w[1] = MSW
    }
    return ret;
}

// Fetch every factory constant and scale it into engineering units. The exponents come from datasheet 11.1;
// each raw value is a signed integer stored pre-multiplied by a power of two.
static esp_err_t mlx_read_cal(void)
{
    int32_t v32;
    uint16_t v16;
    esp_err_t ret;

    // 32-bit constants
    if ((ret = mlx_read_i32(MLX_EE_P_R, &v32)) != ESP_OK) return ret;
    s_cal.P_R = ldexpf((float)v32, -8);
    if ((ret = mlx_read_i32(MLX_EE_P_G, &v32)) != ESP_OK) return ret;
    s_cal.P_G = ldexpf((float)v32, -20);
    if ((ret = mlx_read_i32(MLX_EE_P_T, &v32)) != ESP_OK) return ret;
    s_cal.P_T = ldexpf((float)v32, -44);
    if ((ret = mlx_read_i32(MLX_EE_P_O, &v32)) != ESP_OK) return ret;
    s_cal.P_O = ldexpf((float)v32, -8);
    if ((ret = mlx_read_i32(MLX_EE_EA, &v32)) != ESP_OK) return ret;
    s_cal.Ea = ldexpf((float)v32, -16);
    if ((ret = mlx_read_i32(MLX_EE_EB, &v32)) != ESP_OK) return ret;
    s_cal.Eb = ldexpf((float)v32, -8);
    if ((ret = mlx_read_i32(MLX_EE_FA, &v32)) != ESP_OK) return ret;
    s_cal.Fa = ldexpf((float)v32, -46);
    if ((ret = mlx_read_i32(MLX_EE_FB, &v32)) != ESP_OK) return ret;
    s_cal.Fb = ldexpf((float)v32, -36);
    if ((ret = mlx_read_i32(MLX_EE_GA, &v32)) != ESP_OK) return ret;
    s_cal.Ga = ldexpf((float)v32, -36);

    // 16-bit constants
    if ((ret = mlx_read_word(MLX_EE_GB, &v16)) != ESP_OK) return ret;
    s_cal.Gb = ldexpf((float)(int16_t)v16, -10);
    if ((ret = mlx_read_word(MLX_EE_KA, &v16)) != ESP_OK) return ret;
    s_cal.Ka = ldexpf((float)(int16_t)v16, -10);

    // Customer calibration words. Some parts ship with these blank, where the identity transform
    // (Ha = 1.0 in Q14, Hb = 0) is the right substitute.
    if ((ret = mlx_read_word(MLX_EE_HA, &v16)) != ESP_OK) return ret;
    s_cal.Ha = (v16 == 0) ? 1.0f : ldexpf((float)(int16_t)v16, -14);
    if ((ret = mlx_read_word(MLX_EE_HB, &v16)) != ESP_OK) return ret;
    s_cal.Hb = (v16 == 0xFFFF) ? 0.0f : ldexpf((float)(int16_t)v16, -10);

    // Both appear as divisors, so a zero here means the constants did not read back correctly
    // (most often a byte-order mistake) rather than a legitimate calibration.
    if (s_cal.P_G == 0.0f || s_cal.Ea == 0.0f) {
        ESP_LOGE(TAG, "Implausible calibration (P_G=%.4f Ea=%.4f) - check word order",
                (double)s_cal.P_G, (double)s_cal.Ea);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t mlx90632_init(uint32_t scl_hz)
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
        .device_address = MLX90632_I2C_ADDR,
        .scl_speed_hz = scl_hz,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        s_dev = NULL;
        return ret;
    }

    // Product code is logged for diagnosis but not gated on: the field layout is ambiguous enough that a strict
    // match risks rejecting a good part. A successful, plausible calibration read is the real presence test.
    uint16_t product = 0;
    ret = mlx_read_word(MLX_EE_PRODUCT_CODE, &product);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Product code read failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    // The DSP revision IS gated on, unlike the product code above: every formula in mlx90632_read_object() is
    // written against section 11.1, which is DSPv5-specific. A different revision would still read, still produce
    // a plausible number, and still be wrong - which on a readout labelled medical is worse than showing nothing.
    //
    // Only the low byte is tested, matching both reference drivers. The high byte carries the variant - 0x01
    // medical, 0x02 consumer, 0x05 extended-range - and every one of them reports 0x05 in the low byte, so this
    // cannot reject a genuine part. The whole word is logged so a rejection is one line to diagnose.
    uint16_t ee_version = 0;
    ret = mlx_read_word(MLX_EE_VERSION, &ee_version);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "EEPROM version read failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    if ((ee_version & 0x00FF) != MLX_DSP_V5) {
        ESP_LOGE(TAG, "Unsupported DSP revision (EE_VERSION 0x%04X); the medical maths needs v%u",
                ee_version, MLX_DSP_V5);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }

    ret = mlx_read_cal();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Calibration read failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    // Flag the part as running so a later brown-out shows up as this bit going back to 0
    uint16_t status = 0;
    if (mlx_read_word(MLX_REG_STATUS, &status) == ESP_OK) {
        (void)mlx_write_word(MLX_REG_STATUS, (uint16_t)(status | MLX_STAT_BROWN_OUT));
    }

    // Do not assume the power mode. The datasheet calls continuous the default, but the control register is
    // loaded from EEPROM at power-on and parts have shipped in sleeping-step, which produces no measurements.
    ret = mlx90632_set_continuous(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to select continuous mode: %s", esp_err_to_name(ret));
        goto fail;
    }

    s_seen_cycles = 0;

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "MLX90632 up at %lu Hz (product 0x%04X, EE_VERSION 0x%04X)",
            (unsigned long)scl_hz, product, ee_version);
#endif
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return ret;
}

esp_err_t mlx90632_park_cold(uint32_t scl_hz)
{
    // Not mlx90632_init(): that forces continuous mode, dragging ~1.5 uA up to ~1 mA every time the device sleeps. REG_CONTROL is
    // volatile and POR-loaded from EE_CONTROL, so the cold-boot mode is per-unit - writing sleeping-step blind is the only way to be sure.
    if (s_dev != NULL) { // A session owns the handle; use the ordinary path
        return mlx90632_set_continuous(false);
    }

    if (i2c_bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MLX90632_I2C_ADDR,
        .scl_speed_hz = scl_hz,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    // Borrow the handle for the read-modify-write, then hand it back. s_dev stays NULL so mlx90632_is_present() keeps meaning "a session owns it".
    s_dev = dev;
    ret = mlx90632_set_continuous(false);
    s_dev = NULL;

    i2c_master_bus_rm_device(dev);
    return ret;
}

void mlx90632_deinit(void)
{
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    s_seen_cycles = 0;
}

bool mlx90632_is_present(void)
{
    return s_dev != NULL;
}

esp_err_t mlx90632_set_continuous(bool on)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t ctrl = 0;
    esp_err_t ret = mlx_read_word(MLX_REG_CONTROL, &ctrl);
    if (ret != ESP_OK) {
        return ret;
    }

    // Preserve the reserved bits, force medical measurement (meas_select 0) and set the power mode. Volatile register - nothing touches EEPROM.
    ctrl &= (uint16_t)~(MLX_CTRL_MODE_MASK | MLX_CTRL_MEAS_SEL_MASK);
    ctrl |= on ? MLX_CTRL_MODE_CONTINUOUS : MLX_CTRL_MODE_SLEEP_STEP;

    ret = mlx_write_word(MLX_REG_CONTROL, ctrl);
    if (ret == ESP_OK && !on) {
        s_seen_cycles = 0; // Next wake starts the two-cycle warm-up again
    }
    return ret;
}

esp_err_t mlx90632_read_object(float *t_obj_c, float *t_amb_c)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t status = 0;
    esp_err_t ret = mlx_read_word(MLX_REG_STATUS, &status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!(status & MLX_STAT_NEW_DATA)) { // Nothing new since the last read
        return ESP_ERR_NOT_FINISHED;
    }

    // cycle_pos says which measurement of the table just landed, and therefore which pair of object samples is the fresh one.
    uint8_t cycle = (uint8_t)((status & MLX_STAT_CYCLE_POS_MASK) >> MLX_STAT_CYCLE_POS_SHIFT);

    uint16_t ram[6]; // RAM_4 .. RAM_9
    ret = mlx_read_words(MLX_RAM_4, ram, 6);
    if (ret != ESP_OK) {
        return ret;
    }

    // Acknowledge the sample so the device can flag the next one
    (void)mlx_write_word(MLX_REG_STATUS, (uint16_t)(status & ~MLX_STAT_NEW_DATA));

    const float RAM_4 = (float)(int16_t)ram[0];
    const float RAM_5 = (float)(int16_t)ram[1];
    const float RAM_6 = (float)(int16_t)ram[2]; // Ambient, always from measurement 1
    const float RAM_7 = (float)(int16_t)ram[3];
    const float RAM_8 = (float)(int16_t)ram[4];
    const float RAM_9 = (float)(int16_t)ram[5]; // Ambient, always from measurement 2

    // The object pair alternates with cycle_pos; the two ambient slots above do not.
    float S;
    if (cycle == 1) {
        S = (RAM_4 + RAM_5) / 2.0f;
        s_seen_cycles |= 0x01;
    } else if (cycle == 2) {
        S = (RAM_7 + RAM_8) / 2.0f;
        s_seen_cycles |= 0x02;
    } else { // Anything else is not a valid medical-mode cycle position
        return ESP_ERR_NOT_FINISHED;
    }

    // Ambient needs RAM_6 and RAM_9, from different measurements, so nothing is computable until both cycles have been seen (about a second).
    if (s_seen_cycles != 0x03) {
        return ESP_ERR_NOT_FINISHED;
    }

    // Maths below is transcribed from datasheet 11.1 (medical measurement); the worked example in 11.1.4 is the acceptance
    // test for it - its constants and RAM values must come out at Ta = 28.4 C and To = 27.2 C.

    // Ambient temperature (datasheet 11.1.1.1 and 11.1.2)
    const float ram6_12 = RAM_6 / 12.0f;
    const float VR_TA = RAM_9 + s_cal.Gb * ram6_12;
    if (VR_TA == 0.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const float AMB = (ram6_12 / VR_TA) * 524288.0f; // 2^19
    const float dAMB = AMB - s_cal.P_R;
    const float Ta = s_cal.P_O + dAMB / s_cal.P_G + s_cal.P_T * dAMB * dAMB;

    // Object temperature (datasheet 11.1.1.2 and 11.1.3)
    const float VR_TO = RAM_9 + s_cal.Ka * ram6_12;
    if (VR_TO == 0.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const float S_TO = ((S / 12.0f) / VR_TO) * 524288.0f;

    // Eb here, not P_R - the two are equal in the datasheet worked example, which makes a mix-up easy to miss.
    const float TA_DUT = (AMB - s_cal.Eb) / s_cal.Ea + 25.0f;
    const float Ta_K = TA_DUT + 273.15f;
    const float Ta_K4 = Ta_K * Ta_K * Ta_K * Ta_K;

    // To appears on both sides of its own equation, so it is solved by iteration seeded at 25 C; the datasheet states three passes suffice.
    float TO_DUT = 25.0f;
    for (int i = 0; i < 3; ++i) {
        const float denom = MLX_EMISSIVITY * s_cal.Fa * s_cal.Ha *
                (1.0f + s_cal.Ga * (TO_DUT - 25.0f) + s_cal.Fb * (TA_DUT - 25.0f));
        if (denom == 0.0f) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const float x = S_TO / denom + Ta_K4;
        if (x <= 0.0f) { // Fourth root of a non-positive number: sample is unusable
            return ESP_ERR_INVALID_RESPONSE;
        }
        TO_DUT = sqrtf(sqrtf(x)) - 273.15f - s_cal.Hb; // sqrt of sqrt beats powf(x, 0.25)
    }

    if (t_obj_c) *t_obj_c = TO_DUT;
    if (t_amb_c) *t_amb_c = Ta;

    return ESP_OK;
}
