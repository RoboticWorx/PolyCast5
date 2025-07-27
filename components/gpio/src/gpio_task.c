#include "freertos/idf_additions.h"
#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "portmacro.h"

#include "hal/adc_hal.h"
#include "esp_log.h"

#include "gpio_task.h"
#include "gpio_funcs.h"

#define TAG "GPIO_TASK"

#define POLL_MS 20
#define REPEAT_START_MS 400
#define REPEAT_NEXT_MS 100

SemaphoreHandle_t xSPIBusMutex;
SemaphoreHandle_t xI2CBusMutex;
SemaphoreHandle_t xHapticsMutex;
SemaphoreHandle_t xRgbLedMutex;

SemaphoreHandle_t xPowerButtonSemaphore;
SemaphoreHandle_t xStartAdcBatSemaphore;

SemaphoreHandle_t xUpButtonSemaphore;
SemaphoreHandle_t xDownButtonSemaphore;
SemaphoreHandle_t xRightButtonSemaphore;
SemaphoreHandle_t xLeftButtonSemaphore;
SemaphoreHandle_t xHomeButtonSemaphore;
SemaphoreHandle_t xSelectButtonSemaphore;

SemaphoreHandle_t xIsChargingSemaphore;
SemaphoreHandle_t xNotChargingSemaphore;

QueueHandle_t xAdcBatReadingQueue;
QueueHandle_t xLEDQueue;

typedef struct {
	uint8_t pin; // Expander pin number
	uint16_t ticks; // Ticks until next event
	bool prev; // Last sampled state (1 = released, 0 = pressed)
} btn_state_t;

volatile uint8_t haptic_len_ms = 20; // Default buzz 20ms
volatile bool haptic_btns[6] = {true, false, false, false, false, false}; // Default buzz on select

static const TickType_t adc_timer_interval = pdMS_TO_TICKS(20000); // 20s

static uint8_t rgb_data = 255;

// Buttons and states: same order as buttonSemaphores
static btn_state_t buttons[6] = {
	{USER_BUTTON_SELECT, 0, 1},
	{USER_BUTTON_HOME,   0, 1},
	{USER_BUTTON_UP,	 0, 1},
	{USER_BUTTON_DOWN,   0, 1},
	{USER_BUTTON_LEFT,   0, 1},
	{USER_BUTTON_RIGHT,  0, 1},
};

static SemaphoreHandle_t *buttonSemaphores[] = {
	&xSelectButtonSemaphore,
	&xHomeButtonSemaphore,
	&xUpButtonSemaphore,
	&xDownButtonSemaphore,
	&xLeftButtonSemaphore,
	&xRightButtonSemaphore,
};

// Helper to “give” the right semaphore based on index
static inline void give_button_sem(size_t i)
{
	// Dereference the pointer and give it
	xSemaphoreGive(*buttonSemaphores[i]);
}

static void adc_task(void *arg)
{
	static uint8_t last_percentage = 100;
		
	// Get battery charge on start
	gpio_init_battery_adc();
	float v = gpio_get_battery_voltage();
	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Startup voltage: %f", v);
	#endif
	gpio_deinit_battery_adc();
		
	uint8_t percentage = gpio_volts_to_soc(v);
	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Startup percentage: %u%%", percentage);
	#endif
	
	last_percentage = percentage;
	
	// Send startup value to LCD
	if (xQueueSend(xAdcBatReadingQueue, &percentage, portMAX_DELAY) != pdPASS) {
		ESP_LOGE(TAG, "Failed to send xAdcBatReadingQueue: %%%u", percentage);
	}
	
	TickType_t adc_timer_last = xTaskGetTickCount();
		
	while (1) {
		// Update battery status every adc_timer_interval
		if ((xTaskGetTickCount() - adc_timer_last >= adc_timer_interval) || (xSemaphoreTake(xStartAdcBatSemaphore, 0) == pdTRUE)) {
			adc_timer_last = xTaskGetTickCount();
				
			gpio_init_battery_adc();
			float v = gpio_get_battery_voltage();
			gpio_deinit_battery_adc();
						
			uint8_t percentage = gpio_volts_to_soc(v);
			
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "Battery voltage: %f", v);
				ESP_LOGI(TAG, "Battery percentage: %u%%", percentage);
			#endif
			
			// If fluctuating by one, ignore
			if (percentage == last_percentage + 1) {
				percentage = last_percentage;
			}
			else {
				last_percentage = percentage;
			}
			
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "NEW battery percentage: %u%%", percentage);
			#endif
			
			// Send value to LCD
			if (xQueueSend(xAdcBatReadingQueue, &percentage, portMAX_DELAY) != pdPASS) {
				ESP_LOGE(TAG, "Failed to send xAdcBatReadingQueue: %u%%", percentage);
			}
		}
		
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

static void gpio_task(void *arg)
{
	xHapticsMutex = xSemaphoreCreateMutex();
	configASSERT(xHapticsMutex);
	xRgbLedMutex = xSemaphoreCreateMutex();
	configASSERT(xRgbLedMutex);
	
	xUpButtonSemaphore = xSemaphoreCreateBinary();
	configASSERT(xUpButtonSemaphore);
	xDownButtonSemaphore = xSemaphoreCreateBinary();
	configASSERT(xDownButtonSemaphore);
	xRightButtonSemaphore = xSemaphoreCreateBinary();
	configASSERT(xRightButtonSemaphore);
	xLeftButtonSemaphore = xSemaphoreCreateBinary();
	configASSERT(xLeftButtonSemaphore);
	xHomeButtonSemaphore = xSemaphoreCreateBinary();
	configASSERT(xHomeButtonSemaphore);
	xSelectButtonSemaphore = xSemaphoreCreateBinary();
	configASSERT(xSelectButtonSemaphore);
	
	xIsChargingSemaphore = xSemaphoreCreateBinary();
	configASSERT(xIsChargingSemaphore);
	xNotChargingSemaphore = xSemaphoreCreateBinary();
	configASSERT(xNotChargingSemaphore);
	
	xStartAdcBatSemaphore = xSemaphoreCreateBinary();
	configASSERT(xStartAdcBatSemaphore);
	
	xAdcBatReadingQueue = xQueueCreate(1, sizeof(uint8_t));
	configASSERT(xAdcBatReadingQueue);
	xLEDQueue = xQueueCreate(1, sizeof(uint8_t));
	configASSERT(xLEDQueue);
		
	gpio_write_output(RED_RGB_LED_PIN, 0); // Red LED
	gpio_write_output(GREEN_RGB_LED_PIN, 0); // Green LED
	gpio_write_output(BLUE_RGB_LED_PIN, 0); // Blue LED
	gpio_write_output(3, 0); // NC
	gpio_write_output(4, 0); // NC
	gpio_write_output(5, 0); // NC
	gpio_write_output(6, 0); // NC
	gpio_write_output(7, 0); // NC
	
	#ifdef POLYCAST5_CYCLE_RGB_ON_BOOT
		gpio_cycle_rgb();
	#endif
	
	if (xTaskCreate(adc_task, "adc_task", 1024 * 2, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start adc_task");
	}
	
	// Get opposite initial charging state to update once
	bool was_charging = !(gpio_read_input(CHG_IND_PIN) == 0);
	
	while (1) 
	{
		#ifdef POLYCAST5_DEBUG_GPIO
			//ESP_LOGI(TAG, "GPIO_UP: %d GPIO_DOWN: %d GPIO_RIGHT: %d", gpio_read_input(USER_BUTTON_UP), gpio_read_input(USER_BUTTON_DOWN), gpio_read_input(USER_BUTTON_RIGHT));
		#endif
	
		// Press + auto-repeat state machine
		for (size_t i = 0; i < 6; i++) {
			btn_state_t *b = &buttons[i]; // Get the button
			bool level = gpio_read_input(b->pin); // Read its state: 0 = pressed, 1 = released
	
			// Button pressed
			if (level == 0) {
				if (b->prev == 1) { // New press
					give_button_sem(i); // Signal the press
					b->ticks = REPEAT_START_MS / POLL_MS;
					
					// Haptic feedback if this button is enabled in settings
					xSemaphoreTake(xHapticsMutex, portMAX_DELAY); // Lock haptics
					if (haptic_btns[i]) {
						gpio_spin_haptic(haptic_len_ms);
					}
					xSemaphoreGive(xHapticsMutex); // Release haptics
				}
				else if (b->ticks == 0) { // Time to auto-repeat
					give_button_sem(i); // Repeat the press
					b->ticks = REPEAT_NEXT_MS / POLL_MS;
				}
				else { // Waiting for next repeat
					b->ticks--; // Tick down
				}
			}
			else { // Button released
				// Reset for next press
				b->ticks = 0;
			}
			
			// Set previous
			b->prev = level;
		}
		
		// Go to sleep requested
		if (gpio_read_input(USER_BUTTON_POWER) == 0) {
			xSemaphoreGive(xPowerButtonSemaphore);
		}
		
		// Update LCD based on if charging or not
		bool is_charging = (gpio_read_input(CHG_IND_PIN) == 0);
		if (is_charging != was_charging) { // Only update on state change
			// LiPo is charging	
			if (is_charging) {
				xSemaphoreGive(xIsChargingSemaphore);
			}
			// LiPo is not charging
			else {
				xSemaphoreGive(xNotChargingSemaphore);
			}
			xSemaphoreGive(xStartAdcBatSemaphore); // Update battery reading
			
			was_charging = is_charging;
		}
		
		// Reset hotkey
		if (gpio_read_input(USER_BUTTON_HOME) == 0 && gpio_read_input(USER_BUTTON_RIGHT) == 0) {
			esp_restart();
		}
		
		// RGB LED handling
		if (xQueueReceive(xLEDQueue, &rgb_data, 0) == pdTRUE) {
			gpio_rgb_indicate(rgb_data);
		}
		
		vTaskDelay(pdMS_TO_TICKS(POLL_MS));
	}
}

void gpio_task_create(void)
{
	if (xTaskCreate(gpio_task, "gpio_task", 1024 * 2, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start gpio_task");
	}
}
