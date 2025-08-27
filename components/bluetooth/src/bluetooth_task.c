#include "bluetooth_web_portal.h"
#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bluetooth_funcs.h"
#include "portmacro.h"
#include "bluetooth_task.h"
#include "gpio_task.h"
#include "gpio_funcs.h"

#define TAG "BLUETOOTH_TASK"

QueueHandle_t xBluetoothMediaCmdQueue;

SemaphoreHandle_t xBluetoothScriptMutex;

extern volatile bluetooth_state_t bluetooth_state;

static uint16_t bluetooth_cmd = 0;
static uint8_t battery_percentage = 100;
static const TickType_t battery_timer_interval = pdMS_TO_TICKS(1000);

static void bluetooth_task(void *arg)
{
	xBluetoothMediaCmdQueue = xQueueCreate(1, sizeof(uint16_t));
	configASSERT(xBluetoothMediaCmdQueue);
	
	TickType_t battery_timer_last = xTaskGetTickCount();
	
	while (1) {
		// If a bluetooth command is received
		if (xQueueReceive(xBluetoothMediaCmdQueue, &bluetooth_cmd, 0) == pdTRUE) {
			/* Initialization stuff */
			// Initialize command received
			if (bluetooth_cmd == BLUETOOTH_CMD_INIT) {
				bluetooth_init();
			}
			// De-initialize command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_DEINIT) {
				bluetooth_deinit();
			}
			/* Media commands */
			// Vol-up command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_VOLUME_UP && bluetooth_state == BT_STATE_RUNNING) {
				bluetooth_send_media(BLUETOOTH_CMD_VOLUME_UP, true);
	            vTaskDelay(pdMS_TO_TICKS(100)); // Simulate press
	            bluetooth_send_media(BLUETOOTH_CMD_VOLUME_UP, false);
			}
			// Vol-down command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_VOLUME_DOWN && bluetooth_state == BT_STATE_RUNNING) {
				bluetooth_send_media(BLUETOOTH_CMD_VOLUME_DOWN, true);
	            vTaskDelay(pdMS_TO_TICKS(100));
	            bluetooth_send_media(BLUETOOTH_CMD_VOLUME_DOWN, false);
			}
			// Next track command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_NEXT_TRK && bluetooth_state == BT_STATE_RUNNING) {
				bluetooth_send_media(BLUETOOTH_CMD_NEXT_TRK, true);
	            vTaskDelay(pdMS_TO_TICKS(100));
	            bluetooth_send_media(BLUETOOTH_CMD_NEXT_TRK, false);
			}
			// Previous track command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_PREV_TRK && bluetooth_state == BT_STATE_RUNNING) {
				bluetooth_send_media(BLUETOOTH_CMD_PREV_TRK, true);
	            vTaskDelay(pdMS_TO_TICKS(100));
	            bluetooth_send_media(BLUETOOTH_CMD_PREV_TRK, false);
			}
			// Play pause command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_PLAY_PAUSE && bluetooth_state == BT_STATE_RUNNING) {
				bluetooth_send_media(BLUETOOTH_CMD_PLAY_PAUSE, true);
	            vTaskDelay(pdMS_TO_TICKS(100));
	            bluetooth_send_media(BLUETOOTH_CMD_PLAY_PAUSE, false);
			}
			/* Text scripts */
			else if (bluetooth_cmd >= BLUETOOTH_SCRIPT_OFFSET && bluetooth_state == BT_STATE_RUNNING) {
				// Menu index that was encoded by the UI
				uint16_t menu_idx = (uint16_t)(bluetooth_cmd - BLUETOOTH_SCRIPT_OFFSET);

				#ifdef POLYCAST5_DEBUG
					ESP_LOGI(TAG, "Received cmd index: %d -> menu index: ", bluetooth_cmd, menu_idx);
				#endif
			
				// "Test" at menu index 1, handle it specially
				if (menu_idx == 1) {
					#define TEST_TXT_LN1 "Thanks for choosing PolyCast5! As you can see, this autotype feature can be quite handy. "
					#define TEST_TXT_LN2 "It's perfect for funny pranks, auto-filling long passwords, speeding up typing, coding, you name it! "
					#define TEST_TXT_LN3 "If you see yourself more an ethical hacker, this is also basically a Bluetooth USB Rubber Ducky. "
					#define TEST_TXT_LN4 "To start adding your own text scripts, just go to 'Add/Edit Script' and follow the few simple instructions.\n"
					bluetooth_send_script(TEST_TXT_LN1 TEST_TXT_LN2 TEST_TXT_LN3 TEST_TXT_LN4, 1);
				}

				// Menu has 2 fixed rows before user scripts:
				// So the first user script is menu_idx == 2 -> NVS index 0
				if (menu_idx >= NUM_KEYBOARD_BASE) {
					uint8_t script_idx = (uint8_t)(menu_idx - NUM_KEYBOARD_BASE); // 0-based NVS slot

					char buf[512];
					size_t blen = 0;

					// Ask NVS for the stored body. Pass full sizeof(buf) so there's room for the NUL.
					esp_err_t err = bluetooth_script_body_get(script_idx, buf, sizeof(buf), &blen);
					if (err == ESP_OK && blen > 0 && buf[0] != '\0') {
						// NVS returns a C-string (blen typically includes the NUL). Just send it.
						#ifdef POLYCAST5_DEBUG
							ESP_LOGI(TAG, "Sending script: %s", buf);
						#endif
						bluetooth_send_script(buf, 1);
					}
					else {
						ESP_LOGW(TAG, "No script body at idx=%u (err=%s, blen=%u)",
								 (unsigned)script_idx, esp_err_to_name(err), (unsigned)blen);
					}
				}
				else {
					ESP_LOGW(TAG, "Unhandled menu_idx=%u for BLUETOOTH_SCRIPT_OFFSET", (unsigned)menu_idx);
				}
			}
		}
		
		// Get device battery level
		xQueueReceive(xAdcBatBluetoothQueue, &battery_percentage, 0);
		
		// Update bluetooth battery level every battery_timer_interval
		if (xTaskGetTickCount() - battery_timer_last >= battery_timer_interval && bluetooth_state == BT_STATE_RUNNING) {
			battery_timer_last = xTaskGetTickCount();
			
			bluetooth_set_battery_level(battery_percentage);
		}
		
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void bluetooth_task_create(void)
{
	if (xTaskCreate(bluetooth_task, "bluetooth_task", 1024 * 4, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start bluetooth_task");
	}
}