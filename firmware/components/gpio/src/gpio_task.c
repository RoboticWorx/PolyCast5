#include "gpio_task.h"
#include "gpio_funcs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "GPIO_TASK";
#define TASK_STACK_SIZE 4096
#define TASK_PRIORITY   5

static void gpio_task(void *arg)
{

	gpio_write_output(0, 0); // Red LED
	gpio_write_output(1, 0); // Green LED
	gpio_write_output(2, 0); // Blue LED
	gpio_write_output(3, 1); // 3V enable
	gpio_write_output(4, 0); // NA
	gpio_write_output(5, 0); // NA
	gpio_write_output(6, 0); // NA
	gpio_write_output(7, 0); // NA
	
	uint8_t state = 1;

	while (1) {
        
        if (state == 1) state = 0;
        else if (state == 0) state = 1;
        
        //charge status on P06
        
        gpio_write_output(2, state); // Blue LED
        //gpio_write_output(1, !state); // Green LED

		/*int level = gpio_read_input(1);
		if (level >= 0) {
			ESP_LOGI(TAG, "P0.%d = %d → setting P1.%d", 1, level, 1);
			gpio_write_output(1, (bool)level);
		}*/

		vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void gpio_task_create(void)
{
    xTaskCreate(
        gpio_task,
        "gpio_task",
        TASK_STACK_SIZE,
        NULL,
        TASK_PRIORITY,
        NULL
    );
}
