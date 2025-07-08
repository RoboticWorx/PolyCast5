#include "freertos/projdefs.h"
#include "polycast5_macros.h"

#include "nvs_flash.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "tca9535.h"

#include "gpio_funcs.h"
#include "gpio_task.h"

#define TAG "GPIO_FUNCS"

#define ADC_CH ADC_CHANNEL_4
#define NUM_ADC_SAMPLES 16384

static TimerHandle_t haptic_timer;

static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t cali_handle = NULL;

typedef struct {
    float volt;
    uint8_t soc; // State-of-charge (percentage)
} soc_point_t;

// LiPo voltage and corresponding discharge state based on typical LiPo voltage discharge curve
static const soc_point_t soc_table[] = { // {V, %}
    {4.20, 100}, {4.15, 95},  {4.11, 90},  {4.08, 85},  {4.02, 80},
    {3.98, 75},  {3.95, 70},  {3.91, 65},  {3.87, 60},  {3.85, 55},
   {3.84, 50}, {3.82, 45}, {3.80, 40}, {3.79, 35}, {3.77, 30},
   {3.75, 25}, {3.73, 20}, {3.69, 15}, {3.61, 10} ,{3.50, 5},
   {3.27, 0},
};
static const int TABLE_LEN = sizeof(soc_table) / sizeof(soc_table[0]);

void gpio_init_nvs(void)
{
	// Initialize flash
    esp_err_t ret = nvs_flash_init();
    
    // Error check
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS partition...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    ESP_ERROR_CHECK(ret);
    
    #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "NVS initialized");
    #endif
    
}

static void IRAM_ATTR haptic_off_cb(TimerHandle_t xTimer)
{
    gpio_set_level(HAPTIC_PIN, 0);
}

esp_err_t gpio_init(void)
{
	// Configure outputs
	gpio_config_t io_conf_out = {
	    .pin_bit_mask = (1ULL << ST7789_LEDA_PIN) |
	        			(1ULL << ST7789_DC_PIN)   |
	        			(1ULL << ST7789_RST_PIN)  |
	      				(1ULL << HAPTIC_PIN),
	    .mode = GPIO_MODE_OUTPUT,
	    .pull_up_en = GPIO_PULLUP_DISABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	    .intr_type = GPIO_INTR_DISABLE
	};
	gpio_config(&io_conf_out);
	
	// Default states
	gpio_set_level(ST7789_LEDA_PIN, 0); // LCD BL high on start
	gpio_set_level(HAPTIC_PIN, 0); // Hapic motor low on start
	
	// Configure inputs
	/*gpio_config_t io_conf_in = {
	    .pin_bit_mask = (1ULL << USER_BUTTON_POWER),
	    .mode = GPIO_MODE_INPUT,
	    .intr_type = GPIO_INTR_DISABLE,
	    .pull_up_en = GPIO_PULLUP_DISABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	};
	gpio_config(&io_conf_in);*/
	
	// Configure interrupts
	/*gpio_config_t io_conf_int = {
	    .pin_bit_mask = (1ULL << USER_BUTTON_POWER),
	    .mode = GPIO_MODE_INPUT,
	    .intr_type = GPIO_INTR_NEGEDGE,
	    .pull_up_en = GPIO_PULLUP_DISABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	};
	gpio_config(&io_conf_int);*/


	// ISR service
	gpio_install_isr_service(0);
	//gpio_isr_handler_add(TCA9535_INT_GPIO, tca9535_int_isr, NULL);	
	
	
	// Create a timer for the haptic motor
    haptic_timer = xTimerCreate("haptic_off", pdMS_TO_TICKS(10), pdFALSE, NULL, haptic_off_cb);
    configASSERT(haptic_timer);
	
	
	esp_err_t ret = TCA9535Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA9535Init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Port0 = all inputs (0xFF)
    ret = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0, 0xFF);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config0 write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
	// Port1: pins 0–2 outputs, pins 3–7 inputs.
	// bit7 bit6 bit5 bit4 bit3 bit2 bit1 bit0
	//    1    1    1    1    1    0    0    0    = 0xF8
	ret = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG1, 0xF8);
	if (ret != ESP_OK) {
	    ESP_LOGE(TAG, "Config1 write failed: %s", esp_err_to_name(ret));
	}
	
	return ret;
}

int gpio_read_input(uint8_t pin)
{
    if (pin > 7) {
        ESP_LOGE(TAG, "Invalid input pin %d", pin);
        return -1;
    }
    
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY); // Lock I2C bus
    uint8_t inputs = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0);
    xSemaphoreGive(xI2CBusMutex); // Release I2C bus
    
    return (inputs >> pin) & 0x1;
}

esp_err_t gpio_write_output(uint8_t pin, bool level)
{
    if (pin > 7) {
        ESP_LOGE(TAG, "Invalid output pin %d", pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY); // Lock I2C bus
    uint8_t out = TCA9535ReadSingleRegister(TCA9535_OUTPUT_REG1);
    xSemaphoreGive(xI2CBusMutex); // Release I2C bus
    
    if (level) {
        out |=  (1 << pin);
    } else {
        out &= ~(1 << pin);
    }
    
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY); // Lock I2C bus
    esp_err_t err = TCA9535WriteSingleRegister(TCA9535_OUTPUT_REG1, out);
    xSemaphoreGive(xI2CBusMutex); // Release I2C bus
    
    return err;
}

void gpio_init_battery_adc(void)
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
        .atten     = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CH, &chan_cfg));

	#ifdef POLYCAST5_DEBUG
    	ESP_LOGI(TAG, "ADC initialized");
    #endif
    
    // Configure curve fitting
    adc_cali_curve_fitting_config_t cfg = {
	    .unit_id   = ADC_UNIT_1,
	    .atten     = ADC_ATTEN_DB_12,
	    .bitwidth  = ADC_BITWIDTH_12,
	};
	ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cfg, &cali_handle));
}

void gpio_deinit_battery_adc(void)
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

float gpio_get_battery_voltage(void)
{	
	uint32_t sum = 0;
	
	// Average readings
	for (int i = 0; i < NUM_ADC_SAMPLES; i++) {
	    int raw;
	    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CH, &raw));
	    sum += raw;
	    esp_rom_delay_us(5);
	}
	int avg_raw = sum / NUM_ADC_SAMPLES;
	
	// Get pin mV
	int pin_mv;
	ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, avg_raw, &pin_mv));
	
	float Vadc = pin_mv / 1000.0f; // Convert to volts
	
    //return Vadc;
    
    // Undo divider + op amp: Vbat = (Vadc + off) / gain
    const float R42 = 10000, R43 = 27400;
    const float R44 = 10000, R45 = 27400;
    const float R40 =  2200, R41 = 22000;
    const float Vref = 3.3f * (R41 / (R40+R41));
    const float gain = (1.0f + R45 / R44) * (R43 / (R42+R43));
    const float off  = (R45 / R44) * Vref;

    return (Vadc + off)/gain;
}

uint8_t gpio_volts_to_soc(float voltage)
{
	// Clamp at max
    if (voltage >= soc_table[0].volt) {
    	return 100;
    }
    
    // Clamp at min
    if (voltage <= soc_table[TABLE_LEN - 1].volt) {
		return 0;
	}

    // Search for where the voltage lives between on soc_table
    for (int i = 0; i < TABLE_LEN - 1; i++) {
		// Neighboring high and low voltage
        float v_hi = soc_table[i].volt;
        float v_lo = soc_table[i + 1].volt;
        
        // If 'voltage' lives between them
        if (voltage >= v_lo && voltage <= v_hi) {
			// Get neighboring SoC
            uint8_t soc_hi = soc_table[i].soc;
            uint8_t soc_lo = soc_table[i + 1].soc;
            
            // Linearly interpolate the value (map)
            float soc_f = (voltage - v_lo) * (soc_hi - soc_lo) / (v_hi - v_lo) + soc_lo;
            
            // Round to nearest % and return
            return (uint8_t)(soc_f + 0.5f);
        }
    }

    // Fallback
    return 0;
}

void gpio_spin_haptic(uint32_t ms)
{
    gpio_set_level(HAPTIC_PIN, 1); // Haptic ON
    
    // Re-arm the timer with the new period
    xTimerChangePeriod(haptic_timer, pdMS_TO_TICKS(ms), 0);
    xTimerStart(haptic_timer, 0);
    
    // Haptic OFF when timer expires
}

void gpio_cycle_rgb(void)
{
	gpio_write_output(0, 1); // Red LED
	gpio_write_output(1, 0); // Green LED
	gpio_write_output(2, 0); // Blue LED
	vTaskDelay(pdMS_TO_TICKS(333));
		
	gpio_write_output(0, 0);
	gpio_write_output(1, 1);
	gpio_write_output(2, 0);
	vTaskDelay(pdMS_TO_TICKS(333));
		
	gpio_write_output(0, 0);
	gpio_write_output(1, 0);
	gpio_write_output(2, 1);
	vTaskDelay(pdMS_TO_TICKS(333));
		
	gpio_write_output(0, 0);
	gpio_write_output(1, 0);
	gpio_write_output(2, 0);
}
