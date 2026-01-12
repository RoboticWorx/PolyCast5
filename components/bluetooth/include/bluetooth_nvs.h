#ifndef BLUETOOTH_NVS_H
#define BLUETOOTH_NVS_H

#include "host/ble_hs.h"
#include "esp_err.h"

#include "bluetooth_utils.h"

/** 
 * @brief Sets a given bluetooth peer as the preferred in NVS
 *
 * @param [in] peer Peer to set
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_nvs_set_preferred_peer(const ble_addr_t *peer);

/** 
 * @brief Gets the preferred bluetooth peer from NVS
 *
 * @param [out] out Peer retrieved
 * @param [out] found True if peer was found
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_nvs_get_preferred_peer(ble_addr_t *out, bool *found);

/** 
 * @brief Load all bluetooth peers from NVS
 *
 * @param [out] out Peers retrieved
 * @param [in] max Max peers to consider
 *
 * @returns Number of peers found
 */
int bluetooth_nvs_get_peers_list(bluetooth_peer_info_t *out, int max);

/** 
 * @brief Add a peer to the main peer list in NVS
 *
 * @param [in] peer Peer to add
 */
void bluetooth_nvs_add_to_peers_list(const ble_addr_t *peer);

/** 
 * @brief Clears all bluetooth peers in NVS
 *
 * @param [in] preferred_only If true, only erase the preferred peer
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_nvs_clear_peers_list(bool preferred_only);

/** 
 * @brief Save the Bluetooth pairing key to NVS
 *
 * @param [in] key Pairing key to save
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_nvs_pairing_key_save(uint32_t key);

/** 
 * @brief Load the Bluetooth pairing key from NVS
 *
 * @param [out] key Pairing key to load
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_nvs_pairing_key_load(uint32_t *key);

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
bool bluetooth_nvs_get_peer_label(const ble_addr_t *addr, char *out, size_t out_sz);

/** 
 * @brief Sets peer name label to NVS
 *
 * @param [in] addr Address the label is to be saved under
 * @param [out] label Label to save
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_nvs_set_peer_label(const ble_addr_t *addr, const char *label);

/** 
 * @brief Remove a peer from NVS
 *
 * @param [in] addr Address of peer to delete
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_nvs_remove_peer(const ble_addr_t *addr);


#endif // BLUETOOTH_NVS_H