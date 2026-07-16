#include "polycast5_gpios.h"

#include "driver/gpio.h" // gpio_get_level (DIO1 line state in lora_radio_log_health)

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sx126x.h"

#include "lora_radio.h"
#include "lora_task.h" // g_lora_dio1_isr_count (DIO1 edge counter for diagnostics)

static const char *TAG = "LORA_RADIO";

void lora_radio_log_health(const char *ctx)
{
    // Index matches sx126x_chip_modes_t (0..6)
    static const char *mode_str[] = { "UNUSED", "RFU", "STBY_RC", "STBY_XOSC", "FS", "RX", "TX" };

    uint16_t irq = 0;
    sx126x_chip_status_t chip = {0};
    sx126x_errors_mask_t errs = 0;
    sx126x_get_irq_status(NULL, &irq);
    sx126x_get_status(NULL, &chip);
    sx126x_get_device_errors(NULL, &errs);

    int mode = (int)chip.chip_mode;
    ESP_LOGE(TAG, "%s: irq=0x%04x chip_mode=%s cmd_status=%d dev_errors=0x%04x%s%s%s dio1_level=%d dio1_isr_count=%u",
            ctx, (unsigned)irq,
            (mode >= 0 && mode <= 6) ? mode_str[mode] : "?",
            (int)chip.cmd_status, (unsigned)errs,
            (errs & SX126X_ERRORS_XOSC_START) ? " [XOSC_START: oscillator/TCXO never started]" : "",
            (errs & SX126X_ERRORS_PLL_LOCK) ? " [PLL_LOCK]" : "",
            (errs & (SX126X_ERRORS_RC64K_CALIBRATION | SX126X_ERRORS_RC13M_CALIBRATION |
                    SX126X_ERRORS_PLL_CALIBRATION | SX126X_ERRORS_ADC_CALIBRATION |
                    SX126X_ERRORS_IMG_CALIBRATION)) ? " [CALIBRATION]" : "",
            gpio_get_level(SX126X_DIO1_PIN),
            (unsigned)g_lora_dio1_isr_count);
}

// Receipt-wait RX window: fall back to standby if no ACK arrives within this long
#define LORA_PCP_RX_TIMEOUT_MS 2000

void lora_radio_set_rx_mode(void)
{
    // Enter RX with a timeout so a never-arriving receipt can't wedge RX
    // Pass milliseconds and let the driver do the ms -> RTC-step conversion with its own SX126X_RTC_FREQ_IN_HZ
    sx126x_status_t status = sx126x_set_rx(NULL, LORA_PCP_RX_TIMEOUT_MS);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to enter receipt RX mode\n");
        return;
    }
}

bool lora_radio_tx(uint8_t tx_data[], uint8_t data_len)
{
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
