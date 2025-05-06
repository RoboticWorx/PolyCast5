#include "gpio_task.h"
#include "gpio_funcs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "GPIO_TASK";

SemaphoreHandle_t xGpioEventSemaphore;
SemaphoreHandle_t xUpButtonSemaphore;
SemaphoreHandle_t xDownButtonSemaphore;

static void gpio_task(void *arg)
{
	xUpButtonSemaphore = xSemaphoreCreateBinary();
    xDownButtonSemaphore = xSemaphoreCreateBinary();

	gpio_write_output(0, 0); // Red LED
	gpio_write_output(1, 0); // Green LED
	gpio_write_output(2, 0); // Blue LED
	gpio_write_output(3, 1); // 3V enable
	gpio_write_output(4, 0); // NA
	gpio_write_output(5, 0); // NA
	gpio_write_output(6, 0); // NA
	gpio_write_output(7, 0); // NA

	bool one_button_press = true;
	
	while (1) {
		// If a button is pressed
	    if (xSemaphoreTake(xGpioEventSemaphore, portMAX_DELAY)) {
	        vTaskDelay(pdMS_TO_TICKS(50)); // Ignore bounce window
	
	        if (one_button_press) {
	            if (gpio_read_input(USER_BUTTON_UP) == 0) {
	                xSemaphoreGive(xUpButtonSemaphore);
	            }
	            else if (gpio_read_input(USER_BUTTON_DOWN) == 0) {
	                xSemaphoreGive(xDownButtonSemaphore);
	            }
	            
	            one_button_press = false;
	        }
	    }

	    // Re-arm logic
	    if (!one_button_press) {
	        if (gpio_read_input(USER_BUTTON_UP) == 1 && gpio_read_input(USER_BUTTON_DOWN) == 1) {
	            one_button_press = true;
	        }
	    }
		
	    vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void gpio_task_create(void)
{
	xTaskCreate(gpio_task, "gpio_task", 4096, NULL, tskIDLE_PRIORITY + 1,
				NULL);
}
