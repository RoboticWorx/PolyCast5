#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "esp_log.h"
//#include "services/bas/ble_svc_bas.h"

#include "bluetooth_funcs.h"

#define TAG "BLUETOOTH_FUNCS"

extern volatile bool bluetooth_connected;

typedef struct {
	TaskHandle_t task_hdl;
	esp_hidd_dev_t *hid_dev;
	uint8_t protocol_mode;
	uint8_t *buffer;
} ble_hid_param_t;

static ble_hid_param_t ble_hid_param = {0};

const unsigned char media_report_map[] = {
	0x05, 0x0C,			// 	 Usage Page (Consumer)	
	0x09, 0x01,			// 	 Usage (Consumer Control)
	0xA1, 0x01,			//	 Collection (Application)
	0x85, 0x03,			//   Report ID (3)
	0x09, 0x02,			//   Usage (Numeric Key Pad)
	0xA1, 0x02,		//   Collection (Logical)
	0x05, 0x09,		//	 Usage Page (Button)
	0x19, 0x01,		//	 Usage Minimum (0x01)
	0x29, 0x0A,		//	 Usage Maximum (0x0A)
	0x15, 0x01,		//	 Logical Minimum (1)
	0x25, 0x0A,		//	 Logical Maximum (10)
	0x75, 0x04,		//	 Report Size (4)
	0x95, 0x01,		//	 Report Count (1)
	0x81, 0x00,		//	 Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
	0xC0,			  		//   End Collection
	0x05, 0x0C,		//   Usage Page (Consumer)
	0x09, 0x86,		//   Usage (Channel)
	0x15, 0xFF,		//   Logical Minimum (-1)
	0x25, 0x01,		//   Logical Maximum (1)
	0x75, 0x02,		//   Report Size (2)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x46,		//   Input (Data,Var,Rel,No Wrap,Linear,Preferred State,Null State)
	0x09, 0xE9,		//   Usage (Volume Increment)
	0x09, 0xEA,		//   Usage (Volume Decrement)
	0x15, 0x00,		//   Logical Minimum (0)
	0x75, 0x01,		//   Report Size (1)
	0x95, 0x02,		//   Report Count (2)
	0x81, 0x02,		//   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
	0x09, 0xE2,		//   Usage (Mute)
	0x09, 0x30,		//   Usage (Power)
	0x09, 0x83,		//   Usage (Recall Last)
	0x09, 0x81,		//   Usage (Assign Selection)
	0x09, 0xB0,		//   Usage (Play)
	0x09, 0xB1,		//   Usage (Pause)
	0x09, 0xB2,		//   Usage (Record)
	0x09, 0xB3,		//   Usage (Fast Forward)
	0x09, 0xB4,		//   Usage (Rewind)
	0x09, 0xB5,		//   Usage (Scan Next Track)
	0x09, 0xB6,		//   Usage (Scan Previous Track)
	0x09, 0xB7,		//   Usage (Stop)
	0x15, 0x01,		//   Logical Minimum (1)
	0x25, 0x0C,		//   Logical Maximum (12)
	0x75, 0x04,		//   Report Size (4)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x00,		//   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
	0x09, 0x80,		//   Usage (Selection)
	0xA1, 0x02,		//   Collection (Logical)
	0x05, 0x09,		//	 Usage Page (Button)
	0x19, 0x01,		//	 Usage Minimum (0x01)
	0x29, 0x03,		//	 Usage Maximum (0x03)
	0x15, 0x01,		//	 Logical Minimum (1)
	0x25, 0x03,		//	 Logical Maximum (3)
	0x75, 0x02,		//	 Report Size (2)
	0x81, 0x00,		//	 Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
	0xC0,			  		//   End Collection
	0x81, 0x03,		//   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
	0xC0,			  		// 	 End Collection
};

static esp_hid_raw_report_map_t ble_report_maps[] = {
	{
		.data = media_report_map,
		.len = sizeof(media_report_map)
	}
};

static esp_hid_device_config_t ble_hid_config = {
	.vendor_id = 0x16C0,
	.product_id = 0x05DF,
	.version = 0x0100,
	.device_name = "PolyCast5",
	.manufacturer_name = "RoboticWorx",
	.serial_number = "1234567890",
	.report_maps = ble_report_maps,
	.report_maps_len = 1
};

void bluetooth_send_cmd(uint8_t key_cmd, bool key_pressed)
{
	uint8_t buffer[HID_CC_IN_RPT_LEN] = {0, 0};
	
	if (key_pressed) {
		switch (key_cmd) {
			case BLUETOOTH_CMD_CHANNEL_UP:
				HID_CC_RPT_SET_CHANNEL(buffer, HID_CC_RPT_CHANNEL_UP);
				break;
	
			case BLUETOOTH_CMD_CHANNEL_DOWN:
				HID_CC_RPT_SET_CHANNEL(buffer, HID_CC_RPT_CHANNEL_DOWN);
				break;
	
			case BLUETOOTH_CMD_VOLUME_UP:
				HID_CC_RPT_SET_VOLUME_UP(buffer);
				break;
	
			case BLUETOOTH_CMD_VOLUME_DOWN:
				HID_CC_RPT_SET_VOLUME_DOWN(buffer);
				break;
	
			case BLUETOOTH_CMD_MUTE:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_MUTE);
				break;
	
			case BLUETOOTH_CMD_POWER:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_POWER);
				break;
	
			case BLUETOOTH_CMD_RECALL_LAST:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_LAST);
				break;
	
			case BLUETOOTH_CMD_ASSIGN_SEL:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_ASSIGN_SEL);
				break;
	
			case BLUETOOTH_CMD_PLAY:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_PLAY);
				break;
	
			case BLUETOOTH_CMD_PAUSE:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_PAUSE);
				break;
	
			case BLUETOOTH_CMD_RECORD:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_RECORD);
				break;
	
			case BLUETOOTH_CMD_FAST_FORWARD:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_FAST_FWD);
				break;
	
			case BLUETOOTH_CMD_REWIND:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_REWIND);
				break;
	
			case BLUETOOTH_CMD_SCAN_NEXT_TRK:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_SCAN_NEXT_TRK);
				break;
	
			case BLUETOOTH_CMD_SCAN_PREV_TRK:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_SCAN_PREV_TRK);
				break;
	
			case BLUETOOTH_CMD_STOP:
				HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_STOP);
				break;
	
			default:
				break;
		}
	}
	
	#ifdef POLYCAST5_DEBUG
		ESP_LOG_BUFFER_HEX("HID_PAYLOAD", buffer, 2);
	#endif
	
	esp_hidd_dev_input_set(ble_hid_param.hid_dev, 0, HID_RPT_ID_CC_IN, buffer, HID_CC_IN_RPT_LEN);
	return;
}

static void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
	esp_hidd_event_t event = (esp_hidd_event_t)id;
	esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

	switch (event) {
		case ESP_HIDD_START_EVENT: {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "START");
			#endif
			
			esp_hid_ble_gap_adv_start();
			break;
		}
		case ESP_HIDD_CONNECT_EVENT: {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "CONNECT");
			#endif
			
			bluetooth_connected = true;
			break;
		}
		case ESP_HIDD_PROTOCOL_MODE_EVENT: {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "PROTOCOL MODE[%u]: %s", param->protocol_mode.map_index, param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
			#endif
			
			break;
		}
		case ESP_HIDD_CONTROL_EVENT: {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "CONTROL[%u]: %sSUSPEND", param->control.map_index, param->control.control ? "EXIT_" : "");
			#endif
			
			if (param->control.control) {
				// Exit suspend
			}
			else {
				// Suspend
			}
			break;
		}
		case ESP_HIDD_OUTPUT_EVENT: {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "OUTPUT[%u]: %8s ID: %2u, Len: %d, Data:", param->output.map_index, esp_hid_usage_str(param->output.usage), param->output.report_id, param->output.length);
				ESP_LOG_BUFFER_HEX(TAG, param->output.data, param->output.length);
			#endif
			
			break;
		}
		case ESP_HIDD_FEATURE_EVENT: {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "FEATURE[%u]: %8s ID: %2u, Len: %d, Data:", param->feature.map_index, esp_hid_usage_str(param->feature.usage), param->feature.report_id, param->feature.length);
				ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
			#endif
			
			break;
		}
		case ESP_HIDD_DISCONNECT_EVENT: {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "DISCONNECT: %s", esp_hid_disconnect_reason_str(esp_hidd_dev_transport_get(param->disconnect.dev), param->disconnect.reason));
			#endif
			
			bluetooth_connected = false;
			esp_hid_ble_gap_adv_start();
			break;
		}
		case ESP_HIDD_STOP_EVENT: {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "STOP");
			#endif
			
			break;
		}
		default:
			break;
	}
	return;
}

/* This function will return only when nimble_port_stop() is executed */
static void ble_hid_device_host_task(void *param)
{
	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "BLE Host Task Started");
	#endif
	
	nimble_port_run();
	nimble_port_freertos_deinit();
}

// Declaration of extern esp function
void ble_store_config_init(void);

void bluetooth_init(void)
{
	esp_err_t ret;
	
	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Setting HID gap, mode:%d", HID_DEV_MODE);
	#endif
	
	ret = esp_hid_gap_init(HID_DEV_MODE);
	ESP_ERROR_CHECK(ret);
	
	ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GENERIC, ble_hid_config.device_name);
	ESP_ERROR_CHECK(ret);
	
	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Setting BLE device");
	#endif
	
	ret = esp_hidd_dev_init(&ble_hid_config, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &ble_hid_param.hid_dev);
	ESP_ERROR_CHECK(ret);
	
	//ble_svc_bas_init();
	
	// Need to have a template to store
	ble_store_config_init();

	ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
	
	// Starting nimble task after GATTS is initialized
	ret = esp_nimble_enable(ble_hid_device_host_task);
	if (ret) {
		ESP_LOGE(TAG, "esp_nimble_enable failed: %d", ret);
	}
}

void bluetooth_deinit(void)
{
    int rc;
    esp_err_t err;

    // Stop advertising
    ble_gap_adv_stop();

    // Stop the NimBLE host thread (blocks until nimble_port_run() returns)
    rc = nimble_port_stop();
    if (rc) {
        ESP_LOGE(TAG, "nimble_port_stop failed: %d", rc);
        return;
    }

    // De-initialize the host stack and controller
    err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit failed: %s", esp_err_to_name(err));
        return;
    }

    // Tear down the HID/GATT state
    if (ble_hid_param.hid_dev) {
        err = esp_hidd_dev_deinit(ble_hid_param.hid_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_hidd_dev_deinit failed: %s", esp_err_to_name(err));
        }
        ble_hid_param.hid_dev = NULL;
    }

	#ifdef POLYCAST5_DEBUG
	    ESP_LOGI(TAG, "Bluetooth fully disabled");
    #endif
}