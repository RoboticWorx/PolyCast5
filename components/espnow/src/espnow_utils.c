#include "polycast5_macros.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_err.h"

#include "espnow_utils.h"
#include "espnow_task.h"

#define TAG "ESPNOW_UTILS"

esp_err_t espnow_utils_wifi_driver_init(void)
{
    // Bring up TCP/IP stack and default event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create netif object
    esp_netif_create_default_wifi_sta();
    
    // Initialize Wi-Fi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if ((err != ESP_OK) && (err != ESP_ERR_WIFI_INIT_STATE) && (err != ESP_ERR_INVALID_STATE)) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
#endif
        return err;
    }
    
    // Set as a station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    return ESP_OK;
}

esp_err_t espnow_utils_wifi_radio_start(uint8_t channel)
{
    // Start Wi-Fi - don't abort on error
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // Set the channel
    err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_channel failed: %s", esp_err_to_name(err));
        espnow_utils_wifi_radio_stop();
        return err;
    }

    return ESP_OK;
}

esp_err_t espnow_utils_wifi_radio_stop(void)
{
    // Stop Wi-Fi - don't abort on error
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }

    return err;
}

static void send_cb(const wifi_tx_info_t *info, esp_now_send_status_t status)
{
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG,
            "Send to %02X:%02X:%02X:%02X:%02X:%02X | %s",
            info->des_addr[0], info->des_addr[1], info->des_addr[2],
            info->des_addr[3], info->des_addr[4], info->des_addr[5],
            status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
#endif
    
    if (status == ESP_NOW_SEND_SUCCESS) {
        xSemaphoreGive(xEspCmdRxStatusSemaphore);
    }
}

esp_err_t espnow_utils_espnow_init(const uint8_t *mac, uint8_t channel, bool encrypt, const uint8_t *lmk)
{
    esp_err_t err;

    // Initialize ESP-NOW
    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // Register send callback
    err = esp_now_register_send_cb(send_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_send_cb failed: %s", esp_err_to_name(err));
        return err;
    }

    // Configure peer
    esp_now_peer_info_t peer = {
        .ifidx   = WIFI_IF_STA,
        .channel = channel,
        .encrypt = encrypt,
    };
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    
    // If encrypting and LMK exists, add to LMK peer
    if (encrypt && lmk) {
        memcpy(peer.lmk, lmk, LMK_LEN);
    }
    
    // Register peer
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "add_peer failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t espnow_utils_espnow_deinit(void)
{
    // De-initialize ESP-NOW
    esp_err_t err = esp_now_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_deinit failed: %s", esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t espnow_utils_send_data(const uint8_t *mac, const uint8_t *data, size_t len)
{
    // Cap at max length
    if (len > ESP_NOW_MAX_DATA_LEN) {
        len = ESP_NOW_MAX_DATA_LEN;
    }
    
    // Send to given MAC
    return esp_now_send(mac, data, len);
}

esp_err_t espnow_utils_register_recv_cb(esp_now_recv_cb_t cb)
{
    // Register receiver callback
    esp_err_t err = esp_now_register_recv_cb(cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "recv_cb register failed: %s", esp_err_to_name(err));
    }
    
    return err;
}
