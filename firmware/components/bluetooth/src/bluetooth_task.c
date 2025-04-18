#include "bluetooth_task.h"
#include "bluetooth_funcs.h"
#include "esp_log.h"

void ble_hid_demo_task(void *pvParameters)
{
    static bool send_volum_up = false;
    while (1) {
        ESP_LOGI(BLUETOOTH_TAG, "Send the volume");
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

void ble_hid_task_start_up(void)
{
    if (s_ble_hid_param.task_hdl) {
        return;
    }
    bluetooth_init();
    xTaskCreate(ble_hid_demo_task, "ble_hid_demo_task", 4 * 1024, NULL, configMAX_PRIORITIES - 3,
                &s_ble_hid_param.task_hdl);
}

void ble_hid_task_shut_down(void)
{
    if (s_ble_hid_param.task_hdl) {
        vTaskDelete(s_ble_hid_param.task_hdl);
        s_ble_hid_param.task_hdl = NULL;
    }
}