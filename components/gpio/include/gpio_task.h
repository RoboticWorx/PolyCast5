#ifndef GPIO_TASK_H
#define GPIO_TASK_H

#include "freertos/idf_additions.h"

// ISR semaphores
extern SemaphoreHandle_t xPowerButtonSemaphore; // Select btn pressed

// Mutex
extern SemaphoreHandle_t xSPIBusMutex;

// Regular
extern SemaphoreHandle_t xUpButtonSemaphore; // Up btn pressed
extern SemaphoreHandle_t xDownButtonSemaphore; // Down btn pressed
extern SemaphoreHandle_t xRightButtonSemaphore; // Right btn pressed
extern SemaphoreHandle_t xLeftButtonSemaphore; // Left btn pressed
extern SemaphoreHandle_t xHomeButtonSemaphore; // Back btn pressed
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
