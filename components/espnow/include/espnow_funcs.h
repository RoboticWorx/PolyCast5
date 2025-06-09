#ifndef ESPNOW_FUNCS_H
#define ESPNOW_FUNCS_H

#include "esp_err.h"
#include "esp_now.h"

/**
 * @brief Initialize the Wi-Fi driver and allocate Wi-Fi buffers
 *
 * @return ESP_OK on success
 */
esp_err_t esp_funcs_wifi_driver_init(void);

/**
 * @brief Start the Wi-Fi radio
 *
 * @return ESP_OK on success
 */
esp_err_t esp_funcs_wifi_radio_start(uint8_t channel);

/**
 * @brief Stop the Wi-Fi radio
 *
 * @return ESP_OK on success
 */
esp_err_t esp_funcs_wifi_radio_stop(void);

/**
 * @brief Initialize ESP-NOW
 *
 * @param [in] mac MAC address to add as a peer
 * @param [in] channel Wi-Fi channel to configure to the peer
 * @param [in] encrypt Yes or no to signal if peer should add encryption
 * @param [in] lmk Local master key for said encryption
 *
 * @return ESP_OK on success
 */
esp_err_t esp_funcs_espnow_init(const uint8_t *mac, uint8_t channel, bool encrypt, const uint8_t *lmk);

/**
 * @brief De-initialize ESP-NOW
 *
 * @return ESP_OK on success
 */
esp_err_t esp_funcs_espnow_deinit(void);

/**
 * @brief Send data via ESP-NOW
 *
 * @param [in] mac MAC address to send the data to
 * @param [in] data Data to send
 * @param [in] len Length of the data to send
 *
 * @return ESP_OK on success
 */
esp_err_t esp_funcs_espnow_send_data(const uint8_t *mac, const uint8_t *data, size_t len);

/**
 * @brief Register an ESP-NOW receive callback
 *
 * @param cb Function called on each incoming packet
 *
 * @return ESP_OK on success
 */
esp_err_t esp_funcs_espnow_register_recv_cb(esp_now_recv_cb_t cb);


#endif // ESPNOW_FUNCS_H