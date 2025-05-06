#ifndef GPIO_TASK_H
#define GPIO_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

//#define USER_BUTTON_LEFT 
#define USER_BUTTON_UP 1
//#define USER_BUTTON_RIGHT 
#define USER_BUTTON_DOWN 5
//#define USER_BUTTON_HOME 
//#define USER_BUTTON_MISC 
//#define USER_BUTTON_POWER 

extern SemaphoreHandle_t xUpButtonSemaphore;
extern SemaphoreHandle_t xDownButtonSemaphore;
extern SemaphoreHandle_t xGpioEventSemaphore;

/**
 * @brief  Create the GPIO expander task.
 *         Internally it calls GPIO_Init(), then
 *         polls P0.0–P0.7 and mirrors each bit to P1.0–P1.7.
 */
void gpio_task_create(void);

#endif // GPIO_TASK_H
