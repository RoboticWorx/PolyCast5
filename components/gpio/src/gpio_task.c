#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "portmacro.h"

#include "gpio_task.h"
#include "gpio_funcs.h"

static const char *TAG = "GPIO_TASK";

SemaphoreHandle_t xSPIBusMutex;

SemaphoreHandle_t xGpioEventSemaphore;
SemaphoreHandle_t xUpButtonSemaphore;
SemaphoreHandle_t xDownButtonSemaphore;
SemaphoreHandle_t xRightButtonSemaphore;
SemaphoreHandle_t xLeftButtonSemaphore;
SemaphoreHandle_t xBackButtonSemaphore;
SemaphoreHandle_t xSelectButtonSemaphore;

static void gpio_task(void *arg)
{
	xSPIBusMutex = xSemaphoreCreateMutex();
	configASSERT(xSPIBusMutex); // Ensure success
	
	xUpButtonSemaphore = xSemaphoreCreateBinary();
    xDownButtonSemaphore = xSemaphoreCreateBinary();
    xRightButtonSemaphore = xSemaphoreCreateBinary();
    xLeftButtonSemaphore = xSemaphoreCreateBinary();
    xBackButtonSemaphore = xSemaphoreCreateBinary();
    xSelectButtonSemaphore = xSemaphoreCreateBinary();

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
		#ifdef POLYCAST5_GPIO_DEBUG
        	//ESP_LOGI(TAG, "GPIO_UP: %d GPIO_DOWN: %d GPIO_RIGHT: %d", gpio_read_input(USER_BUTTON_UP), gpio_read_input(USER_BUTTON_DOWN), gpio_read_input(USER_BUTTON_RIGHT));
        #endif
		
		// If a button is pressed
	    if (xSemaphoreTake(xGpioEventSemaphore, portMAX_DELAY)) {
	        vTaskDelay(pdMS_TO_TICKS(50)); // Ignore bounce window

			if (gpio_read_input(USER_BUTTON_UP) == 0) {
				xSemaphoreGive(xUpButtonSemaphore);
				#ifdef POLYCAST5_GPIO_DEBUG
		        	ESP_LOGI(TAG, "xUpButtonSemaphore given");
		        #endif
			}
			else if (gpio_read_input(USER_BUTTON_DOWN) == 0) {
				xSemaphoreGive(xDownButtonSemaphore);
				#ifdef POLYCAST5_GPIO_DEBUG
		        	ESP_LOGI(TAG, "xDownButtonSemaphore given");
		        #endif
			}
			else if (gpio_read_input(USER_BUTTON_RIGHT) == 0) {
				xSemaphoreGive(xRightButtonSemaphore);
				#ifdef POLYCAST5_GPIO_DEBUG
		        	ESP_LOGI(TAG, "xRightButtonSemaphore given");
		        #endif
			}
			else if (gpio_read_input(USER_BUTTON_LEFT) == 0) {
				xSemaphoreGive(xLeftButtonSemaphore);
				#ifdef POLYCAST5_GPIO_DEBUG
		        	ESP_LOGI(TAG, "xLeftButtonSemaphore given");
		        #endif
			}
			else if (gpio_read_input(USER_BUTTON_BACK) == 0) {
				xSemaphoreGive(xBackButtonSemaphore); // THIS IS PWR ON NEW HW
				#ifdef POLYCAST5_GPIO_DEBUG
		        	ESP_LOGI(TAG, "xBackButtonSemaphore given");
		        #endif
				
			}
			else if (gpio_read_input(USER_BUTTON_SELECT) == 0) {
				xSemaphoreGive(xSelectButtonSemaphore);
				#ifdef POLYCAST5_GPIO_DEBUG
		        	ESP_LOGI(TAG, "xSelectButtonSemaphore given");
		        #endif
				
			}
		}
	}
}

void gpio_task_create(void)
{
	xTaskCreate(gpio_task, "gpio_task", 4096, NULL, tskIDLE_PRIORITY + 1,
				NULL);
}
