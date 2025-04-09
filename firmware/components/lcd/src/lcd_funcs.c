#include "lcd_funcs.h"

#include "esp_log.h"
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "LCD_FUNCS";

// SPI device handle (defined in lcd_task.c, declared here as extern)
extern spi_device_handle_t spi_st7789;

// Reset the display
void lcd_reset(void) {
	gpio_set_level(ST7789_RST_PIN, 0);
	vTaskDelay(pdMS_TO_TICKS(100));
	gpio_set_level(ST7789_RST_PIN, 1);
	vTaskDelay(pdMS_TO_TICKS(100));
}

// Send command to ST7789
void lcd_send_cmd(lv_display_t *disp, const uint8_t *cmd, unsigned int cmd_len,
				  const uint8_t *param, unsigned int param_len) {
	esp_err_t ret;
	spi_transaction_t t;

	gpio_set_level(ST7789_DC_PIN, 0); // Command mode
	memset(&t, 0, sizeof(t));
	t.length = cmd_len * 8; // cmd_len is in bytes
	t.tx_buffer = cmd;
	ret = spi_device_polling_transmit(spi_st7789, &t);
	ESP_ERROR_CHECK(ret);

	if (param && param_len > 0) {
		gpio_set_level(ST7789_DC_PIN, 1); // Data mode
		memset(&t, 0, sizeof(t));
		t.length = param_len * 8;
		t.tx_buffer = param;
		ret = spi_device_polling_transmit(spi_st7789, &t);
		ESP_ERROR_CHECK(ret);
	}
}

// Send color data to ST7789 (adjusted to unsigned char *)
void lcd_send_color(lv_display_t *disp, const uint8_t *cmd,
					unsigned int cmd_len, unsigned char *data,
					unsigned int data_len) {
	esp_err_t ret;
	spi_transaction_t t;

	// Send command first
	lcd_send_cmd(disp, cmd, cmd_len, NULL, 0);

	// Send color data
	gpio_set_level(ST7789_DC_PIN, 1); // Data mode
	memset(&t, 0, sizeof(t));
	t.length =
		data_len * 8; // data_len is in bytes (adjusted from 16-bit to 8-bit)
	t.tx_buffer = data;
	ret = spi_device_polling_transmit(spi_st7789, &t);
	ESP_ERROR_CHECK(ret);
}