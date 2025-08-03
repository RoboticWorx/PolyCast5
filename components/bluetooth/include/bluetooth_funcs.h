#ifndef BLUETOOTH_FUNCS_H
#define BLUETOOTH_FUNCS_H

#include <stdbool.h>
#include <stdint.h>

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

/**
 * @brief Initialize bluetooth
 */
void bluetooth_init(void);

/**
 * @brief Deinitialize bluetooth
 */
void bluetooth_deinit(void);

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