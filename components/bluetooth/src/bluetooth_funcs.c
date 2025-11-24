#include "polycast5_macros.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/projdefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "nvs.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/bas/ble_svc_bas.h"
#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "esp_log.h"
#include "esp_err.h"

#include "bluetooth_funcs.h"
#include "gpio_funcs.h"
#include "gpio_task.h"

#define TAG "BLUETOOTH_FUNCS"

// Note: Security Level 2 in menuconfig 'BLE SM' required for iOS pairing!
#define DEVICE_NAME "PolyCast5"

#define PAIRING_KEY_NS "bt_key"
#define PAIRING_KEY_KEY "key"

// Bond index (NVS-only; UI uses this while BT is OFF)
#define BT_IDX_NS "bt_index"
#define BT_IDX_KEY "peers"

#define BT_PEERS_NS "bt_peers"
#define BT_PEERS_KEY "peers"
#define BT_PEERS_PERF_KEY "pref_peer"

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
#define HID_CC_RPT_SET_VOLUME_UP(s)	   (s)[0] &= HID_CC_RPT_VOLUME_BITS;	(s)[0] |= HID_CC_RPT_VOLUME_UP
#define HID_CC_RPT_SET_VOLUME_DOWN(s)  (s)[0] &= HID_CC_RPT_VOLUME_BITS;	(s)[0] |= HID_CC_RPT_VOLUME_DOWN
#define HID_CC_RPT_SET_BUTTON(s, x)	   (s)[1] &= HID_CC_RPT_BUTTON_BITS;	(s)[1] |= (x)
#define HID_CC_RPT_SET_SELECTION(s, x) (s)[1] &= HID_CC_RPT_SELECTION_BITS; (s)[1] |= ((x) & 0x03) << 4

#define HID_CC_IN_RPT_LEN 2 // 2-byte CC report

volatile bluetooth_state_t bluetooth_state = BT_STATE_OFF;

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

static uint8_t bt_mod_state = 0;
static uint8_t bt_keys_state[6] = {0};

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

void bluetooth_send_media(uint8_t cmd, bool key_pressed)
{
	uint16_t usage = 0;

	switch (cmd) {
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
		case BLUETOOTH_CMD_NEXT_TRK:
			usage = 0x00B5;
			break; // Next
		case BLUETOOTH_CMD_PREV_TRK:
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

static void kbd_send_raw(uint8_t modifiers, const uint8_t keys[6])
{
	uint8_t rpt[HID_KB_IN_RPT_LEN] = {0};
	rpt[0] = modifiers;
	for (int i = 0; i < 6; ++i) {
		rpt[2 + i] = keys ? keys[i] : 0;
	}

	hid_input_send(HID_RPT_ID_KB_IN, rpt, sizeof(rpt));
}

static void kbd_release_all(void)
{
	uint8_t rpt[HID_KB_IN_RPT_LEN] = {0};
	hid_input_send(HID_RPT_ID_KB_IN, rpt, sizeof(rpt));
}

static void kbd_state_clear(void)
{
	bt_mod_state = 0;
	memset(bt_keys_state, 0, sizeof(bt_keys_state));
	kbd_release_all(); // Aends an all-zero report
}

// Add modifiers + keys into the current state and send
static void kbd_state_add(uint8_t mods, const uint8_t *keys, size_t nkeys)
{
	bt_mod_state |= mods;

	for (size_t i = 0; i < nkeys; ++i) {
		uint8_t kc = keys[i];
		if (!kc) {
			continue;
		}

		// Skip if already present
		bool already = false;
		for (int j = 0; j < 6; ++j) {
			if (bt_keys_state[j] == kc) {
				already = true;
				break;
			}
		}
		if (already) {
			continue;
		}

		// Insert into first empty slot if available
		for (int j = 0; j < 6; ++j) {
			if (bt_keys_state[j] == 0) {
				bt_keys_state[j] = kc;
				break;
			}
		}
	}

	// Send the current state as one HID report
	kbd_send_raw(bt_mod_state, bt_keys_state);
}

// Remove modifiers + keys from the current state and send
static void kbd_state_remove(uint8_t mods, const uint8_t *keys, size_t nkeys)
{
	bt_mod_state &= (uint8_t)~mods;

	for (size_t i = 0; i < nkeys; ++i) {
		uint8_t kc = keys[i];
		if (!kc) {
			continue;
		}

		for (int j = 0; j < 6; ++j) {
			if (bt_keys_state[j] == kc) {
				bt_keys_state[j] = 0;
				break;
			}
		}
	}

	// Send the current state as one HID report
	kbd_send_raw(bt_mod_state, bt_keys_state);
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
	if (c >= '1' && c <= '9') { // 1..9
		*kc = 0x1E + (c - '1');
		return true;
	}
	if (c == '0') { // 0
		*kc = 0x27;
		return true;
	}

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

static bool kbd_type_char(char c, uint32_t tap_ms)
{
	// Exit if char not mapped
	uint8_t mod, kc;
	if (!ascii_to_hid(c, &mod, &kc)) {
		return false;
	}

	uint8_t keys[6] = {kc, 0,0,0,0,0};

	kbd_send_raw(mod, keys);
	vTaskDelay(pdMS_TO_TICKS(tap_ms));
	kbd_release_all();
	vTaskDelay(pdMS_TO_TICKS(tap_ms));

	return true;
}

// Case-insensitive strcmp
static int icmp(const char *a, const char *b)
{
	while (*a && *b) {
		int da = tolower((unsigned char)*a++);
		int db = tolower((unsigned char)*b++);

		if (da != db) {
			return da - db;
		}
	}

	return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

// Map common key names used in tags to HID keycodes.
// Returns true on success and writes *kc
static bool key_name_to_hid(const char *name, uint8_t *kc)
{
	// Exit if NULL
	if (!name || !kc) {
		return false;
	}

	// Possible <...> commands
	if (icmp(name, "enter") == 0 || icmp(name, "return") == 0) {
		*kc = HID_KC_ENTER;
		return true;
	}
	if (icmp(name, "tab") == 0) {
		*kc = HID_KC_TAB;
		return true;
	}
	if (icmp(name, "esc") == 0 || icmp(name, "escape") == 0) {
		*kc = HID_KC_ESC;
		return true;
	}
	if (icmp(name, "space") == 0 || icmp(name, "spacebar") == 0) {
		*kc = HID_KC_SPACE;
		return true;
	}
	if (icmp(name, "backspace") == 0 || icmp(name, "bs") == 0) {
		*kc = HID_KC_BACKSPACE;
		return true;
	}
	if (icmp(name, "del") == 0) {
		*kc = HID_KC_DELETE;
		return true;
	}
	if (icmp(name, "up") == 0) {
		*kc = HID_KC_UP;
		return true;
	}
	if (icmp(name, "down") == 0) {
		*kc = HID_KC_DOWN;
		return true;
	}
	if (icmp(name, "left") == 0) {
		*kc = HID_KC_LEFT;
		return true;
	}
	if (icmp(name, "right") == 0) {
		*kc = HID_KC_RIGHT;
		return true;
	}
	if (icmp(name, "home") == 0) {
		*kc = HID_KC_HOME;
		return true;
	}
	if (icmp(name, "end") == 0) {
		*kc = HID_KC_END;
		return true;
	}
	if (icmp(name, "pgup") == 0 || icmp(name, "pageup") == 0) {
		*kc = HID_KC_PGUP;
		return true;
	}
	if (icmp(name, "pgdn") == 0 || icmp(name, "pagedown") == 0) {
		*kc = HID_KC_PGDN;
		return true;
	}

	// F1-F24
	if ((name[0] == 'f' || name[0] == 'F') && isdigit((unsigned char)name[1])) {
		long fn = strtol(name + 1, NULL, 10);

		if (fn >= 1 && fn <= 12) {
			*kc = (uint8_t)(HID_KC_F1 + (fn - 1));
			return true;
		}
	}

	// Single ASCII letter/digit/symbol - let your existing ASCII->HID path handle it
	// Report false here so the caller can fall back to ascii_to_hid()
	return false;
}

// Compute modifier bitfield from tokens like "ctrl", "shift", "alt", "gui", "win", "cmd".
static uint8_t mod_from_word(const char *w)
{
	// Exit if NULL
	if (!w) {
		return 0;
	}

	// Look for command
	if (!icmp(w, "ctrl") || !icmp(w, "control") || !icmp(w, "ctl")) {
		return MOD_LCTRL;
	}
	if (!icmp(w, "shift")) {
		return MOD_LSHIFT;
	}
	if (!icmp(w, "alt") || !icmp(w, "option") || !icmp(w, "opt")) {
		return MOD_LALT;
	}
	if (!icmp(w, "gui") || !icmp(w, "win")	|| !icmp(w, "cmd") || !icmp(w, "meta")) {
		return MOD_LGUI;
	}

	return 0;
}

// Lets users write things like <ctrl + d + s> with spaces
static char *trim_tok(char *s)
{
	// Advance s past any leading space/tab/newline
	while (*s && (unsigned char)*s <= ' ') {
		++s;
	}
	
	// Find the current (left-trimmed) end of the string
	char *e = s + strlen(s);

	// Walk backward from the end, turning trailing whitespace into '\0'
	while (e > s && (unsigned char)e[-1] <= ' ') {
		*--e = '\0';
	}
	
	return s;
}

// Press up to 6 keys with optional modifiers, hold for hold_ms, then release
// After release, wait tap_ms to preserve existing pacing
static void send_chord(uint8_t modifiers, const uint8_t *keycodes, size_t nkeys, uint32_t hold_ms, uint32_t tap_ms)
{
	// HID keyboards support up to 6 simultaneous non-modifier keys per report
	uint8_t keys[6] = {0};

	// Safety clamp so we never overflow the HID array
	if (nkeys > 6) {
		nkeys = 6;
	}

	// Copy the requested chord into the HID rollover buffer
	for (size_t i = 0; i < nkeys; ++i) {
		keys[i] = keycodes[i];
	}

	// Send
	kbd_send_raw(modifiers, keys);
	vTaskDelay(pdMS_TO_TICKS(hold_ms));
	kbd_release_all();
	vTaskDelay(pdMS_TO_TICKS(tap_ms));
}

// Parse a <...+...> or <...> style token
// Returns true if it consumed a token and sent it
// *consumed_end points to the closing '>' or NULL if none
static bool parse_and_send_tag(const char *start, const char **consumed_end, uint32_t tap_ms) {
	// Start points at the '<'
	const char *gt = strchr(start, '>'); // Points to first occurrence of '>'
	if (!gt) {
		*consumed_end = NULL;
		return false; // No closing '>'
	}

	// Extract inside text (without < and >)
	size_t len = (size_t)(gt - (start + 1));
	if (len == 0) {
		*consumed_end = gt;
		return true; // Empty tag
	}

	char tmp[64]; // Buf

	// Clamp
	if (len >= sizeof(tmp)) {
		len = sizeof(tmp) - 1;
	}

	memcpy(tmp, start + 1, len);
	tmp[len] = '\0';

	// If is <delay=ms>
	if (!strncasecmp(tmp, "delay=", 6)) { // Compares while ignoring differences in case
		long ms = strtol(tmp + 6, NULL, 10); // Converts a string to a long int

		// Min is 0
		if (ms < 0) {
			ms = 0;
		}

		// Delay that amount (blocks bluetooth_task)
		vTaskDelay(pdMS_TO_TICKS((uint32_t)ms));

		*consumed_end = gt;
		return true;
	}

	// If is <hold:x=x>
	const uint32_t DEFAULT_HOLD_MS = 50; // Used when hold has no explicit duration
	bool is_hold = false;
	uint32_t hold_ms = DEFAULT_HOLD_MS;
	char *payload = tmp; // Points to the chord text that we'll parse into modifiers/keys

	// Flags for key up/down
	bool is_down = false;
	bool is_up = false;

	// If is <hold:chord=ms>
	if (!strncasecmp(tmp, "hold:", 5)) {
		is_hold = true; // Use hold_ms
		payload = tmp + 5; // Start of chord

		// Look for trailing "=ms"
		char *eq = strrchr(payload, '=');
		if (eq) {
			// Points to characters after '='
			char *ms_start = eq + 1;
			ms_start = trim_tok(ms_start); // Remove any spaces

			char *endptr = NULL;

			// Read base-10 int
			long ms = strtol(ms_start, &endptr, 10);

			// If digit was found
			if (endptr != ms_start) {
				// Clamp at min and max
				if (ms < 0) {
					ms = 0;
				}
				if (ms > 10000000) { // ~6.9 days
					ms = 10000000;
				}

				hold_ms = (uint32_t)ms;

				// Strip the "=ms" part from payload and trim spaces before '='
				// Back up over any spaces
				char *e = eq;
				while (e > payload && (unsigned char)e[-1] <= ' ') {
					--e;
				}

				*e = '\0'; // NUL-terminate payload
			}
		}
	}
	// If is <down:chord>
	else if (!strncasecmp(tmp, "down:", 5)) {
		is_down = true;
		payload = tmp + 5;
	}
	// If is <up:chord>
	else if (!strncasecmp(tmp, "up:", 3)) {
		is_up = true;
		payload = tmp + 3;
	}

	/* IF NEEDED LATER:
	// If is <text=x>
	if (!strncasecmp(tmp, "text=", 5)) {
		const char *payload = tmp + 5;

		// Stream via existing per-char typer:
		while (*payload) {
			// Typed via internal helper; see bluetooth_send_string()/kbd_type_char()
			// It presses & releases a single key with delays :contentReference[oaicite:1]{index=1}
			kbd_type_char(*payload++, tap_ms);
		}

		*consumed_end = gt;
		return true;
	}
	*/

	// If is <...+...> / <...>: Split on '+'
	uint8_t mods = 0;
	uint8_t keys[6] = {0};
	size_t nkeys = 0;

	char *save = NULL;

	// Split the chord on '+'
	for (char *tok = strtok_r(payload, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
		// Trim each token for whitespace tolerance
		tok = trim_tok(tok);
		if (!*tok) {
			continue;
		}

		// If the token is a modifier word, OR it into the mods bitfield
		// Modifiers (ctrl, shift, alt, gui/cmd/meta)
		uint8_t add = mod_from_word(tok);
		if (add) {
			mods |= add;
			continue;
		}

		// If it's a named key, convert to its HID usage code and add it to the chord
		// Named keys (enter, tab, esc, home, end, pgup, pgdn, f1...f12, etc.)
		uint8_t kc = 0;
		if (key_name_to_hid(tok, &kc)) {
			if (nkeys < 6) {
				keys[nkeys++] = kc;
			}
			continue;
		}

		// Else: Single ASCII character -> convert to HID
		if (tok[1] == '\0') {
			uint8_t amod = 0, akc = 0;
			if (ascii_to_hid(tok[0], &amod, &akc)) {
				mods |= amod; // Shift for symbols if needed
				if (nkeys < 6) {
					keys[nkeys++] = akc;
				}
			}
		}
	}

	// If we collected at least one key
	if (nkeys > 0 || mods != 0) {
		// If permanent down/up events
		if (is_down) {
			kbd_state_add(mods, keys, nkeys);
			*consumed_end = gt;
			return true;
		}

		if (is_up) {
			kbd_state_remove(mods, keys, nkeys);
			*consumed_end = gt;
			return true;
		}

		// For non-hold tags: use tap_ms as the brief hold
		// For hold tags: use hold_ms
		uint32_t use_hold = is_hold ? hold_ms : tap_ms;

		send_chord(mods, keys, nkeys, use_hold, tap_ms);

		*consumed_end = gt;
		return true;
	}

	// Nothing actionable: do not consume and let caller type '<' literally
	*consumed_end = NULL;
	return false;
}

// Send script
void bluetooth_send_script(const char *script, uint32_t tap_ms)
{
	// Make sure exists
	if (!script) {
		return;
	}

	const char *s = script;

	// Check the script for tokens
	while (*s) {
		// If found command start
		if (*s == '<') {
			const char *gt = NULL;
			
			if (parse_and_send_tag(s, &gt, tap_ms) && gt) {
				s = gt + 1; // Consumed a tag
				continue;
			}
			// Malformed tag: no '>' -> type the '<' literally
		}

		// Ordinary text path, keep typing
		kbd_type_char(*s++, tap_ms);
	}

	// Safety: release anything left down by <down:...> tags
	kbd_state_clear();
}

void bluetooth_set_battery_level(uint8_t percent)
{
	// Cap at max
	if (percent > 100) {
		percent = 100;
	}

	// Updates the 0x2A19 characteristic; if a phone/PC subscribed, NimBLE will notify it
	ble_svc_bas_battery_level_set(percent);
}

void bluetooth_forget_all_peers(void)
{
	int rc;

	// Stop advertising to prevent incoming connections during the reset
	rc = ble_gap_adv_stop();
	if (rc != 0 && rc != BLE_HS_EALREADY) {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "Failed to stop advertising; rc=%d", rc);
		#endif
		// Continue anyway, but aware of potential race conditions
	}

	#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "Deleting Bluetooth peer bonding keys...");
	#endif

	ble_addr_t peers[16];
	int peer_count = 0;

	// Retrieve and delete all bonded peers
	// Also resets device identity (enabled in menuconfig)
	// This makes the device appear as a "new" device so it won't auto-reconnect via BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT
	if (ble_store_util_bonded_peers(peers, &peer_count, (int)(sizeof(peers)/sizeof(peers[0]))) == 0) {
		for (int i = 0; i < peer_count; ++i) {
			rc = ble_gap_unpair(&peers[i]);
			if (rc == 0) {
				#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "Unpaired peer %d", i);
				#endif
			}
			else {
				#ifdef POLYCAST5_DEBUG
				ESP_LOGW(TAG, "Failed to unpair peer %d; rc=%d", i, rc);
				#endif
			}
		}
	}
	else {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "No bonded peers found to delete.");
		#endif
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
			if (bluetooth_state == BT_STATE_RUNNING || bluetooth_state == BT_STATE_INITING) {
				esp_hid_ble_gap_adv_start();
			}

			break;
		case ESP_HIDD_CONNECT_EVENT: {
			// RGB indicator
			uint8_t rgb_state = RGB_SET_BLUE;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);

			bluetooth_set_battery_level(100); // Default start

			break;
		}
		case ESP_HIDD_DISCONNECT_EVENT: {
			// RGB indicator
			uint8_t rgb_state = RGB_SET_OFF;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);

			if (bluetooth_state == BT_STATE_RUNNING || bluetooth_state == BT_STATE_INITING) {
				esp_hid_ble_gap_adv_start();
			}

			break;
		}
		default:
			break;
	}
}

static void ble_hid_device_host_task(void *param)
{
	// This function will return only when nimble_port_stop() is executed 
	nimble_port_run();
	nimble_port_freertos_deinit(); // esp_nimble_disable
}

// Declaration of extern esp function
void ble_store_config_init(void);

/*
	IMPORTANT: To be able to init and deinit Bluetooth correctly as a HID,
	an external patch was applied to ESP-IDF. You may have to apply it also.
	Please see https://github.com/RoboticWorx/PolyCast5/blob/main/components/bluetooth/README.md
*/

void bluetooth_init(void)
{
	#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "bluetooth_init() starting, state=%d", bluetooth_state);
	ESP_LOGI(TAG, "BT controller status before init: %d",
			esp_bt_controller_get_status()); // IDLE, INITED, ENABLED, NUM
	#endif

	// If already on or initing, exit
	if (bluetooth_state == BT_STATE_INITING || bluetooth_state == BT_STATE_RUNNING) {
		return;
	}

	bluetooth_state = BT_STATE_INITING;

	esp_err_t ret;

	ret = esp_hid_gap_init(HID_DEV_MODE);
	ESP_ERROR_CHECK(ret);

	ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, DEVICE_NAME);
	ESP_ERROR_CHECK(ret);

	// Register the standard Battery Service (0x180F)
	ble_svc_bas_init();

	ret = esp_hidd_dev_init(&ble_hid_config, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &ble_hid_param.hid_dev);
	ESP_ERROR_CHECK(ret);

	ble_store_config_init();
	ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

	ret = esp_nimble_enable(ble_hid_device_host_task);
	if (ret) {
		ESP_LOGE(TAG, "esp_nimble_enable failed: %d", ret);
	}

	#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "BT controller status after init: %d",
			esp_bt_controller_get_status()); // IDLE, INITED, ENABLED, NUM
	#endif

	bluetooth_state = BT_STATE_RUNNING;
}

// Declare self-implemented function
extern esp_err_t esp_hid_gap_deinit(void);

void bluetooth_deinit(void)
{
	#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "bluetooth_deinit() starting, state=%d", bluetooth_state);
	ESP_LOGI(TAG, "BT controller status before deinit: %d",
			esp_bt_controller_get_status()); // IDLE, INITED, ENABLED, NUM
	#endif

	// If already off or deiniting, exit
	if (bluetooth_state == BT_STATE_OFF || bluetooth_state == BT_STATE_DEINITING) {
		return;
	}

	bluetooth_state = BT_STATE_DEINITING;

	esp_hid_gap_deinit();

	ble_gap_adv_stop(); // Stop advertising

	ble_gatts_reset();

	if (nimble_port_stop()) {
		ESP_LOGE(TAG, "nimble_port_stop failed");
	}

	if (nimble_port_deinit() != ESP_OK) {
		ESP_LOGE(TAG, "nimble_port_deinit failed");
	}

	if (ble_hid_param.hid_dev) {
		esp_hidd_dev_deinit(ble_hid_param.hid_dev);
		ble_hid_param.hid_dev = NULL;
	}

	#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "Bluetooth fully disabled");
	ESP_LOGI(TAG, "BT controller status after deinit: %d",
			esp_bt_controller_get_status()); // IDLE, INITED, ENABLED, NUM
	#endif

	bluetooth_state = BT_STATE_OFF;
}

/* =============== Known devices =============== */

// List bonded peers (NimBLE store -> our array)
int bluetooth_list_bonded_peers(bluetooth_peer_info_t *out, int max)
{
	// Guard
	if (!out || max <= 0) {
		return 0;
	}

	// Query NimBLE store
	ble_addr_t peers[16];
	int count = 0;
	if (ble_store_util_bonded_peers(peers, &count, (int)(sizeof(peers) / sizeof(peers[0]))) != 0) {
		return 0;
	}

	// Clamp and copy
	if (count > max) {
		count = max;
	}
	for (int i = 0; i < count; i++) {
		out[i].addr = peers[i];
		out[i].label[0] = '\0';
	}

	// Done
	return count;
}

// Load cached peers
int bluetooth_get_peers_list_nvs(bluetooth_peer_info_t *out, int max)
{
	// Guard
	if (!out || max <= 0) {
		return 0;
	}

	// Open NVS
	nvs_handle_t h;
	esp_err_t err = nvs_open(BT_IDX_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		return 0;
	}

	// Read blob
	ble_addr_t tmp[BT_MAX_PEERS] = {0};
	size_t sz = sizeof(tmp);
	err = nvs_get_blob(h, BT_IDX_KEY, tmp, &sz);
	nvs_close(h);

	// No data
	if (err != ESP_OK || sz == 0) {
		return 0;
	}

	// Parse
	int n = (int)(sz / sizeof(ble_addr_t));
	if (n > max) {
		n = max;
	}
	for (int i = 0; i < n; i++) {
		out[i].addr = tmp[i];
		out[i].label[0] = '\0';
	}

	// Done
	return n;
}

// Add peer to cache (idempotent; ring if full)
void bluetooth_add_to_peers_list_nvs(const ble_addr_t *peer)
{
	// Guard
	if (!peer) {
		return;
	}

	// Open NVS
	nvs_handle_t h;
	if (nvs_open(BT_IDX_NS, NVS_READWRITE, &h) != ESP_OK) {
		return;
	}

	// Read existing
	ble_addr_t tmp[BT_MAX_PEERS] = {0};
	size_t sz = sizeof(tmp);
	if (nvs_get_blob(h, BT_IDX_KEY, tmp, &sz) != ESP_OK) {
		sz = 0;
	}

	// Current count
	int n = (int)(sz / sizeof(ble_addr_t));

	// De-dup
	for (int i = 0; i < n; i++) {
		if (tmp[i].type == peer->type && memcmp(tmp[i].val, peer->val, 6) == 0) {
			nvs_close(h);
			return;
		}
	}

	// Append or rotate
	if (n < BT_MAX_PEERS) {
		tmp[n++] = *peer;
	}
	else {
		memmove(&tmp[0], &tmp[1], (BT_MAX_PEERS - 1) * sizeof(ble_addr_t));
		tmp[BT_MAX_PEERS - 1] = *peer;
	}

	// Write back
	if (nvs_set_blob(h, BT_IDX_KEY, tmp, n * sizeof(ble_addr_t)) == ESP_OK) {
		(void)nvs_commit(h);
	}
	nvs_close(h);
}

// Clear all nvs peers
esp_err_t bluetooth_clear_peers_list_nvs(bool preferred_only)
{
	nvs_handle_t h;
	esp_err_t err;

	// If clearing both
	if (!preferred_only) {
		// Open BT_IDX_NS NVS
		err = nvs_open(BT_IDX_NS, NVS_READWRITE, &h);
		if (err != ESP_OK) {
			return err;
		}
	
		// Erase key
		err = nvs_erase_key(h, BT_IDX_KEY);
	
		// Commit 
		if (err == ESP_OK) {
			err = nvs_commit(h);
		}
	
		nvs_close(h);
	}

	// Else only preferred peer
	// Open BT_PEERS_NS NVS
	err = nvs_open(BT_PEERS_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return err;
	}

	// Erase key
	err = nvs_erase_key(h, BT_PEERS_KEY);
	err = nvs_erase_key(h, BT_PEERS_PERF_KEY);

	// Commit and close
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}

	nvs_close(h);

	// Done
	return err;
}

// Save preferred peer (type + 6 bytes) to NVS
esp_err_t bluetooth_set_preferred_peer_nvs(const ble_addr_t *peer)
{
	// Open NVS
	nvs_handle_t h;
	esp_err_t err = nvs_open(BT_PEERS_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return err;
	}

	// Write blob
	uint8_t blob[7] = { peer->type, peer->val[0], peer->val[1], peer->val[2], peer->val[3], peer->val[4], peer->val[5] };
	err = nvs_set_blob(h, BT_PEERS_PERF_KEY, blob, sizeof(blob));

	// Commit and close
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	nvs_close(h);

	// Done
	return err;
}

// Load preferred peer from NVS
esp_err_t bluetooth_get_preferred_peer_nvs(ble_addr_t *out, bool *found)
{
	// Default
	if (found) {
		*found = false;
	}

	// Open NVS
	nvs_handle_t h;
	size_t sz = 7;
	uint8_t blob[7] = {0};
	esp_err_t err = nvs_open(BT_PEERS_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		return err;
	}

	// Read
	err = nvs_get_blob(h, BT_PEERS_PERF_KEY, blob, &sz);
	nvs_close(h);

	// Parse blob
	if (err == ESP_OK && sz == 7) {
		out->type = blob[0];
		memcpy(out->val, &blob[1], 6);
		if (found) {
			*found = true;
		}
	}

	// Done
	return err;
}

/* =============== Pairing key =============== */

esp_err_t bluetooth_pairing_key_save_nvs(uint32_t key)
{
	nvs_handle_t h;
	esp_err_t err;
	
	// Open NVS
	err = nvs_open(PAIRING_KEY_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return err;
	}
	
	// Set the bt pairing key
	err = nvs_set_u32(h, PAIRING_KEY_KEY, key);
	
	// Persist changes if success
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	
	// Close and return
	nvs_close(h);
	return err;
}

esp_err_t bluetooth_pairing_key_load_nvs(uint32_t *key)
{
	nvs_handle_t h;
	esp_err_t err;
	
	// Open NVS
	err = nvs_open(PAIRING_KEY_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		return err;
	}
	
	// Get the saved bt pairing key
	err = nvs_get_u32(h, PAIRING_KEY_KEY, key);
	
	// Close and return
	nvs_close(h);
	return err;
}

/* =============== Get name label =============== */

static void bt_label_key_from_addr(const ble_addr_t *a, char *out, size_t out_sz)
{
	snprintf(out, out_sz, "%02X%02X%02X%02X%02X%02X",
			a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

esp_err_t bluetooth_set_peer_label_nvs(const ble_addr_t *addr, const char *label)
{
	// Guard
	if (!addr) {
		return ESP_ERR_INVALID_ARG;
	}

	nvs_handle_t h;
	esp_err_t err;

	// Open NVS
	err = nvs_open(BT_PEERS_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return err;
	}

	// Write or erase
	char key[20];
	bt_label_key_from_addr(addr, key, sizeof(key));
	if (label && label[0]) {
		// Write label into address key
		err = nvs_set_str(h, key, label);
	}
	else {
		// Erase if DNE
		err = nvs_erase_key(h, key);
		if (err == ESP_ERR_NVS_NOT_FOUND) {
			err = ESP_OK;
		}
	}

	// Commit on success
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}

	// Close and return
	nvs_close(h);
	return err;
}

bool bluetooth_get_peer_label_nvs(const ble_addr_t *addr, char *out, size_t out_sz)
{
	// Guard
	if (!addr || !out || out_sz == 0) {
		return false;
	}

	nvs_handle_t h;

	// Open NVS
	if (nvs_open(BT_PEERS_NS, NVS_READONLY, &h) != ESP_OK) {
		return false;
	}

	// Read address into label
	char key[20];
	bt_label_key_from_addr(addr, key, sizeof(key));

	// Use address key to get string
	size_t sz = out_sz;
	esp_err_t err = nvs_get_str(h, key, out, &sz);

	// Close
	nvs_close(h);

	// Normalize result
	if (err == ESP_OK) {
		out[out_sz - 1] = '\0';
		return true;
	}

	out[0] = '\0';
	return false;
}

esp_err_t bluetooth_remove_peer_nvs(const ble_addr_t *addr)
{
	// Guard
	if (!addr) {
		return ESP_ERR_INVALID_ARG;
	}

	// Remove from index cache (BT_IDX_NS / BT_IDX_KEY)
	nvs_handle_t h;
	esp_err_t err = nvs_open(BT_IDX_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return err;
	}

	ble_addr_t tmp[BT_MAX_PEERS] = {0};
	size_t sz = sizeof(tmp);
	if (nvs_get_blob(h, BT_IDX_KEY, tmp, &sz) != ESP_OK) {
		sz = 0; // Treat as empty list
	}

	// Number of valid entries currently stored in the blob
	int n = (int)(sz / sizeof(ble_addr_t));
	
	// Write index for our compacted array after removing the target addr
	int out = 0;
	
	// Walk the current list and copy forward every entry that is NOT the one we're deleting
	for (int i = 0; i < n; ++i) {
		// Match if both address type (public/random) and 6-byte MAC are identical
		bool same = (tmp[i].type == addr->type) && (memcmp(tmp[i].val, addr->val, 6) == 0);
	
		// Keep only non-matching entries
		if (!same) {
			tmp[out++] = tmp[i];
		}
	}
	
	// Write the compacted list back to NVS
	// If there are still entries, overwrite the blob with the first 'out' elements
	if (out > 0) {
		err = nvs_set_blob(h, BT_IDX_KEY, tmp, out * sizeof(ble_addr_t));
	}
	// Otherwise, no entries remain: erase the key entirely to avoid empty blobs
	else {
		err = nvs_erase_key(h, BT_IDX_KEY);
		// Erasing a non-existent key isn't an error
		if (err == ESP_ERR_NVS_NOT_FOUND) {
			err = ESP_OK;
		}
	}

	// Commit on success
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}

	// Close
	nvs_close(h);

	if (err != ESP_OK) {
		return err;
	}

	// Drop per-address label
	bluetooth_set_peer_label_nvs(addr, NULL); // Passing NULL erases the label key

	// Clear preferred if it matches the removed peer
	ble_addr_t pref = {0};
	bool found = false;
	bluetooth_get_preferred_peer_nvs(&pref, &found);
	if (found && pref.type == addr->type && memcmp(pref.val, addr->val, 6) == 0) {
		bluetooth_clear_peers_list_nvs(true); // Only delete BT_PEERS_KEY
	}

	// Unpair from NimBLE so it won't auto-reconnect
	ble_gap_unpair((ble_addr_t *)addr);

	return ESP_OK;
}