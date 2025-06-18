#ifndef GPIO_TASK_H
#define GPIO_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define USER_BUTTON_LEFT 1
#define USER_BUTTON_UP 0
#define USER_BUTTON_RIGHT 2
#define USER_BUTTON_BACK 3
#define USER_BUTTON_DOWN 4
#define USER_BUTTON_SELECT 5
#define USER_BUTTON_POWER 1

// ISR semaphores
extern SemaphoreHandle_t xPowerButtonSemaphore; // Select btn pressed
extern SemaphoreHandle_t xGpioEventSemaphore; // A btn was pressed

// Mutex
extern SemaphoreHandle_t xSPIBusMutex;

// Regular
extern SemaphoreHandle_t xUpButtonSemaphore; // Up btn pressed
extern SemaphoreHandle_t xDownButtonSemaphore; // Down btn pressed
extern SemaphoreHandle_t xRightButtonSemaphore; // Right btn pressed
extern SemaphoreHandle_t xLeftButtonSemaphore; // Left btn pressed
extern SemaphoreHandle_t xBackButtonSemaphore; // Back btn pressed
extern SemaphoreHandle_t xSelectButtonSemaphore; // Select btn pressed

extern SemaphoreHandle_t xLedBlueSemaphore;
extern SemaphoreHandle_t xLedRedSemaphore;
extern SemaphoreHandle_t xLedGreenSemaphore;
extern SemaphoreHandle_t xLedOffSemaphore;



/**
 * @brief  Create the GPIO expander task.
 *         Internally it calls GPIO_Init(), then
 *         polls P0.0–P0.7 and mirrors each bit to P1.0–P1.7.
 */
void gpio_task_create(void);

#endif // GPIO_TASK_H
