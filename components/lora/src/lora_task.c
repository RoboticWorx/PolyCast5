#include "polycast5_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "sx126x.h"

#include "lora_task.h"
#include "lora_funcs.h"

#define MAX_RETRIES 2

extern uint32_t expected_rx_id;
extern bool waiting_for_ack;

static volatile bool need_to_retry = false;
static uint8_t retry_count = 0;

static lora_cmd_t lora_cmd;

static const char *TAG = "LORA_TASK";

static SemaphoreHandle_t xLoraEventSemaphore;

SemaphoreHandle_t xLoraGenerateEncKeySemaphore;
SemaphoreHandle_t xLoraReceiptValidSemaphore;

QueueHandle_t xLoraSendEncQueue;

static void lora_event_handler_task(void *pvParameters);

// ISR handler for DIO1
static void IRAM_ATTR dio1_isr_handler(void *arg) {
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	// Signal the event handler task
	xSemaphoreGiveFromISR(xLoraEventSemaphore, &xHigherPriorityTaskWoken);

	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// LoRa Task
static void lora_task(void *pvParameters) {
	
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
		ESP_LOGE(TAG, "Failed to create xReceiveEncKeyQueue semaphore");
	}
	configASSERT(xLoraReceiptValidSemaphore);
	
	xLoraSendEncQueue = xQueueCreate(1, sizeof(lora_cmd_t));
	if (xLoraSendEncQueue == NULL) {
		ESP_LOGE(TAG, "Failed to create xReceiveEncKeyQueue queue");
	}
	configASSERT(xLoraSendEncQueue);
	
	// Create the LoRa event handler task
	if (xTaskCreate(lora_event_handler_task, "lora_event_handler", 1024 * 3, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
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
		.pld_len_in_bytes = PAYLOAD_LENGTH,
		.crc_is_on = true,
		.invert_iq_is_on = false,
	};

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

	status = sx126x_cal(NULL, SX126X_CAL_ALL);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to calibrate");
	}

	status = sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set standby");
	}

	status = sx126x_set_pkt_type(NULL, SX126X_PKT_TYPE_LORA);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set packet type");
	}

	status = sx126x_set_rf_freq(NULL, 915000000);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set frequency");
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

	status = sx126x_set_lora_sync_word(NULL, 0x62);
	if (status != SX126X_STATUS_OK) {
		ESP_LOGE(TAG, "Failed to set LoRa sync word");
	}

	status = sx126x_set_dio_irq_params(
		NULL,
		SX126X_IRQ_ALL, // Enable all IRQs
		SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT |
			SX126X_IRQ_HEADER_ERROR |
			SX126X_IRQ_CRC_ERROR, // Enable IRQ finished
		SX126X_IRQ_NONE,		  // No IRQs mapped to DIO2
		SX126X_IRQ_NONE			  // No IRQs mapped to DIO3
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

	//gpio_install_isr_service(0);
	gpio_isr_handler_add(SX126X_DIO1_PIN, dio1_isr_handler, NULL);

	char payload[CYPHERTEXT_LENGTH] = {0}; // Hold data to send
	for (;;) {
		
		if (xSemaphoreTake(xLoraGenerateEncKeySemaphore, 0) == pdTRUE) {
			lora_generate_random_key();
		}
		
		// If retrying from no receipt
		if (need_to_retry) {
			#ifdef POLYCAST5_DEBUG
	        	ESP_LOGI(TAG, "RETRYING: %s", payload);
	        #endif
	        
			// Encrypt and send the same payload again
			lora_encrypt_and_transmit((uint8_t *)payload);
			
			need_to_retry = false;
		}
		// Else if new command
		else if (!waiting_for_ack && uxQueueMessagesWaiting(xLoraSendEncQueue) > 0) {
			xQueuePeek(xLoraSendEncQueue, &lora_cmd, 0);
			
			memcpy(encryption_key, lora_cmd.key, ENC_KEY_LEN);
			
			// Create unique message ID
			uint32_t msg_id = lora_create_msg_id();
			expected_rx_id = msg_id;
				
			retry_count = 0; // Reset count
			waiting_for_ack = true; // Need to wait for acknowledgement before sending again
			
			// If no instructions, just put a "0"
			if (strlen(lora_cmd.instr) == 0) {
				strcpy(lora_cmd.instr, "0");
			}
			
			// Format command into string
			snprintf(payload, sizeof(payload), "PolyCast_Command_Value:%" PRIu32 ":%d:%s", expected_rx_id, lora_cmd.index, lora_cmd.instr);
			
			#ifdef POLYCAST5_DEBUG
	        	ESP_LOGI(TAG, "SENDING: %s", payload);
	        #endif
			
			// Encrypt and send over
			lora_encrypt_and_transmit((uint8_t *)payload);
		}

		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

static void lora_event_handler_task(void *pvParameters) {
	for (;;) {
		// Wait for an event from the ISR
		if (xSemaphoreTake(xLoraEventSemaphore, portMAX_DELAY) == pdTRUE) {
			// Check IRQ flags
			uint16_t irq_flags = 0;
			sx126x_get_irq_status(NULL, &irq_flags);

			// If transmission complete
			if (irq_flags & SX126X_IRQ_TX_DONE) {
				#ifdef POLYCAST5_DEBUG
		        	ESP_LOGI(TAG, "Transmission completed");
		        #endif
				
				sx126x_clear_irq_status(NULL, SX126X_IRQ_TX_DONE);
				lora_set_rx_mode(); // Listen for receipt from receiver
			}
			// Else if receive complete
			else if (irq_flags & SX126X_IRQ_RX_DONE) {
				// Read the received packet
				
				uint8_t rx_buffer[PAYLOAD_LENGTH];
				uint8_t rx_size = 0;
				
				sx126x_rx_buffer_status_t rx_status;
				
				// Check RX
				sx126x_get_rx_buffer_status(NULL, &rx_status);
				
				// Get size of packet
				rx_size = rx_status.pld_len_in_bytes;

				// Read data into buffer
				sx126x_read_buffer(NULL, rx_status.buffer_start_pointer,
								   rx_buffer, rx_size);
				
				#ifdef POLYCAST5_DEBUG
		        	ESP_LOGI(TAG, "Received packet of size %d", rx_size);
		        #endif

				// Process received
				lora_process_received_message(rx_buffer, rx_size);

				// Clear IRQ
				sx126x_clear_irq_status(NULL, SX126X_IRQ_RX_DONE);
			}

			if (irq_flags & SX126X_IRQ_TIMEOUT) {
				#ifdef POLYCAST5_DEBUG
					ESP_LOGW(TAG, "RX timeout occurred");
				#endif
				
				sx126x_clear_irq_status(NULL, SX126X_IRQ_TIMEOUT);
				sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);
				
				// Never got receipt, need to try again with same everything
				if (retry_count < MAX_RETRIES) { // Cap at MAX_RETRIES
					need_to_retry = true;
					retry_count++;
				}
				else {
					xQueueReset(xLoraSendEncQueue); // Clear pending commands
					waiting_for_ack = false;
					
					#ifdef POLYCAST5_DEBUG
						ESP_LOGW(TAG, "Hit max LoRa retires");
					#endif
				}
			}

			if (irq_flags & SX126X_IRQ_HEADER_ERROR) {
				ESP_LOGE(TAG, "Header error in received packet");
				sx126x_clear_irq_status(NULL, SX126X_IRQ_HEADER_ERROR);
			}

			if (irq_flags & SX126X_IRQ_CRC_ERROR) {
				ESP_LOGE(TAG, "CRC error in received packet");
				sx126x_clear_irq_status(NULL, SX126X_IRQ_CRC_ERROR);
			}
		}
	}
}

// Function to create the LoRa task
void lora_task_create(void) {
	// Create the LoRa task
	if (xTaskCreate(lora_task, "lora_task", 1024 * 2, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start lora_task");
	}
}