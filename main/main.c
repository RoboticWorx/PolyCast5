#include <stdbool.h>
#include <stdio.h>

#include "polycast5_gpios.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/rtc_io.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_spiffs.h" // POLYCAST5_DEBUG_SPIFFS
#include "esp_psram.h" // POLYCAST5_DEBUG_RAM
#include "esp_random.h"

#include "sx126x_hal.h"
#include "tca9535.h"

#include "lora_task.h"
#include "lora_radio.h"
#include "lcd_utils.h"
#include "lcd_task.h"
#include "infrared_task.h"
#include "bluetooth_task.h"
//#include "bluetooth_utils.h"
#include "gpio_task.h"
#include "gpio_utils.h"
#include "espnow_task.h"
#include "espnow_utils.h"
#include "wifi_task.h"
#include "ai_task.h"

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

void app_main(void)
{
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
    gpio_utils_init_nvs();
    
    // Allocate Wi-Fi buffers now without fragmentation
    ESP_ERROR_CHECK(espnow_utils_wifi_driver_init());
    // Turn off radio to save power
    ESP_ERROR_CHECK(espnow_utils_wifi_radio_stop());
    
    // Isolate and configure sleep wake up
    //ESP_ERROR_CHECK(rtc_gpio_isolate(TCA9535_USER_BUTTON_POWER_PIN));
    //ESP_ERROR_CHECK(rtc_gpio_set_direction(TCA9535_USER_BUTTON_POWER_PIN, RTC_GPIO_MODE_INPUT_ONLY));
    //ESP_ERROR_CHECK(rtc_gpio_pullup_dis(TCA9535_USER_BUTTON_POWER_PIN));
    //ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(TCA9535_USER_BUTTON_POWER_PIN));
    //ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));
#ifdef POLYCAST5_DEBUG
    //ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_ON));
#endif
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << TCA9535_INT_PIN, ESP_EXT1_WAKEUP_ANY_LOW));

    // Reference so sleep code is pulled in now
    if (false) {
        ESP_ERROR_CHECK(esp_light_sleep_start());
    }

    // Create I2C bus mutex (used in gpio_utils_init)
    xI2CBusMutex = xSemaphoreCreateMutex();
    configASSERT(xI2CBusMutex);

    esp_err_t err = gpio_utils_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_utils_init failed: %s", esp_err_to_name(err));
        return;
    }
    
    // Create SPI mutex before any SPI usage
    xSPIBusMutex = xSemaphoreCreateMutex();
    configASSERT(xSPIBusMutex);

    // Initialize various
    lcd_init_driver();
    lcd_lvgl_init();
    spi_sx126x_init();

    // Create remaining mutexes
    xHapticsMutex = xSemaphoreCreateMutex();
    configASSERT(xHapticsMutex);
    xRgbLedMutex = xSemaphoreCreateMutex();
    configASSERT(xRgbLedMutex);
    xLEDCMutex = xSemaphoreCreateMutex();
    configASSERT(xLEDCMutex);
    
    xPowerButtonSemaphore = xSemaphoreCreateBinary();
    configASSERT(xPowerButtonSemaphore);
    
    // Initialize the SX126x HAL with the SPI handle
    sx126x_hal_init(spi_sx126x);

    // Initialize the sx126x_t structure
    sx126x.context = NULL; // Not used
    sx126x.hal_reset = sx126x_hal_reset;
    sx126x.hal_wakeup = sx126x_hal_wakeup;
    sx126x.hal_write = sx126x_hal_write;
    sx126x.hal_read = sx126x_hal_read;
    
    // Seed random number generation XORWOW PRNG core: TRNG at boot
    uint8_t seed[32];
    esp_fill_random(seed, sizeof(seed));

    // Create tasks
    gpio_task_create();
    lcd_task_create();
    lora_task_create();
    infrared_task_create();
    espnow_task_create();
    wifi_task_create();
    bluetooth_task_create();
    ai_task_create();
    
#ifdef POLYCAST5_DEBUG_RAM
    // Wait for tasks to allocate
    vTaskDelay(pdMS_TO_TICKS(8000));
    
    // Log again after allocating some things
    // Prints how much of that is free for malloc()
    free_ext = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI("AFTER PSRAM", "Free PSRAM heap = %u KB", free_ext / 1024);
    
    // Also show internal
    free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI("AFTER PSRAM", "Free internal heap = %u KB", free_int / 1024);
#endif
    
#ifdef POLYCAST5_DEBUG_SPIFFS
    // Wait for tasks to allocate
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    size_t total_bytes, used_bytes;
    esp_err_t ret = esp_spiffs_info("assets", &total_bytes, &used_bytes);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS Partition Info:");
        ESP_LOGI(TAG, "Total: %d bytes", total_bytes);
        ESP_LOGI(TAG, "Used: %d bytes", used_bytes);
    } else {
        ESP_LOGE(TAG, "Failed to get SPIFFS info (%s)", esp_err_to_name(ret));
    }
#endif

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Main initialized and tasks created");
#endif
    
}