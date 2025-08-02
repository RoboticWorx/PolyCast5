#ifndef BLUETOOTH_FUNCS_H
#define BLUETOOTH_FUNCS_H

#include <stdbool.h>
#include <stdint.h>

#define HID_CC_RPT_MUTE 1
#define HID_CC_RPT_POWER 2
#define HID_CC_RPT_LAST 3
#define HID_CC_RPT_ASSIGN_SEL 4
#define HID_CC_RPT_PLAY	5
#define HID_CC_RPT_PAUSE 6
#define HID_CC_RPT_RECORD 7
#define HID_CC_RPT_FAST_FWD 8
#define HID_CC_RPT_REWIND 9
#define HID_CC_RPT_SCAN_NEXT_TRK 10
#define HID_CC_RPT_SCAN_PREV_TRK 11
#define HID_CC_RPT_STOP 12

#define HID_CC_RPT_CHANNEL_UP 0x10
#define HID_CC_RPT_CHANNEL_DOWN 0x30
#define HID_CC_RPT_VOLUME_UP 0x40
#define HID_CC_RPT_VOLUME_DOWN 0x80

// HID Consumer Control report bitmasks
#define HID_CC_RPT_NUMERIC_BITS 0xF0
#define HID_CC_RPT_CHANNEL_BITS 0xCF
#define HID_CC_RPT_VOLUME_BITS 0x3F
#define HID_CC_RPT_BUTTON_BITS 0xF0
#define HID_CC_RPT_SELECTION_BITS 0xCF

// Macros for the HID Consumer Control 2-byte report
#define HID_CC_RPT_SET_NUMERIC(s, x)	(s)[0] &= HID_CC_RPT_NUMERIC_BITS;		(s)[0] = (x)
#define HID_CC_RPT_SET_CHANNEL(s, x)	(s)[0] &= HID_CC_RPT_CHANNEL_BITS;		(s)[0] |= ((x) & 0x03) << 4
#define HID_CC_RPT_SET_VOLUME_UP(s)		(s)[0] &= HID_CC_RPT_VOLUME_BITS;		(s)[0] |= 0x40
#define HID_CC_RPT_SET_VOLUME_DOWN(s)	(s)[0] &= HID_CC_RPT_VOLUME_BITS;		(s)[0] |= 0x80
#define HID_CC_RPT_SET_BUTTON(s, x)		(s)[1] &= HID_CC_RPT_BUTTON_BITS;		(s)[1] |= (x)
#define HID_CC_RPT_SET_SELECTION(s, x)	(s)[1] &= HID_CC_RPT_SELECTION_BITS;	(s)[1] |= ((x) & 0x03) << 4

// HID Consumer Usage IDs (subset of the codes available in the USB HID Usage Tables spec)
#define BLUETOOTH_CMD_POWER 48 // Power
#define BLUETOOTH_CMD_RESET 49 // Reset
#define BLUETOOTH_CMD_SLEEP 50 // Sleep

#define BLUETOOTH_CMD_MENU 64 // Menu
#define BLUETOOTH_CMD_SELECTION	128 // Selection
#define BLUETOOTH_CMD_ASSIGN_SEL 129 // Assign Selection
#define BLUETOOTH_CMD_MODE_STEP 130 // Mode Step
#define BLUETOOTH_CMD_RECALL_LAST 131 // Recall Last
#define BLUETOOTH_CMD_QUIT 148 // Quit
#define BLUETOOTH_CMD_HELP 149 // Help
#define BLUETOOTH_CMD_CHANNEL_UP 156 // Channel Increment
#define BLUETOOTH_CMD_CHANNEL_DOWN 157 // Channel Decrement

#define BLUETOOTH_CMD_PLAY 176 // Play
#define BLUETOOTH_CMD_PAUSE 177 // Pause
#define BLUETOOTH_CMD_RECORD 178 // Record
#define BLUETOOTH_CMD_FAST_FORWARD 179 // Fast Forward
#define BLUETOOTH_CMD_REWIND 180 // Rewind
#define BLUETOOTH_CMD_SCAN_NEXT_TRK 181 // Scan Next Track
#define BLUETOOTH_CMD_SCAN_PREV_TRK 182 // Scan Previous Track
#define BLUETOOTH_CMD_STOP 183 // Stop
#define BLUETOOTH_CMD_EJECT 184 // Eject
#define BLUETOOTH_CMD_RANDOM_PLAY 185 // Random Play
#define BLUETOOTH_CMD_SELECT_DISC 186 // Select Disk
#define BLUETOOTH_CMD_ENTER_DISC 187 // Enter Disc
#define BLUETOOTH_CMD_REPEAT 188 // Repeat
#define BLUETOOTH_CMD_STOP_EJECT 204 // Stop/Eject
#define BLUETOOTH_CMD_PLAY_PAUSE 205 // Play/Pause
#define BLUETOOTH_CMD_PLAY_SKIP 206 // Play/Skip

#define BLUETOOTH_CMD_VOLUME 224 // Volume
#define BLUETOOTH_CMD_BALANCE 225 // Balance
#define BLUETOOTH_CMD_MUTE 226 // Mute
#define BLUETOOTH_CMD_BASS 227 // Bass
#define BLUETOOTH_CMD_VOLUME_UP 233 // Volume Increment
#define BLUETOOTH_CMD_VOLUME_DOWN 234 // Volume Decrement

#define HID_RPT_ID_CC_IN 3 // Consumer Control input report ID
#define HID_CC_IN_RPT_LEN 2 // Consumer Control input report Len

/** 
 * @brief Initialize Bluetooth
 */
void bluetooth_init(void);

/** 
 * @brief Send a media command over Bluetooth
 *
 * @param [in] key_cmd Command to send
 * @param [in] key_pressed If a key was pressed to send it
 */
void bluetooth_send_cmd(uint8_t key_cmd, bool key_pressed);

/** 
 * @brief Send a keyboard command over Bluetooth
 *
 * @param [in] character Character to send
 */
//void bluetooth_send_key(char character);


#endif /* BLUETOOTH_FUNCS_H */