#include "nvs_flash.h"

#include "driver/i2c.h"

#include "esp_sleep.h"
#include "esp_log.h"

#include "TCA9535.h"

#include "lcd_utils.h"
#include "gpio_funcs.h"
#include "gpio_task.h"

static const char *TAG = "GPIO_FUNCS";

static void IRAM_ATTR tca9535_int_isr(void *arg) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(xGpioEventSemaphore, &woken);
    portYIELD_FROM_ISR(woken);
}

static void IRAM_ATTR power_int_isr(void *arg) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(xPowerButtonSemaphore, &woken);
    portYIELD_FROM_ISR(woken);
}

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

esp_err_t gpio_init(void)
{
	// Configure outputs
	gpio_config_t io_conf_out = {
	    .pin_bit_mask = (1ULL << ST7789_LEDK_PIN) |
	        			(1ULL << ST7789_DC_PIN)   |
	        			(1ULL << ST7789_RST_PIN),  //|
	      				//(1ULL << HAPTIC_PIN),
	    .mode = GPIO_MODE_OUTPUT,
	    .pull_up_en = GPIO_PULLUP_DISABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	    .intr_type = GPIO_INTR_DISABLE
	};
	gpio_config(&io_conf_out);
	
	// Configure inputs
	/*gpio_config_t io_conf_in = {
	    .pin_bit_mask = (1ULL << USER_BUTTON_POWER),
	    .mode = GPIO_MODE_INPUT,
	    .intr_type = GPIO_INTR_DISABLE,
	    .pull_up_en = GPIO_PULLUP_DISABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	};
	gpio_config(&io_conf_in);*/
	
	// Configure inputs
	gpio_config_t io_conf_int = {
	    .pin_bit_mask = (1ULL << TCA9535_INT_GPIO) |
	    				(1ULL << USER_BUTTON_POWER),
	    .mode = GPIO_MODE_INPUT,
	    .intr_type = GPIO_INTR_NEGEDGE,
	    .pull_up_en = GPIO_PULLUP_DISABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	};
	gpio_config(&io_conf_int);

	// ISR service
	gpio_install_isr_service(0);
	gpio_isr_handler_add(TCA9535_INT_GPIO, tca9535_int_isr, NULL);
	gpio_isr_handler_add(USER_BUTTON_POWER, power_int_isr, NULL);
	

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

#ifdef POLYCAST5_CYCLE_RGB_ON_BOOT
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
#endif

int gpio_read_input(uint8_t pin)
{
    if (pin > 7) {
        ESP_LOGE(TAG, "Invalid input pin %d", pin);
        return -1;
    }
    uint8_t inputs = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0);
    return (inputs >> pin) & 0x1;
}

esp_err_t gpio_write_output(uint8_t pin, bool level)
{
    if (pin > 7) {
        ESP_LOGE(TAG, "Invalid output pin %d", pin);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t out = TCA9535ReadSingleRegister(TCA9535_OUTPUT_REG1);
    if (level) {
        out |=  (1 << pin);
    } else {
        out &= ~(1 << pin);
    }
    return TCA9535WriteSingleRegister(TCA9535_OUTPUT_REG1, out);
}
