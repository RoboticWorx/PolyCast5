#include "polycast5_macros.h"
#include "polycast5_gpios.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "sx126x.h"

#include "lora_task.h"
#include "lora_pcp.h"
#include "lora_radio.h"
#include "lora_meshtastic.h"
#include "lora_meshtastic_portal.h"

#define MAX_RETRIES 2

static volatile bool need_to_retry = false;
static volatile uint8_t retry_count = 0;

static lora_pcp_cmd_t lora_cmd;

static const char *TAG = "LORA_TASK";

static SemaphoreHandle_t xLoraEventSemaphore;

SemaphoreHandle_t xLoraGenerateEncKeySemaphore;
SemaphoreHandle_t xLoraReceiptValidSemaphore;

QueueHandle_t xLoraSendEncQueue;

static void lora_event_handler_task(void *pvParameters);

// ISR handler for DIO1
static void IRAM_ATTR dio1_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Signal the event handler task
    xSemaphoreGiveFromISR(xLoraEventSemaphore, &xHigherPriorityTaskWoken);

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// LoRa Task
static void lora_task(void *pvParameters)
{
    // Create semaphores for LoRa events
    xLoraEventSemaphore = xSemaphoreCreateBinary();
    if (xLoraEventSemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create xLoraEventSemaphore semaphore");
    }
    configASSERT(xLoraEventSemaphore);
    
    xLoraGenerateEncKeySemaphore = xSemaphoreCreateBinary();
    if (xLoraGenerateEncKeySemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create xLoraGenerateEncKeySemaphore semaphore");
    }
    configASSERT(xLoraGenerateEncKeySemaphore);
    
    xLoraReceiptValidSemaphore = xSemaphoreCreateBinary();
    if (xLoraReceiptValidSemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create xLoraReceiptValidSemaphore semaphore");
    }
    configASSERT(xLoraReceiptValidSemaphore);
    
    xLoraSendEncQueue = xQueueCreate(1, sizeof(lora_pcp_cmd_t));
    if (xLoraSendEncQueue == NULL) {
        ESP_LOGE(TAG, "Failed to create xLoraSendEncQueue queue");
    }
    configASSERT(xLoraSendEncQueue);

    // Load (or generate) the Meshtastic web portal password
    lora_meshtastic_portal_pass_init();

    // Set global meshtastic enabled flag
    g_meshtastic_mode = lora_meshtastic_portal_enabled_load_nvs();

    // Create the LoRa event handler task
    if (xTaskCreate(lora_event_handler_task, "lora_event_handler", 1024 * 3, NULL, POLYCAST5_PRIORITY_INTERRUPT, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start lora_event_handler_task");
    }
    
    sx126x_mod_params_lora_t lora_mod_params = {
        .sf = SX126X_LORA_SF7, // Spreading factor (higher value sends further but takes more time)
        .bw = SX126X_LORA_BW_125, // Bandwidth
        .cr = SX126X_LORA_CR_4_5, // Error correction
        .ldro = 0, // 1 if SF > 10
    };

    sx126x_pkt_params_lora_t lora_pkt_params = {
        .preamble_len_in_symb = 12,
        .header_type = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes = LORA_PCP_PAYLOAD_LENGTH,
        .crc_is_on = true,
        .invert_iq_is_on = false,
    };

    // User-selected region picks the RF band to match the attached antenna (region table in lora_pcp.c)
    lora_region_t lora_region = lora_pcp_load_region_nvs();
    const lora_region_params_t *region_params = lora_region_get_params(lora_region);

    // Mode-dependent PHY: PCP defaults above, or Meshtastic LongFast below
    uint32_t rf_freq = region_params->pcp_freq_hz; // PCP frequency
    uint8_t lora_sync_word = 0x62; // PCP sync word
    if (g_meshtastic_mode) {
        // Region picks the LongFast slot to match the antenna
        lora_meshtastic_get_radio_params(&lora_mod_params, &lora_pkt_params, &rf_freq, &lora_sync_word, lora_region);
    } else {
        // Apply the user-selected spreading factor (PCP mode only; Meshtastic sets its own SF above)
        uint8_t user_sf = lora_pcp_load_sf_nvs();
        lora_mod_params.sf = (sx126x_lora_sf_t)user_sf;
        lora_mod_params.ldro = (user_sf > SX126X_LORA_SF10) ? 1 : 0; // LDRO required for SF11/SF12 at BW125
    }

    // Define the PA configuration parameters
    sx126x_pa_cfg_params_t pa_config = {
        .pa_duty_cycle = 0x04, // Duty cycle setting
        .hp_max = 0x07, // Maximum output power
        .device_sel = 0x00, // Select SX1262-specific PA configuration
        .pa_lut = 0x01 // Default LUT (Look-Up Table)
    };

    sx126x_hal_reset(NULL);

    vTaskDelay(pdMS_TO_TICKS(10));

    sx126x_status_t status = sx126x_init_retention_list(NULL);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to init retention list");
    }

    status = sx126x_set_reg_mode(NULL, SX126X_REG_MODE_LDO);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set reg mode");
    }

    status = sx126x_set_dio2_as_rf_sw_ctrl(NULL, true);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set dio2 as rf switch");
    }

    status = sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set standby");
    }

    status = sx126x_cal(NULL, SX126X_CAL_ALL);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to calibrate");
    }

    status = sx126x_set_pkt_type(NULL, SX126X_PKT_TYPE_LORA);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set packet type");
    }

    status = sx126x_set_rf_freq(NULL, rf_freq);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set frequency");
    }

    // Calibrate the image for the active band so image rejection matches the operating frequency
    status = sx126x_cal_img_in_mhz(NULL, region_params->cal_img_mhz_min, region_params->cal_img_mhz_max);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to calibrate image");
    }

    status = sx126x_set_pa_cfg(NULL, &pa_config);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set PA configuration");
    }

    sx126x_ramp_time_t ramp_time = SX126X_RAMP_200_US; // 200 us ramp time
    status = sx126x_set_tx_params(NULL, (int8_t)22, ramp_time); // 22dBm
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set TX params");
    }

    status = sx126x_cfg_tx_clamp(NULL); // SX1262 §15.2 PA-clamp init workaround
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to configure TX clamp");
    }

    // sx126x_set_rx_tx_fallback_mode // Default is RC standby

    /*status = sx126x_cfg_rx_boosted(
        NULL, true); // More sensitive RX at cost of more power
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to configure RX boost mode");
    }*/

    status = sx126x_set_lora_mod_params(NULL, &lora_mod_params);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set LoRa modulation parameters");
    }

    status = sx126x_set_lora_pkt_params(NULL, &lora_pkt_params);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set LoRa packet parameters");
    }

    status = sx126x_set_lora_sync_word(NULL, lora_sync_word);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set LoRa sync word");
    }

    status = sx126x_set_dio_irq_params(
        NULL,
        SX126X_IRQ_ALL, // Enable all IRQs
        SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT | SX126X_IRQ_HEADER_ERROR | SX126X_IRQ_CRC_ERROR, // DIO1 event on
        SX126X_IRQ_NONE, // No IRQs mapped to DIO2
        SX126X_IRQ_NONE // No IRQs mapped to DIO3
    );
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set DIO IRQ parameters");
    }
    sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL); // Clear IRQs at start

    // Set up DIO1 interrupt for RX
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SX126X_DIO1_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE, // Trigger on rising edge
    };
    gpio_config(&io_conf);
    gpio_isr_handler_add(SX126X_DIO1_PIN, dio1_isr_handler, NULL);

    // Meshtastic mode runs its own loop (continuous RX + text/NodeInfo TX) and never returns if true
    if (g_meshtastic_mode) {
        lora_meshtastic_run();
    }

    lora_pcp_load_msg_id_nvs(); // Load persisted msg_id counter
    lora_pcp_cmd_msg_t cmd_msg = {0}; // Hold binary command to send
    while (1) {
        // Generate encryption key requested
        if (xSemaphoreTake(xLoraGenerateEncKeySemaphore, 0) == pdTRUE) {
            lora_pcp_generate_random_key();
        }

        // If retrying from no receipt
        if (need_to_retry) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "RETRYING msg_id=%" PRIu32, cmd_msg.msg_id);
#endif
            // Encrypt and send the same command again
            if (lora_pcp_encrypt_and_transmit((uint8_t *)&cmd_msg, sizeof(cmd_msg))) {
                need_to_retry = false;
            } else if (retry_count < MAX_RETRIES) {
                retry_count++;
                ESP_LOGE(TAG, "Retry TX failed, will retry next loop");
            } else { // Radio TX keeps failing: give up on this command
                need_to_retry = false;
                waiting_for_ack = false;
                ESP_LOGE(TAG, "Giving up TX after max retries");
            }
        }
        // Else if new command: take ownership now so the queue slot frees -
        // a command sent while this one is in flight waits its turn instead of overwriting it in the queue (where it would be wiped on ACK/give-up)
        else if (!waiting_for_ack && xQueueReceive(xLoraSendEncQueue, &lora_cmd, 0) == pdTRUE) {
            xSemaphoreTake(xLoraReceiptValidSemaphore, 0); // Drain stale receipt: only this command's ACK may show 'delivered'
            lora_pcp_set_key(lora_cmd.key);

            // Create unique message ID
            uint32_t msg_id = lora_pcp_create_msg_id(); // 1, 2, ...
            expected_rx_id = msg_id;

            retry_count = 0; // Reset count

            // If no instructions, just put a "0"
            if (strlen(lora_cmd.instr) == 0) {
                strcpy(lora_cmd.instr, "0");
            }

            // Build binary command message
            memset(&cmd_msg, 0, sizeof(cmd_msg)); // Clear previous contents
            cmd_msg.type = LORA_PCP_COMMAND;
            cmd_msg.msg_id = msg_id;
            cmd_msg.index = (uint8_t)lora_cmd.index;
            memcpy(cmd_msg.instr, lora_cmd.instr, sizeof(cmd_msg.instr));

            // Ensure null termination
            cmd_msg.instr[sizeof(cmd_msg.instr) - 1] = '\0';
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "SENDING idx=%d instr=%s msg_id=%" PRIu32, cmd_msg.index, cmd_msg.instr, cmd_msg.msg_id);
            ESP_LOG_BUFFER_HEX("LORA_TASK: Using encryption_key", lora_cmd.key, LORA_PCP_ENC_KEY_LEN);
#endif
            // Encrypt and send
            if (lora_pcp_encrypt_and_transmit((uint8_t *)&cmd_msg, sizeof(cmd_msg))) {
                waiting_for_ack = true;
            } else {
                // Command is already off the queue: retry it via the retry path (bounded)
                waiting_for_ack = true;
                need_to_retry = true;
                ESP_LOGE(TAG, "TX failed, will retry next loop");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void lora_event_handler_task(void *pvParameters)
{
    while (1) {
        // Wait for an event from the ISR
        if (xSemaphoreTake(xLoraEventSemaphore, portMAX_DELAY) == pdTRUE) {
            // Check IRQ flags
            uint16_t irq_flags = 0;
            sx126x_get_irq_status(NULL, &irq_flags);

            if (g_meshtastic_mode) {
                lora_meshtastic_handle_irq(irq_flags);
                continue;
            }

            // Clear all latched flags up front; keeps a co-latched flag from being left set
            sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL);

            // If transmission complete
            if (irq_flags & SX126X_IRQ_TX_DONE) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Transmission completed");
#endif
                lora_radio_set_rx_mode(); // Listen for receipt from receiver
            } else if (irq_flags & SX126X_IRQ_RX_DONE) { // Else if receive complete
                // Read the received packet
                
                uint8_t rx_buffer[LORA_PCP_PAYLOAD_LENGTH];
                uint8_t rx_size = 0;
                
                sx126x_rx_buffer_status_t rx_status;
                
                // Check RX
                sx126x_get_rx_buffer_status(NULL, &rx_status);
                
                // Get size of packet
                rx_size = rx_status.pld_len_in_bytes;

                // Validate size
                if (rx_size == 0 || rx_size > sizeof(rx_buffer)) {
#ifdef POLYCAST5_DEBUG
                    ESP_LOGW(TAG, "Invalid RX size %d, discarding", rx_size);
#endif
                    sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);

                    if (retry_count < MAX_RETRIES) {
                        need_to_retry = true;
                        retry_count++;
                    } else {
                        waiting_for_ack = false; // Give up; a queued next command dispatches normally
                    }
                    continue;
                }

                // Read data into buffer
                sx126x_read_buffer(NULL, rx_status.buffer_start_pointer, rx_buffer, rx_size);
                
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Received packet of size %d", rx_size);
#endif
                // Process received
                lora_pcp_process_received_message(rx_buffer, rx_size);

                sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);

                // If ACK wasn't accepted, treat like a failed receive
                if (waiting_for_ack) {
                    if (retry_count < MAX_RETRIES) {
                        need_to_retry = true;
                        retry_count++;
                    } else {
                        waiting_for_ack = false; // Give up; a queued next command dispatches normally
                    }
                }
            } else if (irq_flags & SX126X_IRQ_TIMEOUT) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGW(TAG, "RX timeout occurred");
#endif
                sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);

                // Never got receipt, need to try again with same everything
                if (retry_count < MAX_RETRIES) { // Cap at MAX_RETRIES
                    need_to_retry = true;
                    retry_count++;
                } else {
                    waiting_for_ack = false; // Give up; a queued next command dispatches normally

#ifdef POLYCAST5_DEBUG
                    ESP_LOGW(TAG, "Hit max LoRa retries");
#endif
                }
            } else if (irq_flags & SX126X_IRQ_HEADER_ERROR) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGE(TAG, "Header error in received packet");
#endif
                sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);

                // Never got receipt, need to try again with same everything
                if (retry_count < MAX_RETRIES) { // Cap at MAX_RETRIES
                    need_to_retry = true;
                    retry_count++;
                } else {
                    waiting_for_ack = false; // Give up; a queued next command dispatches normally

#ifdef POLYCAST5_DEBUG
                    ESP_LOGW(TAG, "Hit max LoRa retries");
#endif
                }
            } else if (irq_flags & SX126X_IRQ_CRC_ERROR) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGE(TAG, "CRC error in received packet");
#endif
                sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);

                // Never got receipt, need to try again with same everything
                if (retry_count < MAX_RETRIES) { // Cap at MAX_RETRIES
                    need_to_retry = true;
                    retry_count++;
                } else {
                    waiting_for_ack = false; // Give up; a queued next command dispatches normally

#ifdef POLYCAST5_DEBUG
                    ESP_LOGW(TAG, "Hit max LoRa retries");
#endif
                }
            }
        }
    }
}

void lora_task_abort_pending(void)
{
    if (g_meshtastic_mode) {
        return;
    }

    // Idle the radio first so no further DIO1 IRQs can re-set the flags
    sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);
    sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL);

    need_to_retry = false;
    retry_count = 0;
    waiting_for_ack = false;

    if (xLoraSendEncQueue) {
        xQueueReset(xLoraSendEncQueue);
    }
}

void lora_task_resume_after_sleep(void)
{
    if (g_meshtastic_mode) {
        // The sleep path already ended the portal session via
        // lora_meshtastic_listen_stop(), so this only re-arms continuous RX in
        // the unexpected case a session is still marked active
        lora_meshtastic_resume_rx();
    }
    // PCP idles the radio in the standby left by lora_task_abort_pending() and
    // re-enters RX/TX on the next command, so that standby is the correct woken
    // resting state - nothing to re-arm here.
}

// Function to create the LoRa task
void lora_task_create(void)
{
    // Create the LoRa task
    if (xTaskCreate(lora_task, "lora_task", 1024 * 3, NULL, POLYCAST5_PRIORITY_MEDIUM, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start lora_task");
    }
}