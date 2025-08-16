#ifndef BLUETOOTH_FUNCS_H
#define BLUETOOTH_FUNCS_H

#include <stdbool.h>
#include <stdint.h>

// Report IDs
#define HID_RPT_ID_KB_IN     1   // Keyboard input (8 bytes)
#define HID_RPT_ID_CC_IN     3   // Consumer Control input (2 bytes)  <-- keep 3 to match sender

// Keyboard modifier bits
#define MOD_LCTRL (1<<0)
#define MOD_LSHIFT (1<<1)
#define MOD_LALT (1<<2)
#define MOD_LGUI (1<<3)
#define MOD_RCTRL (1<<4)
#define MOD_RSHIFT (1<<5)
#define MOD_RALT (1<<6)
#define MOD_RGUI (1<<7)

// Minimal HID keycodes
#define HID_KC_A 0x04
#define HID_KC_B 0x05
#define HID_KC_C 0x06
#define HID_KC_D 0x07
#define HID_KC_E 0x08
#define HID_KC_F 0x09
#define HID_KC_G 0x0A
#define HID_KC_H 0x0B
#define HID_KC_I 0x0C
#define HID_KC_J 0x0D
#define HID_KC_K 0x0E
#define HID_KC_L 0x0F
#define HID_KC_M 0x10
#define HID_KC_N 0x11
#define HID_KC_O 0x12
#define HID_KC_P 0x13
#define HID_KC_Q 0x14
#define HID_KC_R 0x15
#define HID_KC_S 0x16
#define HID_KC_T 0x17
#define HID_KC_U 0x18
#define HID_KC_V 0x19
#define HID_KC_W 0x1A
#define HID_KC_X 0x1B
#define HID_KC_Y 0x1C
#define HID_KC_Z 0x1D
#define HID_KC_1 0x1E
#define HID_KC_2 0x1F
#define HID_KC_3 0x20
#define HID_KC_4 0x21
#define HID_KC_5 0x22
#define HID_KC_6 0x23
#define HID_KC_7 0x24
#define HID_KC_8 0x25
#define HID_KC_9 0x26
#define HID_KC_0 0x27
#define HID_KC_ENTER 0x28
#define HID_KC_SPACE 0x2C

// Punctuation and control keys (USB HID Usage Page 0x07)
#define HID_KC_MINUS 0x2D // - _
#define HID_KC_EQUAL 0x2E // = +
#define HID_KC_LBRACKET 0x2F // [ {
#define HID_KC_RBRACKET 0x30 // ] }
#define HID_KC_BACKSLASH 0x31 // \ |
#define HID_KC_SEMICOLON 0x33 // ; :
#define HID_KC_APOSTROPHE 0x34 // ' "
#define HID_KC_GRAVE 0x35 // ` ~
#define HID_KC_COMMA 0x36 // , <
#define HID_KC_DOT 0x37 // . >
#define HID_KC_SLASH 0x38 // / ?
#define HID_KC_TAB 0x2B
#define HID_KC_BSPACE 0x2A

/* Consumer Control (keep your 2-byte layout) */
// High byte bits for volume, low byte low-nibble for "button index"
#define CC_PAYLOAD_LEN 2
#define CC_VOL_UP_BIT 0x40 // 0x40 00
#define CC_VOL_DOWN_BIT 0x80 // 0x80 00
#define CC_BTN_PLAY 0x05 // 00 05
#define CC_BTN_PAUSE 0x06 // 00 06
#define CC_BTN_RECORD 0x07 // 00 07
#define CC_BTN_FF 0x08 // 00 08
#define CC_BTN_RW 0x09 // 00 09
#define CC_BTN_NEXT 0x0A // 00 0A
#define CC_BTN_PREV 0x0B // 00 0B
#define CC_BTN_STOP 0x0C // 00 0C
#define CC_BTN_PLAY_PAUSE 0x0D // 00 0D (toggle)

/* HID Consumer Usage IDs */
// Self defined
#define BLUETOOTH_CMD_INIT 0
#define BLUETOOTH_CMD_DEINIT 1
#define BLUETOOTH_CMD_SCRIPT_ONE 2

// Actual commands
#define BLUETOOTH_CMD_POWER 48
#define BLUETOOTH_CMD_RESET 49
#define BLUETOOTH_CMD_SLEEP 50

#define BLUETOOTH_CMD_MENU 64
#define BLUETOOTH_CMD_SELECTION 128
#define BLUETOOTH_CMD_ASSIGN_SEL 130
#define BLUETOOTH_CMD_RECALL_LAST 131
#define BLUETOOTH_CMD_QUIT 148
#define BLUETOOTH_CMD_HELP 149
#define BLUETOOTH_CMD_CHANNEL_UP 156
#define BLUETOOTH_CMD_CHANNEL_DOWN 157

#define BLUETOOTH_CMD_PLAY 176
#define BLUETOOTH_CMD_PAUSE 177
#define BLUETOOTH_CMD_RECORD 178
#define BLUETOOTH_CMD_FAST_FORWARD 179
#define BLUETOOTH_CMD_REWIND 180
#define BLUETOOTH_CMD_NEXT_TRK 181
#define BLUETOOTH_CMD_PREV_TRK 182
#define BLUETOOTH_CMD_STOP 183
#define BLUETOOTH_CMD_PLAY_PAUSE 205

#define BLUETOOTH_CMD_VOLUME 224
#define BLUETOOTH_CMD_BALANCE 225
#define BLUETOOTH_CMD_MUTE 226
#define BLUETOOTH_CMD_BASS 227
#define BLUETOOTH_CMD_VOLUME_UP 233
#define BLUETOOTH_CMD_VOLUME_DOWN 234

/** 
 * @brief Initialize bluetooth and start advertising to connect with last known
 */
void bluetooth_init(void);

/** 
 * @brief Deinitialize bluetooth
 */
void bluetooth_deinit(void);

/** 
 * @brief Send a media command over bluetooth
 *
 * @param [in] cmd Command to send
 * @param [in] key_pressed Bool to simulate press
 */
void bluetooth_send_media(uint8_t key_cmd, bool key_pressed);

/** 
 * @brief Send a keyboard string over bluetooth
 *
 * @param [in] s String to send
 * @param [in] tap_ms Delay between characters
 */
void bluetooth_send_string(const char *s, uint32_t tap_ms);

/** 
 * @brief Sends the battery level to the connected device
 *
 * @param [in] percent The battery level percentage to send
 */
void bluetooth_set_battery_level(uint8_t percent);

#endif // BLUETOOTH_FUNCS_H
