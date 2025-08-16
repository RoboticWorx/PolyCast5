#include "polycast5_macros.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "esp_log.h"

#include "bluetooth_funcs.h"
#include "gpio_funcs.h"
#include "gpio_task.h"

#define TAG "BLUETOOTH_FUNCS"

#define DEVICE_NAME "PolyCast5"

/* Consumer-control report encoding (matches your sender) */
#define HID_CC_RPT_MUTE 1
#define HID_CC_RPT_POWER 2
#define HID_CC_RPT_LAST 3
#define HID_CC_RPT_ASSIGN_SEL 4
#define HID_CC_RPT_PLAY 5
#define HID_CC_RPT_PAUSE 6
#define HID_CC_RPT_RECORD 7
#define HID_CC_RPT_FAST_FWD 8
#define HID_CC_RPT_REWIND 9
#define HID_CC_RPT_SCAN_NEXT_TRK 10
#define HID_CC_RPT_SCAN_PREV_TRK 11
#define HID_CC_RPT_STOP 12
#define HID_CC_RPT_PLAY_PAUSE 13

#define HID_CC_RPT_VOLUME_UP 0x40
#define HID_CC_RPT_VOLUME_DOWN 0x80

// Masks / setters (unchanged behavior)
#define HID_CC_RPT_NUMERIC_BITS 0xF0
#define HID_CC_RPT_CHANNEL_BITS 0xCF
#define HID_CC_RPT_VOLUME_BITS 0x3F
#define HID_CC_RPT_BUTTON_BITS 0xF0
#define HID_CC_RPT_SELECTION_BITS 0xCF

#define HID_CC_RPT_SET_NUMERIC(s, x)   (s)[0] &= HID_CC_RPT_NUMERIC_BITS;   (s)[0] = (x)
#define HID_CC_RPT_SET_CHANNEL(s, x)   (s)[0] &= HID_CC_RPT_CHANNEL_BITS;   (s)[0] |= ((x) & 0x03) << 4
#define HID_CC_RPT_SET_VOLUME_UP(s)	   (s)[0] &= HID_CC_RPT_VOLUME_BITS;    (s)[0] |= HID_CC_RPT_VOLUME_UP
#define HID_CC_RPT_SET_VOLUME_DOWN(s)  (s)[0] &= HID_CC_RPT_VOLUME_BITS;    (s)[0] |= HID_CC_RPT_VOLUME_DOWN
#define HID_CC_RPT_SET_BUTTON(s, x)	   (s)[1] &= HID_CC_RPT_BUTTON_BITS;    (s)[1] |= (x)
#define HID_CC_RPT_SET_SELECTION(s, x) (s)[1] &= HID_CC_RPT_SELECTION_BITS; (s)[1] |= ((x) & 0x03) << 4

#define HID_CC_IN_RPT_LEN 2 // 2-byte CC report

extern volatile bool bluetooth_connected;

/* BLE HID state */
typedef struct {
	TaskHandle_t task_hdl;
	esp_hidd_dev_t *hid_dev;
	uint8_t protocol_mode;
} ble_hid_param_t;

static ble_hid_param_t ble_hid_param = {0};

/* COMPOSITE REPORT MAP */
// 	- Keyboard (Boot) on Report ID 1 (8-byte input, 1-byte LED out)
// 	- Consumer Control on Report ID 3 (2-byte payload: bits + 4-bit array)
const uint8_t data_report_map[] = {
	/* Keyboard (Report ID 1) */
	0x05, 0x01,					// Usage Page (Generic Desktop)
	0x09, 0x06,					// Usage (Keyboard)
	0xA1, 0x01,					// Collection (Application)
	0x85, HID_RPT_ID_KB_IN,		// Report ID (1)

	// Modifier bits (8 x 1-bit)
	0x05, 0x07,					// Usage Page (Key Codes)
	0x19, 0xE0,				// Usage Minimum (LeftControl)
	0x29, 0xE7,				// Usage Maximum (Right GUI)
	0x15, 0x00,				// Logical Minimum (0)
	0x25, 0x01,				// Logical Maximum (1)
	0x75, 0x01,				// Report Size (1)
	0x95, 0x08,				// Report Count (8)
	0x81, 0x02,				// Input (Data,Var,Abs)

	// Reserved byte
	0x75, 0x08,				// Report Size (8)
	0x95, 0x01,				// Report Count (1)
	0x81, 0x01,				// Input (Const,Array,Abs)

	// 6-key rollover array
	0x05, 0x07,				// Usage Page (Key Codes)
	0x19, 0x00,				// Usage Minimum (0)
	0x29, 0x65,				// Usage Maximum (101)
	0x15, 0x00,				// Logical Minimum (0)
	0x25, 0x65,				// Logical Maximum (101)
	0x75, 0x08,				// Report Size (8)
	0x95, 0x06,				// Report Count (6)
	0x81, 0x00,				// Input (Data,Array,Abs)

	// LED Output report (Num/Caps/Scroll/Kana, with padding)
	0x05, 0x08,				// Usage Page (LEDs)
	0x19, 0x01,				// Usage Minimum (Num Lock)
	0x29, 0x05,				// Usage Maximum (Kana)
	0x15, 0x00,				// Logical Minimum (0)
	0x25, 0x01,				// Logical Maximum (1)
	0x75, 0x01,				// Report Size (1)
	0x95, 0x05,				// Report Count (5)
	0x91, 0x02,				// Output (Data,Var,Abs)
	0x75, 0x03,				// Report Size (3)
	0x95, 0x01,				// Report Count (1)
	0x91, 0x01,				// Output (Const,Array,Abs)

	0xC0,							// End Collection (Keyboard)

	/* Consumer Control (Report ID 3) */
	// Canonical single 16-bit Consumer usage report
	0x05, 0x0C,				// Usage Page (Consumer)
	0x09, 0x01,				// Usage (Consumer Control)
	0xA1, 0x01,				// Collection (Application)
	0x85, HID_RPT_ID_CC_IN,	// Report ID (3)

	// Declare selectable usage range (and match value range)
	0x19, 0x00,				// Usage Minimum (0)
	0x2A, 0x9C, 0x02,	// Usage Maximum (0x029C)
	0x15, 0x00,				// Logical Minimum (0)
	0x26, 0x9C, 0x02,	// Logical Maximum (0x029C)

	// One 16-bit array entry = one Consumer usage (e.g., 0x00E9 for Vol+)
	0x75, 0x10,				// Report Size (16)
	0x95, 0x01,				// Report Count (1)
	0x81, 0x00,				// Input (Data,Array,Abs)

	0xC0							// End Collection (Consumer Control)
};

// One combined raw map
static esp_hid_raw_report_map_t ble_report_maps[] = {
	{ .data = data_report_map, .len = sizeof(data_report_map) }
};

static esp_hid_device_config_t ble_hid_config = {
	.vendor_id = 0x16C0,
	.product_id = 0x05DF,
	.version = 0x0100,
	.device_name = DEVICE_NAME,
	.manufacturer_name = "RoboticWorx",
	.serial_number = "1234567890",
	.report_maps = ble_report_maps,
	.report_maps_len = 1
};

// Sends a single Consumer Control usage (press or release)
static inline void cc_send_usage(uint16_t usage, bool key_pressed)
{
	uint8_t rpt[2];
	if (key_pressed) {
		rpt[0] = (uint8_t)(usage & 0xFF); // LSB first
		rpt[1] = (uint8_t)((usage >> 8) & 0xFF);
	}
	else {
		rpt[0] = 0x00; rpt[1] = 0x00; // Release: no usage
	}

	// Map_index=0 because we have one raw map; Report ID = HID_RPT_ID_CC_IN (3)
	esp_hidd_dev_input_set(ble_hid_param.hid_dev, 0, HID_RPT_ID_CC_IN, rpt, sizeof(rpt));
}

void bluetooth_send_cmd(uint8_t key_cmd, bool key_pressed)
{
	uint16_t usage = 0;

	switch (key_cmd) {
		// Volume
		case BLUETOOTH_CMD_VOLUME_UP:
			usage = 0x00E9;
			break; // Volume Increment
		case BLUETOOTH_CMD_VOLUME_DOWN:
			usage = 0x00EA;
			break; // Volume Decrement
		case BLUETOOTH_CMD_MUTE:
			usage = 0x00E2;
			break; // Mute

		// Transport
		case BLUETOOTH_CMD_PLAY:
			usage = 0x00B0;
			break; // Play
		case BLUETOOTH_CMD_PAUSE:
			usage = 0x00B1;
			break; // Pause
		case BLUETOOTH_CMD_RECORD:
			usage = 0x00B2;
			break; // Record
		case BLUETOOTH_CMD_FAST_FORWARD:
			usage = 0x00B3;
			break; // Fast Forward
		case BLUETOOTH_CMD_REWIND:
			usage = 0x00B4;
			break; // Rewind
		case BLUETOOTH_CMD_SCAN_NEXT_TRK:
			usage = 0x00B5;
			break; // Next
		case BLUETOOTH_CMD_SCAN_PREV_TRK:
			usage = 0x00B6;
			break; // Previous
		case BLUETOOTH_CMD_STOP:
			usage = 0x00B7;
			break; // Stop
		case BLUETOOTH_CMD_PLAY_PAUSE:
			usage = 0x00CD;
			break; // Play/Pause (toggle)

		// Power/menu
		case BLUETOOTH_CMD_POWER:
			usage = 0x0030;
			break; // Power
		case BLUETOOTH_CMD_MENU:
			usage = 0x0040;
			break; // Menu
		default:
			usage = 0;
			break;
	}

	if (usage) {
		#ifdef POLYCAST5_DEBUG
			uint8_t dbg[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
			ESP_LOG_BUFFER_HEX("HID_CC_USAGE", dbg, 2);
		#endif

		cc_send_usage(usage, key_pressed);
	}
}

/* Keyboard helpers */
#define HID_KB_IN_RPT_LEN 8

// Wrap esp_hidd_dev_input_set; it wants a non-const buffer
static inline void hid_input_send(uint8_t rpt_id, const uint8_t *data, size_t len)
{
	esp_hidd_dev_input_set(ble_hid_param.hid_dev, 0, rpt_id, (uint8_t *)data, len);
}

void bluetooth_kbd_send_raw(uint8_t modifiers, const uint8_t keys[6])
{
	uint8_t rpt[HID_KB_IN_RPT_LEN] = {0};
	rpt[0] = modifiers;
	for (int i = 0; i < 6; ++i) {
		rpt[2 + i] = keys ? keys[i] : 0;
	}

	hid_input_send(HID_RPT_ID_KB_IN, rpt, sizeof(rpt));
}

void bluetooth_kbd_release_all(void)
{
	uint8_t rpt[HID_KB_IN_RPT_LEN] = {0};
	hid_input_send(HID_RPT_ID_KB_IN, rpt, sizeof(rpt));
}

static bool ascii_to_hid(char c, uint8_t *mod, uint8_t *kc)
{
	*mod = 0;

	// Letters
	if (c >= 'a' && c <= 'z') {
		*kc = HID_KC_A + (c - 'a');
		return true;
	}
	if (c >= 'A' && c <= 'Z') {
		*kc = HID_KC_A + (c - 'A');
		*mod = MOD_LSHIFT;
		return true;
	}

	// Digits (unshifted)
	if (c >= '1' && c <= '9') {
		*kc = 0x1E + (c - '1');
		return true;
	} // 1..9
	if (c == '0') {
		*kc = 0x27;
		return true;
	} // 0

	// Whitespace / control
	if (c == ' ') {
		*kc = HID_KC_SPACE;
		return true;
	}
	if (c == '\n' || c == '\r') {
		*kc = HID_KC_ENTER;
		return true;
	}
	if (c == '\t') {
		*kc = HID_KC_TAB;
		return true;
	}
	if (c == '\b') {
		*kc = HID_KC_BSPACE;
		return true;
	}

	// Number row shifted symbols
	switch (c) {
		case '!':
			*kc = 0x1E;
			*mod = MOD_LSHIFT;
			return true; // Shift+1
		case '@':
			*kc = 0x1F;
			*mod = MOD_LSHIFT;
			return true; // Shift+2
		case '#':
			*kc = 0x20;
			*mod = MOD_LSHIFT;
			return true; // Shift+3
		case '$':
			*kc = 0x21;
			*mod = MOD_LSHIFT;
			return true; // Shift+4
		case '%':
			*kc = 0x22;
			*mod = MOD_LSHIFT;
			return true; // Shift+5
		case '^':
			*kc = 0x23;
			*mod = MOD_LSHIFT;
			return true; // Shift+6
		case '&':
			*kc = 0x24;
			*mod = MOD_LSHIFT;
			return true; // Shift+7
		case '*':
			*kc = 0x25;
			*mod = MOD_LSHIFT;
			return true; // Shift+8
		case '(':
			*kc = 0x26;
			*mod = MOD_LSHIFT;
			return true; // Shift+9
		case ')':
			*kc = 0x27;
			*mod = MOD_LSHIFT;
			return true; // Shift+0
	}

	// Punctuation keys (with shift variants)
	switch (c) {
		case '-':
			*kc = HID_KC_MINUS;
			return true;
		case '_':
			*kc = HID_KC_MINUS;
			*mod = MOD_LSHIFT;
			return true;

		case '=':
			*kc = HID_KC_EQUAL;
			return true;
		case '+':
			*kc = HID_KC_EQUAL;
			*mod = MOD_LSHIFT;
			return true;

		case '[':
			*kc = HID_KC_LBRACKET;
			return true;
		case '{':
			*kc = HID_KC_LBRACKET;
			*mod = MOD_LSHIFT;
			return true;

		case ']':
			*kc = HID_KC_RBRACKET;
			return true;
		case '}':
			*kc = HID_KC_RBRACKET;
			*mod = MOD_LSHIFT;
			return true;

		case '\\':
			*kc = HID_KC_BACKSLASH;
			return true;
		case '|':
			*kc = HID_KC_BACKSLASH;
			*mod = MOD_LSHIFT;
			return true;

		case ';':
			*kc = HID_KC_SEMICOLON;
			return true;
		case ':':
			*kc = HID_KC_SEMICOLON;
			*mod = MOD_LSHIFT;
			return true;

		case '\'':
			*kc = HID_KC_APOSTROPHE;
			return true;
		case '\"':
			*kc = HID_KC_APOSTROPHE;
			*mod = MOD_LSHIFT;
			return true;

		case '`':
			*kc = HID_KC_GRAVE;
			return true;
		case '~':
			*kc = HID_KC_GRAVE;
			*mod = MOD_LSHIFT;
			return true;

		case ',':
			*kc = HID_KC_COMMA;
			return true;
		case '<':
			*kc = HID_KC_COMMA;
			*mod = MOD_LSHIFT;
			return true;

		case '.':
			*kc = HID_KC_DOT;
			return true;
		case '>':
			*kc = HID_KC_DOT;
			*mod = MOD_LSHIFT;
			return true;

		case '/':
			*kc = HID_KC_SLASH;
			return true;
		case '?':
			*kc = HID_KC_SLASH;
			*mod = MOD_LSHIFT;
			return true;
	}

	// Not mapped
	return false;
}

bool bluetooth_kbd_type_char(char c, uint32_t tap_ms)
{
	uint8_t mod, kc;
	if (!ascii_to_hid(c, &mod, &kc)) {
		return false;
	}

	uint8_t keys[6] = { kc, 0,0,0,0,0 };

	bluetooth_kbd_send_raw(mod, keys);
	vTaskDelay(pdMS_TO_TICKS(tap_ms));
	bluetooth_kbd_release_all();
	vTaskDelay(pdMS_TO_TICKS(tap_ms));

	return true;
}

void bluetooth_kbd_type_string(const char *s, uint32_t tap_ms)
{
	if (!s) {
		return;
	}

	while (*s) {
		bluetooth_kbd_type_char(*s++, tap_ms);
	}
}

/* HID events / init / deinit */
static void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
	esp_hidd_event_t event = (esp_hidd_event_t)id;

	(void)handler_args;
	(void)base;
	(void)event_data; // Silence unused warnings

	switch (event) {
		case ESP_HIDD_START_EVENT:
			esp_hid_ble_gap_adv_start();
			break;
		case ESP_HIDD_CONNECT_EVENT: {
			// RGB indicator
			uint8_t rgb_state = RGB_SET_BLUE;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);

			bluetooth_connected = true;
			break;
		}
		case ESP_HIDD_DISCONNECT_EVENT: {
			// RGB indicator
			uint8_t rgb_state = RGB_SET_OFF;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);

			bluetooth_connected = false;
			esp_hid_ble_gap_adv_start();
			break;
		}
		default:
			break;
	}
}

static void ble_hid_device_host_task(void *param)
{
	nimble_port_run();
	nimble_port_freertos_deinit();
}

// Declaration of extern esp function
void ble_store_config_init(void);

void bluetooth_init(void)
{
	esp_err_t ret;

	static bool gap_inited_once = false;

	if (!gap_inited_once) {
		ret = esp_hid_gap_init(HID_DEV_MODE);
		ESP_ERROR_CHECK(ret);

		ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, DEVICE_NAME);
		ESP_ERROR_CHECK(ret);

		gap_inited_once = true;
	}

	ret = esp_hidd_dev_init(&ble_hid_config, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &ble_hid_param.hid_dev);
	ESP_ERROR_CHECK(ret);

	ble_store_config_init();
	ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

	ret = esp_nimble_enable(ble_hid_device_host_task);
	if (ret) {
		ESP_LOGE(TAG, "esp_nimble_enable failed: %d", ret);
	}
}

void bluetooth_deinit(void)
{
	ble_gap_adv_stop();

	if (nimble_port_stop()) {
		ESP_LOGE(TAG, "nimble_port_stop failed");
		return;
	}

	if (nimble_port_deinit() != ESP_OK) {
		ESP_LOGE(TAG, "nimble_port_deinit failed");
		return;
	}

	if (ble_hid_param.hid_dev) {
		esp_hidd_dev_deinit(ble_hid_param.hid_dev);
		ble_hid_param.hid_dev = NULL;
	}

	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Bluetooth fully disabled");
	#endif
}
