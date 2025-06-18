#include "freertos/projdefs.h"
#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "portmacro.h"
#include "esp_log.h"

#include "gpio_task.h"
#include "gpio_funcs.h"

SemaphoreHandle_t xSPIBusMutex;

SemaphoreHandle_t xGpioEventSemaphore;
SemaphoreHandle_t xPowerButtonSemaphore;

SemaphoreHandle_t xUpButtonSemaphore;
SemaphoreHandle_t xDownButtonSemaphore;
SemaphoreHandle_t xRightButtonSemaphore;
SemaphoreHandle_t xLeftButtonSemaphore;
SemaphoreHandle_t xBackButtonSemaphore;
SemaphoreHandle_t xSelectButtonSemaphore;

SemaphoreHandle_t xLedBlueSemaphore;
SemaphoreHandle_t xLedRedSemaphore;
SemaphoreHandle_t xLedGreenSemaphore;
SemaphoreHandle_t xLedOffSemaphore;

static const char *TAG = "GPIO_TASK";

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
    xBackButtonSemaphore = xSemaphoreCreateBinary();
    configASSERT(xBackButtonSemaphore);
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

	gpio_write_output(0, 0); // Red LED
	gpio_write_output(1, 0); // Green LED
	gpio_write_output(2, 0); // Blue LED
	gpio_write_output(3, 1); // 3V enable
	gpio_write_output(4, 0); // NA
	gpio_write_output(5, 0); // NA
	gpio_write_output(6, 0); // NA
	gpio_write_output(7, 0); // NA
		
	while (1) 
	{
		#ifdef POLYCAST5_DEBUG_GPIO
        	//ESP_LOGI(TAG, "GPIO_UP: %d GPIO_DOWN: %d GPIO_RIGHT: %d", gpio_read_input(USER_BUTTON_UP), gpio_read_input(USER_BUTTON_DOWN), gpio_read_input(USER_BUTTON_RIGHT));
        #endif
		
		// If a button is pressed
	    if (xSemaphoreTake(xGpioEventSemaphore, 0) == pdTRUE) {		
	        vTaskDelay(pdMS_TO_TICKS(50)); // Ignore bounce window

			if (gpio_read_input(USER_BUTTON_UP) == 0) {
				xSemaphoreGive(xUpButtonSemaphore);
				#ifdef POLYCAST5_DEBUG_GPIO
		        	ESP_LOGI(TAG, "xUpButtonSemaphore given");
		        #endif
			}
			else if (gpio_read_input(USER_BUTTON_DOWN) == 0) {
				xSemaphoreGive(xDownButtonSemaphore);
				#ifdef POLYCAST5_DEBUG_GPIO
		        	ESP_LOGI(TAG, "xDownButtonSemaphore given");
		        #endif
			}
			else if (gpio_read_input(USER_BUTTON_RIGHT) == 0) {
				xSemaphoreGive(xRightButtonSemaphore);
				#ifdef POLYCAST5_DEBUG_GPIO
		        	ESP_LOGI(TAG, "xRightButtonSemaphore given");
		        #endif
			}
			else if (gpio_read_input(USER_BUTTON_LEFT) == 0) {
				xSemaphoreGive(xLeftButtonSemaphore);
				#ifdef POLYCAST5_DEBUG_GPIO
		        	ESP_LOGI(TAG, "xLeftButtonSemaphore given");
		        #endif
			}
			else if (gpio_read_input(USER_BUTTON_BACK) == 0) {
				xSemaphoreGive(xBackButtonSemaphore); // THIS IS PWR ON NEW HW
				#ifdef POLYCAST5_DEBUG_GPIO
		        	ESP_LOGI(TAG, "xBackButtonSemaphore given");
		        #endif
				
			}
			else if (gpio_read_input(USER_BUTTON_SELECT) == 0) {
				xSemaphoreGive(xSelectButtonSemaphore);
				#ifdef POLYCAST5_DEBUG_GPIO
		        	ESP_LOGI(TAG, "xSelectButtonSemaphore given");
		        #endif
			}
		}
			
		if (xSemaphoreTake(xLedBlueSemaphore, 0) == pdTRUE) {	
			gpio_write_output(2, 1); // Blue LED
		}
		if (xSemaphoreTake(xLedRedSemaphore, 0) == pdTRUE) {	
			gpio_write_output(0, 1); // Red LED
		}
		if (xSemaphoreTake(xLedGreenSemaphore, 0) == pdTRUE) {	
			gpio_write_output(1, 1); // Green LED
		}
		if (xSemaphoreTake(xLedOffSemaphore, 0) == pdTRUE) {	
			gpio_write_output(0, 0); // Red LED
			gpio_write_output(1, 0); // Green LED
			gpio_write_output(2, 0); // Blue LED
		}
	    
	    vTaskDelay(pdMS_TO_TICKS(20));
	}
}

void gpio_task_create(void)
{
	if (xTaskCreate(gpio_task, "gpio_task", 1024 * 2, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start gpio_task");
	}
}
