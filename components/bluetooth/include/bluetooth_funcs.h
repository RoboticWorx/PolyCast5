#ifndef BLUETOOTH_FUNCS_H
#define BLUETOOTH_FUNCS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "host/ble_hs.h"

#include "host/ble_hs.h"

typedef struct {
    ble_addr_t addr; // Identity address
    char label[32]; // Name
} bluetooth_peer_info_t;

#define BT_MAX_PEERS 20

// Report IDs
#define HID_RPT_ID_KB_IN 1 // Keyboard input
#define HID_RPT_ID_CC_IN 3 // Consumer Control input

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

// Added keys for bt scripts
#define HID_KC_ESC 0x29
#define HID_KC_BACKSPACE HID_KC_BSPACE
#define HID_KC_DELETE 0x4C // Forward Delete
#define HID_KC_HOME 0x4A
#define HID_KC_END 0x4D
#define HID_KC_PGUP 0x4B
#define HID_KC_PGDN 0x4E
#define HID_KC_RIGHT 0x4F
#define HID_KC_LEFT 0x50
#define HID_KC_DOWN 0x51
#define HID_KC_UP 0x52

// Function keys (F1..F12)
#define HID_KC_F1 0x3A
#define HID_KC_F2 0x3B
#define HID_KC_F3 0x3C
#define HID_KC_F4 0x3D
#define HID_KC_F5 0x3E
#define HID_KC_F6 0x3F
#define HID_KC_F7 0x40
#define HID_KC_F8 0x41
#define HID_KC_F9 0x42
#define HID_KC_F10 0x43
#define HID_KC_F11 0x44
#define HID_KC_F12 0x45

/* Consumer Control (2-byte layout) */
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
#define BLUETOOTH_CMD_UNPAIR_ALL 2
#define BLUETOOTH_CMD_UNPAIR_ALL_NO_REINIT 3

#define BLUETOOTH_SCRIPT_OFFSET 1000

// Pre-defined media commands
// Presentation
#define BLUETOOTH_SCRIPT_PRESENTATION_START 2000
#define BLUETOOTH_SCRIPT_PRESENTATION_LEFT 2001
#define BLUETOOTH_SCRIPT_PRESENTATION_RIGHT 2002
#define BLUETOOTH_SCRIPT_PRESENTATION_ESC 2003
#define BLUETOOTH_SCRIPT_PRESENTATION_BLANK 2004
// Scroll
#define BLUETOOTH_SCRIPT_SCROLL_UP 2005
#define BLUETOOTH_SCRIPT_SCROLL_DOWN 2006
#define BLUETOOTH_SCRIPT_SCROLL_PG_UP 2007
#define BLUETOOTH_SCRIPT_SCROLL_PG_DOWN 2008
// Socials
#define BLUETOOTH_SCRIPT_SOCIALS_UP 2009
#define BLUETOOTH_SCRIPT_SOCIALS_DOWN 2010
#define BLUETOOTH_SCRIPT_SOCIALS_LIKE 2011

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

typedef enum {
    BT_STATE_OFF = 0, BT_STATE_INITING, BT_STATE_RUNNING, BT_STATE_DEINITING
} bluetooth_state_t;

/** 
 * @brief Initialize bluetooth and start advertising to connect with last known
 */
void bluetooth_init(void);

/** 
 * @brief Deinitialize bluetooth
 */
void bluetooth_deinit(void);

/** 
 * @brief Forgets all bluetooth peers so nothing auto-connects
 */
void bluetooth_forget_all_peers(void);

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
void bluetooth_send_script(const char *s, uint32_t tap_ms);

/** 
 * @brief Sends the battery level to the connected device
 *
 * @param [in] percent The battery level percentage to send
 */
void bluetooth_set_battery_level(uint8_t percent);

/** 
 * @brief Sets a given bluetooth peer as the preferred in NVS
 *
 * @param [in] peer Peer to set
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_set_preferred_peer_nvs(const ble_addr_t *peer);

/** 
 * @brief Gets the preferred bluetooth peer from NVS
 *
 * @param [out] out Peer retrieved
 * @param [out] found True if peer was found
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_get_preferred_peer_nvs(ble_addr_t *out, bool *found);

/** 
 * @brief Load all bluetooth peers from NVS
 *
 * @param [out] out Peers retrieved
 * @param [in] max Max peers to consider
 *
 * @returns Number of peers found
 */
int bluetooth_get_peers_list_nvs(bluetooth_peer_info_t *out, int max);

/** 
 * @brief Add a peer to the main peer list in NVS
 *
 * @param [in] peer Peer to add
 */
void bluetooth_add_to_peers_list_nvs(const ble_addr_t *peer);

/** 
 * @brief Clears all bluetooth peers in NVS
 *
 * @param [in] preferred_only If true, only erase the preferred peer
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_clear_peers_list_nvs(bool preferred_only);

/** 
 * @brief Save the Bluetooth pairing key to NVS
 *
 * @param [in] key Pairing key to save
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_pairing_key_save_nvs(uint32_t key);

/** 
 * @brief Load the Bluetooth pairing key from NVS
 *
 * @param [out] key Pairing key to load
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_pairing_key_load_nvs(uint32_t *key);

/* =============== For naming labels =============== */

/** 
 * @brief Gets peer name label from NVS
 *
 * @param [in] addr Address the label is saved under
 * @param [out] out Output to write the label into
 * @param [in] out_sz Desired size of the output
 *
 * @returns True on success
 */
bool bluetooth_get_peer_label_nvs(const ble_addr_t *addr, char *out, size_t out_sz);

/** 
 * @brief Sets peer name label to NVS
 *
 * @param [in] addr Address the label is to be saved under
 * @param [out] label Label to save
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_set_peer_label_nvs(const ble_addr_t *addr, const char *label);

/** 
 * @brief Remove a peer from NVS
 *
 * @param [in] addr Address of peer to delete
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_remove_peer_nvs(const ble_addr_t *addr);


#endif // BLUETOOTH_FUNCS_H