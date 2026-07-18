#include <stdbool.h>
#include <stdio.h>

#include "polycast5_gpios.h"
#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/rtc_io.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_littlefs.h" // POLYCAST5_DEBUG_FS
#include "esp_psram.h" // POLYCAST5_DEBUG_RAM
#include "esp_random.h"
#if CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_system.h"
#endif

#include "esp_chip_info.h"

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

#if CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE
// Locked-down builds only (POLYCAST5_SECURE_RELEASE=1 in CMakeLists.txt)
// A device upgraded from dev-mode flash encryption keeps its DEVELOPMENT eFuses:
// the bootloader only burns them on the very first encryption pass. Finish the job here.
// See www.polycast5.com/blogs/docs/lock-it-down
static void lockdown_complete_release_mode(void)
{
    if (!esp_secure_boot_enabled()) {
        ESP_LOGE(TAG, "LOCKDOWN INCOMPLETE: secure boot eFuse is not set - not burning "
                      "flash-encryption release eFuses on an unsecured device. Flash the "
                      "signed bootloader over USB to proceed (see lock-it-down docs)");
        return;
    }

    // Guard against the abort() inside esp_flash_encryption_set_release_mode()
    if (!esp_flash_encryption_cfg_verify_release_mode()) {
        // A power cut during a previous attempt can leave DIS_DOWNLOAD_MANUAL_ENCRYPT
        // unburned but already write-protected (shared WR_DIS bit with DIS_ICACHE)
        
        // Retrying would abort() into a permanent boot loop - keep the device running
        if (esp_efuse_read_field_bit(ESP_EFUSE_WR_DIS_DIS_ICACHE) &&
            !esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MANUAL_ENCRYPT)) {
            ESP_LOGE(TAG, "Release-mode eFuses partially burned and now write-protected; "
                          "cannot complete lockdown on this unit (device still works, OTA only)");
            return;
        }
        ESP_LOGW(TAG, "Completing dev -> release flash-encryption eFuse transition...");
        esp_flash_encryption_set_release_mode();

        // Self-heal after a power cut during an earlier attempt: once the mode already
        // reads RELEASE, set_release_mode() early-returns without burning the remaining bits
        
        // Both burns below are no-ops when already done and still writable here
        uint8_t xts_level = 0;
        esp_efuse_read_field_blob(ESP_EFUSE_XTS_DPA_PSEUDO_LEVEL, &xts_level, ESP_EFUSE_XTS_DPA_PSEUDO_LEVEL[0]->bit_count);
        if (xts_level == 0) {
            xts_level = 1; // ESP_XTS_AES_PSEUDO_ROUNDS_LOW
            esp_efuse_write_field_blob(ESP_EFUSE_XTS_DPA_PSEUDO_LEVEL, &xts_level, ESP_EFUSE_XTS_DPA_PSEUDO_LEVEL[0]->bit_count);
        }
        esp_efuse_write_field_bit(ESP_EFUSE_WR_DIS_DIS_ICACHE);
        if (!esp_flash_encryption_cfg_verify_release_mode()) {
            ESP_LOGE(TAG, "Failed to put flash encryption into release mode");
            return;
        }
        ESP_LOGW(TAG, "Lockdown complete, restarting...");
        esp_restart();
    }

    // Release state verified
    // SECURE_BOOT_SKIP_WRITE_PROTECTION_SCA deferred the write-protect of the SECURE_BOOT_SHA384_EN / XTS_DPA_PSEUDO_LEVEL eFuse group,
    // so upgraded devices could still burn the DPA level above
    esp_err_t wp_err = esp_efuse_write_field_bit(ESP_EFUSE_WR_DIS_SECURE_BOOT_SHA384_EN);
    if (wp_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write-protect the SECURE_BOOT_SHA384_EN eFuse group (%s)", esp_err_to_name(wp_err));
    }

    // Both features verified - normal locked-down boot
    
    // Note: both release-mode audits log a benign "SOFT_DIS_JTAG is set but HMAC key..." warning;
    // they fall back to the hard-JTAG eFuse checks, which pass
    ESP_LOGI(TAG, "Flash encryption: release mode verified");
    ESP_LOGI(TAG, "Secure boot: enabled%s", esp_secure_boot_cfg_verify_release_mode()
             ? ", release mode verified" : " (release-mode eFuse check FAILED, see log above)");
}
#endif

void app_main(void)
{
#if CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE
    // Verify (and if upgrading from a dev-mode device, complete) the lockdown
    lockdown_complete_release_mode();
#endif

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

    // DFS scales CPU between min/max based on load
    // Explicitly request 240 MHz max even though Kconfig caps boot at 160 MHz
    // The 240->160 drop is only errata-safe on chip rev v1.2+; v1.0/below stay at a steady 160 MHz
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    int max_freq_mhz = (chip_info.revision >= 102)               // v1.2+ : 240
                           ? POLYCAST5_CPU_MAX_FREQ_MHZ
                           : POLYCAST5_CPU_FLASH_WRITE_FREQ_MHZ; // v1.0- : 160 steady
    if (max_freq_mhz != POLYCAST5_CPU_MAX_FREQ_MHZ) {
        ESP_LOGE(TAG, "Chip rev v1.0- detected: CPU will run at 160 MHz steady (no DFS)");
    }
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = max_freq_mhz,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };
    esp_err_t pm_err = esp_pm_configure(&pm_cfg);
    if (pm_err != ESP_OK && pm_cfg.max_freq_mhz != POLYCAST5_CPU_FLASH_WRITE_FREQ_MHZ) {
        ESP_LOGE(TAG, "%d MHz PM config rejected (%s), falling back to %d MHz",
                pm_cfg.max_freq_mhz, esp_err_to_name(pm_err), POLYCAST5_CPU_FLASH_WRITE_FREQ_MHZ);
        pm_cfg.max_freq_mhz = POLYCAST5_CPU_FLASH_WRITE_FREQ_MHZ;
        pm_err = esp_pm_configure(&pm_cfg);
    }
    ESP_ERROR_CHECK(pm_err);

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
    
#ifdef POLYCAST5_DEBUG_FS
    // Wait for tasks to allocate
    vTaskDelay(pdMS_TO_TICKS(2000));

    size_t total_bytes, used_bytes;
    esp_err_t ret = esp_littlefs_info("assets", &total_bytes, &used_bytes);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS Partition Info:");
        ESP_LOGI(TAG, "Total: %d bytes", total_bytes);
        ESP_LOGI(TAG, "Used: %d bytes", used_bytes);
    } else {
        ESP_LOGE(TAG, "Failed to get LittleFS info (%s)", esp_err_to_name(ret));
    }
#endif

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Main initialized and tasks created");
#endif
    
}