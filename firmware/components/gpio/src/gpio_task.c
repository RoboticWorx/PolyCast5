#include "gpio_task.h"
#include "gpio_funcs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "GPIO_TASK";

SemaphoreHandle_t xGpioEventSemaphore;
SemaphoreHandle_t xUpButtonSemaphore;
SemaphoreHandle_t xDownButtonSemaphore;
SemaphoreHandle_t xRightButtonSemaphore;
SemaphoreHandle_t xLeftButtonSemaphore;
SemaphoreHandle_t xBackButtonSemaphore;
SemaphoreHandle_t xHomeButtonSemaphore;

static void gpio_task(void *arg)
{
	xUpButtonSemaphore = xSemaphoreCreateBinary();
    xDownButtonSemaphore = xSemaphoreCreateBinary();
    xRightButtonSemaphore = xSemaphoreCreateBinary();
    xLeftButtonSemaphore = xSemaphoreCreateBinary();
    xBackButtonSemaphore = xSemaphoreCreateBinary();
    xHomeButtonSemaphore = xSemaphoreCreateBinary();

	gpio_write_output(0, 0); // Red LED
	gpio_write_output(1, 0); // Green LED
	gpio_write_output(2, 0); // Blue LED
	gpio_write_output(3, 1); // 3V enable
	gpio_write_output(4, 0); // NA
	gpio_write_output(5, 0); // NA
	gpio_write_output(6, 0); // NA
	gpio_write_output(7, 0); // NA

	bool one_button_press = true;
	
	while (1) 
	{
		
		//ESP_LOGI(TAG, "GPIO_UP: %d GPIO_DOWN: %d GPIO_RIGHT: %d", gpio_read_input(USER_BUTTON_UP), gpio_read_input(USER_BUTTON_DOWN), gpio_read_input(USER_BUTTON_RIGHT));
		
		// If a button is pressed
	    if (xSemaphoreTake(xGpioEventSemaphore, 10)) {
	        vTaskDelay(pdMS_TO_TICKS(50)); // Ignore bounce window
	
	        if (one_button_press) {
	            if (gpio_read_input(USER_BUTTON_UP) == 0) {
	                xSemaphoreGive(xUpButtonSemaphore);
	            }
	            else if (gpio_read_input(USER_BUTTON_DOWN) == 0) {
	                xSemaphoreGive(xDownButtonSemaphore);
	            }
	            else if (gpio_read_input(USER_BUTTON_RIGHT) == 0) {
	                xSemaphoreGive(xRightButtonSemaphore);
	            }
	            else if (gpio_read_input(USER_BUTTON_LEFT) == 0) {
	                xSemaphoreGive(xLeftButtonSemaphore);
	            }
	            else if (gpio_read_input(USER_BUTTON_BACK) == 0) {
	                xSemaphoreGive(xBackButtonSemaphore);
	            }
	            else if (gpio_read_input(USER_BUTTON_HOME) == 0) {
	                xSemaphoreGive(xHomeButtonSemaphore);
	            }
	            
	            one_button_press = false;
	        }
	    }

		// Re-arm logic
		if (!one_button_press) {
	
			if (gpio_read_input(USER_BUTTON_UP) == 1) // && gpio_read_input(USER_BUTTON_DOWN) == 1)
			{
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
