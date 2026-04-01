#include "polycast5_gpios.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sx126x.h"

#include "lora_radio.h"

static const char *TAG = "LORA_RADIO";

void lora_radio_set_rx_mode(void)
{
    #define RTC_FREQ_HZ 32768U
    #define MS_TO_RTC_STEP(ms) ((uint32_t)(((uint64_t)(ms) * RTC_FREQ_HZ) / 1000U))

    // Wait for SX126x to be ready (BUSY pin goes low)
    for (int i = 0; i < 1000 && gpio_get_level(SX126X_BUSY_PIN); ++i) {
        vTaskDelay(1);
    }
    if (gpio_get_level(SX126X_BUSY_PIN) == 1) {
        ESP_LOGE(TAG, "BUSY timeout in lora_radio_set_rx_mode");
        return;
    }

    // Enter RX mode
    // Use timeout in case receipt is never received
    uint32_t timeout_steps = MS_TO_RTC_STEP(2000);
    sx126x_status_t status = sx126x_set_rx_with_timeout_in_rtc_step(NULL, timeout_steps);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to enter continuous RX mode\n");
        return;
    }
}

bool lora_radio_tx(uint8_t tx_data[], uint8_t data_len)
{
    // Wait for SX126x to be ready (BUSY pin goes low)
    for (int i = 0; i < 1000 && gpio_get_level(SX126X_BUSY_PIN); ++i) {
        vTaskDelay(1);
    }
    if (gpio_get_level(SX126X_BUSY_PIN) == 1) {
        ESP_LOGE(TAG, "BUSY timeout in lora_radio_tx");
        return false;
    }

    // Update payload length for this transmission
    sx126x_pkt_params_lora_t pkt_params = {
        .preamble_len_in_symb = 12,
        .header_type = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes = data_len,
        .crc_is_on = true,
        .invert_iq_is_on = false,
    };
    sx126x_status_t status = sx126x_set_lora_pkt_params(NULL, &pkt_params);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set packet params");
        return false;
    }

    status = sx126x_write_buffer(NULL, 0, tx_data, data_len);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to write to buffer");
        return false;
    }

    // Start transmission
    status = sx126x_set_tx(NULL, SX126X_MAX_TIMEOUT_IN_MS);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to start transmission");
        return false;
    }

    return true;
}
