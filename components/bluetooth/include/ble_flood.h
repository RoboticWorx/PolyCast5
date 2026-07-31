#ifndef BLE_FLOOD_H
#define BLE_FLOOD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * BLE advertising-flood ("BLE Flood") for wireless testing.
 *
 * Brings up an advertise-only NimBLE broadcaster and rapidly transmits fake
 * proximity-pairing adverts (Apple Continuity, Google Fast Pair, Microsoft
 * Swift Pair, Samsung), each with a fresh random advertiser MAC, to observe
 * how nearby devices react. Intended for authorized RF testing on hardware you
 * own. Mutually exclusive with the HID Bluetooth stack (both share the single
 * NimBLE host); starting flood is refused while HID Bluetooth is up.
 */

/** @brief Target ecosystem for the advertising flood. */
typedef enum {
    BLE_FLOOD_MODE_APPLE = 0,   /**< Apple Continuity proximity-pair + nearby-action (non-connectable) */
    BLE_FLOOD_MODE_FASTPAIR,    /**< Google Fast Pair service data 0xFE2C (connectable)                */
    BLE_FLOOD_MODE_SWIFTPAIR,   /**< Microsoft Swift Pair manufacturer 0x0006 (connectable)            */
    BLE_FLOOD_MODE_SAMSUNG,     /**< Samsung Buds/Watch manufacturer 0x0075 (non-connectable)          */
    BLE_FLOOD_MODE_ALL,         /**< Round-robin across all of the above                               */
    BLE_FLOOD_MODE_MAX
} ble_flood_mode_t;

/**
 * @brief Bring up an advertise-only NimBLE stack and start the flood.
 *
 * Spawns a dedicated worker task that owns the broadcaster lifecycle. Refuses
 * (without side effects) if HID Bluetooth is up, the controller is not idle, or
 * flood is already running.
 *
 * @param [in] mode Target ecosystem to flood.
 *
 * @returns ESP_OK on start, ESP_ERR_INVALID_ARG for a bad mode,
 *          ESP_ERR_INVALID_STATE if the radio is busy, ESP_ERR_NO_MEM if the
 *          worker task could not be created.
 */
esp_err_t ble_flood_start(ble_flood_mode_t mode);

/**
 * @brief Signal the flood to stop and tear the broadcaster stack down.
 *
 * Non-blocking: sets a stop flag and returns. The worker halts advertising,
 * tears down NimBLE, and self-deletes. Use ble_flood_is_active() to observe when
 * teardown has completed.
 *
 * @returns ESP_OK.
 */
esp_err_t ble_flood_stop(void);

/**
 * @brief Whether the flood broadcaster currently owns the BLE controller.
 *
 * @returns true from ble_flood_start() until the worker has fully torn down.
 */
bool ble_flood_is_active(void);

/**
 * @brief Number of adverts transmitted since the current flood started.
 *
 * @returns Running packet count (for the live UI counter).
 */
uint32_t ble_flood_get_count(void);

#endif // BLE_FLOOD_H
