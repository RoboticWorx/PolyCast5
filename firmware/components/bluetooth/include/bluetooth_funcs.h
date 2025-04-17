#ifndef BLUETOOTH_FUNCS_H
#define BLUETOOTH_FUNCS_H

#include "esp_hidd.h"

#define HID_CC_IN_RPT_LEN 2
#define HID_RPT_ID_CC_IN 3

// HID Consumer Usage IDs
#define HID_CONSUMER_CHANNEL_UP 156
#define HID_CONSUMER_CHANNEL_DOWN 157
#define HID_CONSUMER_VOLUME_UP 233
#define HID_CONSUMER_VOLUME_DOWN 234
#define HID_CONSUMER_MUTE 226
#define HID_CONSUMER_POWER 48
#define HID_CONSUMER_RECALL_LAST 131
#define HID_CONSUMER_ASSIGN_SEL 129
#define HID_CONSUMER_PLAY 176
#define HID_CONSUMER_PAUSE 177
#define HID_CONSUMER_RECORD 178
#define HID_CONSUMER_FAST_FORWARD 179
#define HID_CONSUMER_REWIND 180
#define HID_CONSUMER_SCAN_NEXT_TRK 181
#define HID_CONSUMER_SCAN_PREV_TRK 182
#define HID_CONSUMER_STOP 183

typedef struct {
    TaskHandle_t task_hdl;
    esp_hidd_dev_t *hid_dev;
    uint8_t protocol_mode;
    uint8_t *buffer;
} local_param_t;

extern local_param_t s_ble_hid_param;
extern esp_hid_device_config_t ble_hid_config;

void esp_hidd_send_consumer_value(uint8_t key_cmd, bool key_pressed);
void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data);
void ble_hid_device_host_task(void *param);
void ble_hid_task_start_up(void);
void ble_hid_task_shut_down(void);
void init_battery_service(void);
void init_device_info_service(void);

#endif // BLUETOOTH_FUNCS_H