#include "gpio_funcs.h"
#include "TCA9535.h"          // your TCA9535 library header
#include "driver/i2c.h"
#include "esp_log.h"
#include "lcd_funcs.h"

static const char *TAG = "GPIO_FUNCS";

esp_err_t gpio_init(void)
{
	gpio_config_t io_conf_out = {
	    .pin_bit_mask =
	        (1ULL << ST7789_LEDK_PIN) |
	        (1ULL << ST7789_DC_PIN)   |
	        (1ULL << ST7789_RST_PIN),
	    .mode           = GPIO_MODE_OUTPUT,
	    .pull_up_en     = GPIO_PULLUP_DISABLE,
	    .pull_down_en   = GPIO_PULLDOWN_DISABLE,
	    .intr_type      = GPIO_INTR_DISABLE
	};
	gpio_config(&io_conf_out);
	
	gpio_config_t io_conf_in = {
		.pin_bit_mask =
			(1ULL << TCA9535_INT_GPIO), // | (1ULL << ST7789_RST_PIN),
		.mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE};

	gpio_config(&io_conf_in);
	
	
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

int  gpio_read_input(uint8_t pin)
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
