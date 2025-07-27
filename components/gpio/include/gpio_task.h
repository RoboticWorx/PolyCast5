#ifndef GPIO_TASK_H
#define GPIO_TASK_H

#include "freertos/idf_additions.h"

// Mutex
extern SemaphoreHandle_t xSPIBusMutex;
extern SemaphoreHandle_t xI2CBusMutex;
extern SemaphoreHandle_t xHapticsMutex;
extern SemaphoreHandle_t xRgbLedMutex;

// Regular
extern SemaphoreHandle_t xPowerButtonSemaphore;
extern SemaphoreHandle_t xStartAdcBatSemaphore;

extern SemaphoreHandle_t xUpButtonSemaphore; // Up btn pressed
extern SemaphoreHandle_t xDownButtonSemaphore; // Down btn pressed
extern SemaphoreHandle_t xRightButtonSemaphore; // Right btn pressed
extern SemaphoreHandle_t xLeftButtonSemaphore; // Left btn pressed
extern SemaphoreHandle_t xHomeButtonSemaphore; // Back btn pressed
extern SemaphoreHandle_t xSelectButtonSemaphore; // Select btn pressed

extern SemaphoreHandle_t xIsChargingSemaphore;
extern SemaphoreHandle_t xNotChargingSemaphore;

// Queues
extern QueueHandle_t xAdcBatReadingQueue;
extern QueueHandle_t xLEDQueue;

/**
 * @brief  Create the GPIO expander task.
 *         Internally it calls GPIO_Init(), then
 *         polls P0.0–P0.7 and mirrors each bit to P1.0–P1.7.
 */
void gpio_task_create(void);

#endif // GPIO_TASK_H
