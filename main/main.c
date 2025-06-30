#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/rtc_io.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_psram.h" // POLYCAST5_DEBUG_RAM

#include "sx126x_hal.h"

#include "lora_task.h"
#include "lora_funcs.h"
#include "lcd_utils.h"
#include "lcd_task.h"
#include "infrared_task.h"
//#include "bluetooth_task.h"
//#include "bluetooth_funcs.h"
#include "gpio_task.h"
#include "gpio_funcs.h"
#include "espnow_task.h"
#include "espnow_funcs.h"
#include "wifi_task.h"

// Logging tag
static const char *TAG = "MAIN";

// SPI device handle
spi_device_handle_t spi_sx126x;

// SX126x instance
sx126x_t sx126x;

static void spi_sx126x_init()
{
    esp_err_t ret;

    // Attach the SX126x device
    spi_device_interface_config_t sx_cfg = {
        .mode = 0,
        .clock_speed_hz = 1 * 1000 * 1000, // 1 MHz
        .spics_io_num = SX126X_CS_PIN,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI2_HOST, &sx_cfg, &spi_sx126x);
    assert(ret == ESP_OK);
}

void app_main(void) {

	#ifdef POLYCAST5_DEBUG_RAM
		// Prints PSRAM chip size
	    size_t psram_size = esp_psram_get_size();
	    ESP_LOGI("PSRAM", "Detected PSRAM size = %u KB", psram_size / 1024);
	
	    // Prints how much of that is free for malloc()
	    size_t free_ext = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	    ESP_LOGI("PSRAM", "Free PSRAM heap = %u KB", free_ext / 1024);
	
	    // Also show internal
	    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
	    ESP_LOGI("PSRAM", "Free internal heap = %u KB", free_int / 1024);
    #endif
    
    // Initialize NVS flash
    gpio_init_nvs();
    
    // Allocate Wi-Fi buffers now without fragmentation
    ESP_ERROR_CHECK(espnow_funcs_wifi_driver_init());
    // Turn off radio to save power
    ESP_ERROR_CHECK(espnow_funcs_wifi_radio_stop());
    
	// Isolate and configure sleep wake up
	//ESP_ERROR_CHECK(rtc_gpio_isolate(USER_BUTTON_POWER));
	//ESP_ERROR_CHECK(rtc_gpio_set_direction(USER_BUTTON_POWER, RTC_GPIO_MODE_INPUT_ONLY));
	//ESP_ERROR_CHECK(rtc_gpio_pullup_dis(USER_BUTTON_POWER));
	//ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(USER_BUTTON_POWER));
	//ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));
	#ifdef POLYCAST5_DEBUG
		//ESP_ERROR_CHECKesp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_ON));
	#endif
	ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << USER_BUTTON_POWER, ESP_EXT1_WAKEUP_ANY_LOW));

	// Reference so sleep code is pulled in now
    if (false) {
    	ESP_ERROR_CHECK(esp_light_sleep_start());
    }
	
	// Initialize various
	lcd_init_driver();
    lcd_lvgl_init();
	spi_sx126x_init();
	
	xSPIBusMutex = xSemaphoreCreateMutex();
	configASSERT(xSPIBusMutex); // Ensure success
	
	xPowerButtonSemaphore = xSemaphoreCreateBinary();
	configASSERT(xPowerButtonSemaphore);
	
    if (gpio_init() != ESP_OK) {
        ESP_LOGE(TAG, "gpio_init failed");
        return;
    }
	
	gpio_set_level(ST7789_LEDK_PIN, 1); // LCD BL high on start
	
	// Initialize the SX126x HAL with the SPI handle
	sx126x_hal_init(spi_sx126x);

	// Initialize the sx126x_t structure
	sx126x.context = NULL; // Not used
	sx126x.hal_reset = sx126x_hal_reset;
	sx126x.hal_wakeup = sx126x_hal_wakeup;
	sx126x.hal_write = sx126x_hal_write;
	sx126x.hal_read = sx126x_hal_read;

	// Create tasks
	gpio_task_create();
	lcd_task_create();
	lora_task_create();
	infrared_task_create();
	espnow_task_create();
	wifi_task_create();
	//ble_hid_task_start_up();
	
	#ifdef POLYCAST5_DEBUG_RAM
		// Log again after allocating some things
	    // Prints how much of that is free for malloc()
	    free_ext = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	    ESP_LOGI("AFTER PSRAM", "Free PSRAM heap = %u KB", free_ext / 1024);
	
	    // Also show internal
	    free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
	    ESP_LOGI("AFTER PSRAM", "Free internal heap = %u KB", free_int / 1024);
    #endif

	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Main initialized and tasks created");
	#endif
	
}