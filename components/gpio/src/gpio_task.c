#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "portmacro.h"

#include "esp_log.h"

#include "gpio_task.h"
#include "gpio_funcs.h"

#define TAG "GPIO_TASK"

#define POLL_MS 20
#define REPEAT_START_MS 400
#define REPEAT_NEXT_MS 100

SemaphoreHandle_t xSPIBusMutex;
SemaphoreHandle_t xI2CBusMutex;

SemaphoreHandle_t xPowerButtonSemaphore;
SemaphoreHandle_t xStartAdcBatSemaphore;

SemaphoreHandle_t xUpButtonSemaphore;
SemaphoreHandle_t xDownButtonSemaphore;
SemaphoreHandle_t xRightButtonSemaphore;
SemaphoreHandle_t xLeftButtonSemaphore;
SemaphoreHandle_t xHomeButtonSemaphore;
SemaphoreHandle_t xSelectButtonSemaphore;

SemaphoreHandle_t xLedBlueSemaphore;
SemaphoreHandle_t xLedRedSemaphore;
SemaphoreHandle_t xLedGreenSemaphore;
SemaphoreHandle_t xLedOffSemaphore;

QueueHandle_t xAdcBatReadingQueue;

typedef struct {
    uint8_t pin; // Expander pin number
    uint16_t ticks; // Ticks until next event
    bool prev; // Last sampled state (1 = released, 0 = pressed)
} btn_state_t;

// Buttons and states: same order as buttonSemaphores
static btn_state_t buttons[6] = {
    {USER_BUTTON_UP,     0, 1},
    {USER_BUTTON_DOWN,   0, 1},
    {USER_BUTTON_RIGHT,  0, 1},
    {USER_BUTTON_LEFT,   0, 1},
    {USER_BUTTON_HOME,   0, 1},
    {USER_BUTTON_SELECT, 0, 1},
};

static SemaphoreHandle_t *buttonSemaphores[] = {
    &xUpButtonSemaphore,
    &xDownButtonSemaphore,
    &xRightButtonSemaphore,
    &xLeftButtonSemaphore,
    &xHomeButtonSemaphore,
    &xSelectButtonSemaphore,
};

// Helper to “give” the right semaphore based on index
static inline void give_button_sem(size_t i)
{
    // Dereference the pointer and give it
    xSemaphoreGive(*buttonSemaphores[i]);
}

static const TickType_t adc_timer_interval = pdMS_TO_TICKS(20000); // 20s
static uint32_t haptic_ms = 20;

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
    
    xLedBlueSemaphore = xSemaphoreCreateBinary();
    configASSERT(xLedBlueSemaphore);
    xLedRedSemaphore = xSemaphoreCreateBinary();
    configASSERT(xLedRedSemaphore);
    xLedGreenSemaphore = xSemaphoreCreateBinary();
    configASSERT(xLedGreenSemaphore);
    xLedOffSemaphore = xSemaphoreCreateBinary();
    configASSERT(xLedOffSemaphore);
    
    xStartAdcBatSemaphore = xSemaphoreCreateBinary();
	configASSERT(xStartAdcBatSemaphore);
    
    xAdcBatReadingQueue = xQueueCreate(1, sizeof(uint8_t));
	configASSERT(xAdcBatReadingQueue);
    
	gpio_write_output(0, 1); // Red LED
	gpio_write_output(1, 1); // Green LED
	gpio_write_output(2, 1); // Blue LED
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
	                
	                // If select button
	                if (i == 5) {
						gpio_spin_haptic(haptic_ms);
					}
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
	    
	    // Reset hotkey
	    if (gpio_read_input(USER_BUTTON_HOME) == 0 && gpio_read_input(USER_BUTTON_RIGHT) == 0) {
			esp_restart();
		}
	
	    // RGB LED handling
	    if (xSemaphoreTake(xLedBlueSemaphore, 0) == pdTRUE) {
			gpio_write_output(2, 1);
		}
	    if (xSemaphoreTake(xLedRedSemaphore, 0) == pdTRUE) {
			gpio_write_output(0, 1);
		}
	    if (xSemaphoreTake(xLedGreenSemaphore, 0) == pdTRUE) {
			gpio_write_output(1, 1);
		}
	    if (xSemaphoreTake(xLedOffSemaphore, 0) == pdTRUE) {
	        gpio_write_output(0, 0);
	        gpio_write_output(1, 0);
	        gpio_write_output(2, 0);
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
