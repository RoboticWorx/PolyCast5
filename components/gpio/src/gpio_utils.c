#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "polycast5_macros.h"
#include "polycast5_gpios.h"

#include "portmacro.h"

#include "nvs_flash.h"
#include "esp_partition.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "tca9535.h"
#include "lis2dh12.h"
#include "mmc5603.h"

#include "gpio_utils.h"
#include "gpio_task.h"

#define TAG "GPIO_UTILS"

#define ADC_CH ADC_CHANNEL_4
#define NUM_ADC_SAMPLES 16384

volatile int16_t rbg_blink_period_ms = 25; // Default rgb period
volatile int16_t rgb_blink_total_ms = 125; // Default rgb total

static TimerHandle_t rgb_blink_timer;
static TimerHandle_t rgb_blink_stop_timer;
static uint8_t rgb_blink_color;
static bool rgb_blink_state;

static TimerHandle_t haptic_timer;

static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t cali_handle = NULL;

void gpio_utils_init_nvs(void)
{
    // Notice that release mode encryption and secure boot are intentionally disabled by default
#if !defined(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE) || !defined(CONFIG_SECURE_BOOT_V2_ENABLED)
    ESP_LOGE("", "!!! READ THIS !!!");
    ESP_LOGW("", "Release mode encryption and secure boot are DISABLED!");
    ESP_LOGW("", "This is so that you are able to freely modify/re-flash the code that this device is running.");
    ESP_LOGW("", "If you don't plan to modify the code, release mode encryption and secure boot should be enabled to prevent physical access attacks. A tutorial to enable this is available at polycast5.com/blogs/docs/lock-it-down.");
    ESP_LOGW("", "It's important to note that if you do this, you will be UNABLE to re-flash the device again afterwards.");
#endif

    // This enables development mode encryption for passive protection while still allowing re-flashing
#if CONFIG_NVS_ENCRYPTION
    const esp_partition_t *key_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);
    assert(key_part && "nvs_keys partition missing from partition table!");

    nvs_sec_cfg_t cfg;
    esp_err_t ret = nvs_flash_read_security_cfg(key_part, &cfg);
    if (ret == ESP_ERR_NVS_KEYS_NOT_INITIALIZED || ret == ESP_ERR_NVS_CORRUPT_KEY_PART) {
        ESP_ERROR_CHECK(nvs_flash_generate_keys(key_part, &cfg));
    } else {
        ESP_ERROR_CHECK(ret);
    }

    // First boot after enabling encryption will find unreadable plaintext NVS pages and erase the partition.
    // Equivalent to factory reset!
    ret = nvs_flash_secure_init(&cfg);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_secure_init(&cfg);
    }
    ESP_ERROR_CHECK(ret); // Catch any unhandled init failure (XTS decrypt, key mismatch, etc.)
#else
    ESP_ERROR_CHECK(nvs_flash_init());
    #error Development CONFIG_NVS_ENCRYPTION should be enabled! (Separate from release mode encryption noted above.)
#endif

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "NVS initialized");
#endif
}

static void haptic_off_cb(TimerHandle_t xTimer)
{
    gpio_utils_write_output(TCA9535_HAPTIC_PIN, 0);
}

// Called every RGB_BLINK_PERIOD_MS to toggle the LED
static void rgb_blink_cb(TimerHandle_t xTimer)
{
    rgb_blink_state = !rgb_blink_state;
    // Turn the LEDs on or off based on rgb_blink_color + state
    switch(rgb_blink_color) {
        case RGB_SET_RED:
            gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, rgb_blink_state);
            break;
        
        case RGB_SET_GREEN:
            gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, rgb_blink_state);
            break;
            
        case RGB_SET_BLUE:
            gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, rgb_blink_state);
            break;
            
        case RGB_SET_PURPLE:
            gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, rgb_blink_state);
            gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, rgb_blink_state);
            break;
            
        case RGB_SET_TEAL:
            gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, rgb_blink_state);
            gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, rgb_blink_state);
            break;
            
        default:
            break;
    }
}
// Called once after RGB_BLINK_TOTAL_MS to stop blinking
static void rgb_blink_stop_cb(TimerHandle_t xTimer)
{
    // Stop the periodic toggle (0 timeout: timer callbacks must not block on their own queue)
    xTimerStop(rgb_blink_timer, 0);

    // Ensure all LEDs off
    gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, 0);
    gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, 0);
    gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 0);
}

static void init_ledc_pwm(void)
{
    // Configure LEDC timer
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_LEDC_RESOLUTION,
        .timer_num = LCD_LEDC_TIMER,
        .freq_hz = LCD_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_USE_XTAL_CLK
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s\n", esp_err_to_name(err));
        return;
    }

    // Configure LEDC channel
    ledc_channel_config_t channel_config = {
        .gpio_num = ST7789_LEDA_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE, // No interrupts needed
        .timer_sel = LCD_LEDC_TIMER,
        .duty = 100, // Start with 100% duty (ON)
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags.output_invert = 1 // P-CH inversion
    };
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s\n", esp_err_to_name(err));
        return;
    }
}

esp_err_t gpio_utils_init(void)
{
    esp_err_t ret = TCA9535Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA9535Init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // TCA9535 configuration register bit meaning:
    // 1 = input
    // 0 = output

    // Assert the 3V3_EN power latch value and make Port1 drive it before the
    // Port0 config below can early-return: on battery the device powers itself
    // off if P12 is left undriven once the power-button bootstrap releases
    ret = gpio_utils_write_output(TCA9535_3V3_EN_PIN, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to preload 3V3_EN high: %s", esp_err_to_name(ret));
        return ret;
    }

    // Port1: all outputs
    // bit7 bit6 bit5 bit4 bit3 bit2 bit1 bit0
    //    0    0    0    0    0    0    0    0    = 0x00
    ret = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG1, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure Port1 outputs: %s", esp_err_to_name(ret));
        return ret;
    }

    // Port0 = all inputs (0xFF)
    ret = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0, 0xFF);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config0 write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure outputs
    gpio_config_t io_conf_out = {
        .pin_bit_mask = (1ULL << ST7789_LEDA_PIN) |
                        (1ULL << ST7789_DC_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf_out);
    
    // Default states
    gpio_set_level(ST7789_LEDA_PIN, LCD_BL_STATE_ON); // LCD BL high
    gpio_utils_write_output(TCA9535_HAPTIC_PIN, 0); // Haptic motor low
    gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, 0); // Red LED off
    gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, 0); // Green LED off
    gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 0); // Blue LED off
    gpio_utils_write_output(TCA9535_TSOP_EN_PIN, 1); // TSOP OFF (active low)
    
    // Configure inputs
    /*gpio_config_t io_conf_in = {
        .pin_bit_mask = (1ULL << ADC_PIN),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&io_conf_in);*/
    
    // Configure interrupts
    /*gpio_config_t io_conf_int = {
        .pin_bit_mask = (1ULL << TCA9535_USER_BUTTON_POWER_PIN),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_NEGEDGE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf_int);*/

    // ISR service
    gpio_install_isr_service(0);
    //gpio_isr_handler_add(TCA9535_INT_PIN, tca9535_int_isr, NULL);    
    
    init_ledc_pwm();
    
    // Create a timer for the haptic motor
    haptic_timer = xTimerCreate("haptic_off", pdMS_TO_TICKS(10), pdFALSE, NULL, haptic_off_cb);
    configASSERT(haptic_timer);
    
    // No need to use xRgbLedMutex since this function is called from main
    // Create the periodic blink timer
    rgb_blink_timer = xTimerCreate("rgb_blink", pdMS_TO_TICKS(rbg_blink_period_ms), pdTRUE, NULL, rgb_blink_cb);
    configASSERT(rgb_blink_timer);

    // Create the one-shot blink stop timer
    rgb_blink_stop_timer = xTimerCreate("rgb_blink_stop", pdMS_TO_TICKS(rgb_blink_total_ms), pdFALSE, NULL, rgb_blink_stop_cb);
    configASSERT(rgb_blink_stop_timer);

    // Bring up the LIS2DH12 accelerometer on the same I2C bus (non-fatal if absent)
    esp_err_t accel_ret = lis2dh12_init();
    if (accel_ret != ESP_OK) {
        ESP_LOGE(TAG, "lis2dh12_init failed: %s", esp_err_to_name(accel_ret));
    }

    // Bring up the MMC5603 magnetometer on the same I2C bus (non-fatal if absent)
    esp_err_t mag_ret = mmc5603_init();
    if (mag_ret != ESP_OK) {
        ESP_LOGE(TAG, "mmc5603_init failed: %s", esp_err_to_name(mag_ret));
    }

    return ret;
}

int gpio_utils_read_input(uint8_t pin)
{
    if (pin > 7) {
        ESP_LOGE(TAG, "Invalid input pin %d", pin);
        return -1;
    }
    
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY); // Lock I2C bus
    uint8_t inputs = 0xFF; // Default: all released (active-low buttons)
    esp_err_t err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0, &inputs);
    xSemaphoreGive(xI2CBusMutex); // Release I2C bus

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_utils_read_input: I2C read failed: %s", esp_err_to_name(err));
        return 1; // Default: released (active-low)
    }

    return (inputs >> pin) & 0x1;
}

esp_err_t gpio_utils_write_output(uint8_t pin, bool level)
{
    if (pin > 7) {
        ESP_LOGE(TAG, "Invalid output pin %d", pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY); // Lock I2C bus

    uint8_t out = 0;
    esp_err_t err = TCA9535ReadSingleRegister(TCA9535_OUTPUT_REG1, &out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_utils_write_output: I2C read failed for pin %d: %s", pin, esp_err_to_name(err));
        xSemaphoreGive(xI2CBusMutex); // Release I2C bus
        return err;
    }

    if (level) {
        out |= (1 << pin);
    } else {
        out &= ~(1 << pin);
    }

    err = TCA9535WriteSingleRegister(TCA9535_OUTPUT_REG1, out);
    xSemaphoreGive(xI2CBusMutex); // Release I2C bus
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_utils_write_output: Failed to write output pin %d: %s", pin, esp_err_to_name(err));
    }

    return err;
}

void gpio_utils_init_battery_adc(void)
{
    // Init one-shot ADC
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc1_handle));

    // Configure ADC channel
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CH, &chan_cfg));

#ifdef POLYCAST5_DEBUG_ADC
    ESP_LOGI(TAG, "ADC initialized");
#endif
    
    // Configure curve fitting
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cfg, &cali_handle));
}

void gpio_utils_deinit_battery_adc(void)
{
    // Deinit cali_handle
    if (cali_handle) {
        ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(cali_handle));
        cali_handle = NULL;
    }
    
    // Deinit adc1_handle
    if (adc1_handle) {
        ESP_ERROR_CHECK(adc_oneshot_del_unit(adc1_handle));
        adc1_handle = NULL;
    }
}

void gpio_utils_en_tsop_receiver(bool enable)
{
    // TSOP_EN_PIN is active low
    gpio_utils_write_output(TCA9535_TSOP_EN_PIN, !enable);
}

// Undo divider + op amp: Vbat = (Vadc + off) / gain
static float battery_vadc_to_vbat(float Vadc)
{
    // VIN+ divider: V+ = Vbat * (R36+R37+R38+R39)/(R35+R36+R37+R38+R39)
    const float R35 = 100000, R36 = 100000, R37 = 100000, R38 = 10000, R39 = 10000;
    const float d = (R36 + R37 + R38 + R39) / (R35 + R36 + R37 + R38 + R39); // 0.6875

    // Reference node: 3V3 through R28+R29, R34 to GND
    const float R28 = 10000, R29 = 10000, R34 = 100000;
    const float Vref = 3.30f * (R34 / (R28 + R29 + R34)); // 2.75V

    // Difference-amp gain: k = Rf/Rin = (R31+R32+R33)/R30
    const float R30 = 100000, R31 = 100000, R32 = 24, R33 = 24;
    const float k = (R31 + R32 + R33) / R30; // ~1.0005

    const float gain = (1.0f + k) * d; // ~1.3753
    const float off = k * Vref;        // ~2.7513

    return (Vadc + off) / gain;
}

float gpio_utils_get_battery_voltage(void)
{
    uint32_t sum = 0;
    
    // Average readings
    uint32_t valid_samples = 0;
    for (int i = 0; i < NUM_ADC_SAMPLES; i++) {
        int raw = 0;
        esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_CH, &raw);
        if (ret != ESP_OK) {
#ifdef POLYCAST5_DEBUG_ADC
            ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(ret));
#endif
            continue; // Skip this sample
        }
        sum += raw;
        valid_samples++;
        esp_rom_delay_us(5);

        // Yield to task_wdt
        if (i % 50 == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    if (valid_samples == 0) {
        ESP_LOGE(TAG, "All ADC samples failed");
        return 0.0f;
    }
    
    int avg_raw = sum / valid_samples;
    
#ifdef POLYCAST5_DEBUG_ADC
    ESP_LOGI(TAG, "Raw battery reading: %d", avg_raw);
#endif
    
    // Get pin mV
    int pin_mv = 0;
    esp_err_t ret = adc_cali_raw_to_voltage(cali_handle, avg_raw, &pin_mv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC calibration failed: %s", esp_err_to_name(ret));
        return 0.0f;
    }
    
    float Vadc = pin_mv / 1000.0f; // Convert to volts
    
#ifdef POLYCAST5_DEBUG_ADC
    ESP_LOGI(TAG, "Raw voltage reading: %f", Vadc);
#endif
    
    //return Vadc;

    return battery_vadc_to_vbat(Vadc);
}

float gpio_utils_battery_selftest_voltage(void)
{
    // Short standalone burst for the boot hardware self-test
    // Runs before the gpio/adc tasks exist, so briefly owning the ADC handles here is safe
    gpio_utils_init_battery_adc();

    uint32_t sum = 0;
    uint32_t valid_samples = 0;
    for (int i = 0; i < 64; i++) {
        int raw = 0;
        if (adc_oneshot_read(adc1_handle, ADC_CH, &raw) == ESP_OK) {
            sum += raw;
            valid_samples++;
        }
        esp_rom_delay_us(5);
    }

    float vbat = 0.0f;
    if (valid_samples > 0) {
        int pin_mv = 0;
        if (adc_cali_raw_to_voltage(cali_handle, sum / valid_samples, &pin_mv) == ESP_OK) {
            vbat = battery_vadc_to_vbat(pin_mv / 1000.0f);
        }
    }

    gpio_utils_deinit_battery_adc();
    return vbat;
}

uint8_t gpio_utils_volts_to_soc(float voltage)
{
    voltage += 0.13; // Offset max error high (users want to see it at 100%)

    // Typical LiPo discharge curve, but the voltages are this board's MEASURED (ADC) readings rather than true cell voltage
    static const struct { float volt; uint8_t soc; } soc_table[] = { // {measured V, %}
        {4.20, 100}, {4.15, 95},  {4.11, 90},  {4.08, 85},  {4.02, 80},
        {3.98, 75},  {3.95, 70},  {3.91, 65},  {3.87, 60},  {3.85, 55},
        {3.84, 50},  {3.82, 45},  {3.80, 40},  {3.79, 35},  {3.77, 30},
        {3.75, 25},  {3.73, 20},  {3.69, 15},  {3.61, 10},  {3.50, 5},
        {3.27, 0},
    };
    const int n = sizeof(soc_table) / sizeof(soc_table[0]);

    // Clamp above full / below empty (empty sits just above BATT_CUTOFF_VBAT)
    if (voltage >= soc_table[0].volt) {
        return 100;
    }
    // Clamp at min
    if (voltage <= soc_table[n - 1].volt) {
        return 1;
    }

    // Interpolate along the curve
    for (int i = 0; i < n - 1; i++) {
        float v_hi = soc_table[i].volt;
        float v_lo = soc_table[i + 1].volt;
        // If 'voltage' lives between them
        if (voltage >= v_lo && voltage <= v_hi) {
            // Get neighboring SoC
            uint8_t soc_hi = soc_table[i].soc;
            uint8_t soc_lo = soc_table[i + 1].soc;
            
            // Linearly interpolate the value (map)
            float soc_f = (voltage - v_lo) * (soc_hi - soc_lo) / (v_hi - v_lo) + soc_lo;
            
            // Round to nearest % and clamp to [1, 100]
            if (soc_f < 1.0f) {
                soc_f = 1.0f;
            }
            return (uint8_t)(soc_f + 0.5f);
        }
    }

    return 1; // Fallback
}

void gpio_utils_spin_haptic(uint32_t ms)
{
    // Cap
    if (ms < HAPTIC_MIN_MS) {
        ms = HAPTIC_MIN_MS;
    }
    //else if (ms > HAPTIC_MAX_MS) {
    //    ms = HAPTIC_MAX_MS;
    //}
    
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0) {
        ticks = 1; // Can't be 0
    }
    
    // Stop first so a pending expiry from a previous buzz can't fire OFF around the ON write
    xTimerStop(haptic_timer, portMAX_DELAY);

    gpio_utils_write_output(TCA9535_HAPTIC_PIN, 1); // Haptic ON

    // Arm the off-timer only after the ON write is committed, so OFF always follows ON
    // (xTimerChangePeriod also starts timer)
    xTimerChangePeriod(haptic_timer, ticks, portMAX_DELAY);

    // Haptic OFF when timer expires
}

void gpio_utils_rgb_indicate(uint8_t rgb_data)
{
    xSemaphoreTake(xRgbLedMutex, portMAX_DELAY); // Lock RGB LED
    bool is_blink = (rgb_data == RGB_BLINK_RED || rgb_data == RGB_BLINK_GREEN || rgb_data == RGB_BLINK_BLUE || rgb_data == RGB_BLINK_PURPLE || rgb_data == RGB_BLINK_TEAL);
    // Skip blink cmds if invalid (pdMS_TO_TICKS rounds down to 0); always process solid cmds
    if (is_blink && rbg_blink_period_ms < 10) {
        xSemaphoreGive(xRgbLedMutex); // Release RGB LED
        return;
    }
    
    /*
        Fun fact:
        A light blinking at a frequency of around 50-60Hz+ will typically
        appear as a solid, continuous light to the human eye.
        This frequency is known as the critical flicker fusion frequency.
        (Observed when rbg_blink_period_ms < 20.)
        
        If you look at the RGB LED when it's like this in your peripheral‐vision,
        you can see the blinking better being that the threshold can creep up to 80–90Hz.
    */
    
    // Stop any previous blinking
    xTimerStop(rgb_blink_timer, portMAX_DELAY);
    xTimerStop(rgb_blink_stop_timer, portMAX_DELAY);
    
    // Update periods if blink cmd
    if (is_blink) {
        xTimerChangePeriod(rgb_blink_timer, pdMS_TO_TICKS(rbg_blink_period_ms), portMAX_DELAY);
        xTimerChangePeriod(rgb_blink_stop_timer, pdMS_TO_TICKS(rgb_blink_total_ms), portMAX_DELAY);
    }
    xSemaphoreGive(xRgbLedMutex); // Release RGB LED
    
    // All LEDs OFF to start
    gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, 0);
    gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, 0);
    gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 0);

    switch(rgb_data) {
        // Solid color cases
        case RGB_SET_RED:
            gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, 1);
            break;
            
        case RGB_SET_GREEN:
            gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, 1);
            break;
            
        case RGB_SET_BLUE:
            gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 1);
            break;
            
        case RGB_SET_PURPLE:
            gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 1);
            gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, 1);
            break;
            
        case RGB_SET_TEAL:
            gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 1);
            gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, 1);
            break;

        // Blink cases: start timers
        case RGB_BLINK_RED:
            rgb_blink_color = RGB_SET_RED;
            rgb_blink_state = false;
            xTimerStart(rgb_blink_timer, portMAX_DELAY);
            xTimerStart(rgb_blink_stop_timer, portMAX_DELAY);
            break;
            
        case RGB_BLINK_GREEN:
            rgb_blink_color = RGB_SET_GREEN;
            rgb_blink_state = false;
            xTimerStart(rgb_blink_timer, portMAX_DELAY);
            xTimerStart(rgb_blink_stop_timer, portMAX_DELAY);
            break;
        
        case RGB_BLINK_BLUE:
            rgb_blink_color = RGB_SET_BLUE;
            rgb_blink_state = false;
            xTimerStart(rgb_blink_timer, portMAX_DELAY);
            xTimerStart(rgb_blink_stop_timer, portMAX_DELAY);
            break;

        case RGB_BLINK_PURPLE:
            rgb_blink_color = RGB_SET_PURPLE;
            rgb_blink_state = false;
            xTimerStart(rgb_blink_timer, portMAX_DELAY);
            xTimerStart(rgb_blink_stop_timer, portMAX_DELAY);
            break;
            
        case RGB_BLINK_TEAL:
            rgb_blink_color = RGB_SET_TEAL;
            rgb_blink_state = false;
            xTimerStart(rgb_blink_timer, portMAX_DELAY);
            xTimerStart(rgb_blink_stop_timer, portMAX_DELAY);
            break;

        default:
            // Unknown code, LEDs off
            gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, 0);
            gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, 0);
            gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 0);
            break;
    }
}

void gpio_utils_cycle_rgb(void)
{
    gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, 1);
    gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, 0);
    gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(333));
        
    gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, 0);
    gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, 1);
    gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(333));
        
    gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, 0);
    gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, 0);
    gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(333));
        
    gpio_utils_write_output(TCA9535_RED_RGB_LED_PIN, 0);
    gpio_utils_write_output(TCA9535_GREEN_RGB_LED_PIN, 0);
    gpio_utils_write_output(TCA9535_BLUE_RGB_LED_PIN, 0);
}
