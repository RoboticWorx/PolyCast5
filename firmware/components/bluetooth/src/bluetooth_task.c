#include <stdio.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "esp_hid_gap.h"
#include "esp_hidd.h"
#include "bluetooth_funcs.h"
#include "bluetooth_task.h"

static const char *TAG = "HID_DEV_DEMO";

static void bluetooth_main_task(void *pvParameters) {
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "setting hid gap, mode:%d", HID_DEV_MODE);
    ret = esp_hid_gap_init(HID_DEV_MODE);
    ESP_ERROR_CHECK(ret);

    ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GENERIC, ble_hid_config.device_name);
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "setting ble device");
    ret = esp_hidd_dev_init(&ble_hid_config, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &s_ble_hid_param.hid_dev);
    ESP_ERROR_CHECK(ret);

    extern void ble_store_config_init(void);
    ble_store_config_init();
    
    init_device_info_service();
    init_battery_service();

    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ret = esp_nimble_enable(ble_hid_device_host_task);
    if (ret) {
        ESP_LOGE(TAG, "esp_nimble_enable failed: %d", ret);
    }

    static bool send_volum_up = false;
    while (1) {
        ESP_LOGI(TAG, "Send the volume");
        if (send_volum_up) {
            esp_hidd_send_consumer_value(HID_CONSUMER_VOLUME_UP, true);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            esp_hidd_send_consumer_value(HID_CONSUMER_VOLUME_UP, false);
        } else {
            esp_hidd_send_consumer_value(HID_CONSUMER_VOLUME_DOWN, true);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            esp_hidd_send_consumer_value(HID_CONSUMER_VOLUME_DOWN, false);
        }
        send_volum_up = !send_volum_up;
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

void bluetooth_task_create(void) {
    xTaskCreate(bluetooth_main_task, "BluetoothMainTask", 4096, NULL, 5, NULL);
}