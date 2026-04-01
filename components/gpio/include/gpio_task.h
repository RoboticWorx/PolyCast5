#ifndef GPIO_TASK_H
#define GPIO_TASK_H

#include "freertos/idf_additions.h"

// Mutex
extern SemaphoreHandle_t xSPIBusMutex;
extern SemaphoreHandle_t xI2CBusMutex;
extern SemaphoreHandle_t xHapticsMutex;
extern SemaphoreHandle_t xRgbLedMutex;
extern SemaphoreHandle_t xLEDCMutex;
extern SemaphoreHandle_t xGpioLeftBtnMutex;

// Regular
extern SemaphoreHandle_t xPowerButtonSemaphore;
extern SemaphoreHandle_t xStartAdcBatSemaphore;

// Short presses
extern SemaphoreHandle_t xUpButtonSemaphore; // Up btn pressed
extern SemaphoreHandle_t xDownButtonSemaphore; // Down btn pressed
extern SemaphoreHandle_t xRightButtonSemaphore; // Right btn pressed
extern SemaphoreHandle_t xLeftButtonSemaphore; // Left btn pressed
extern SemaphoreHandle_t xHomeButtonSemaphore; // Back btn pressed
extern SemaphoreHandle_t xSelectButtonSemaphore; // Select btn pressed

// Long presses
extern SemaphoreHandle_t xSelectButtonLongSemaphore;  
extern SemaphoreHandle_t xHomeButtonLongSemaphore;    
extern SemaphoreHandle_t xUpButtonLongSemaphore;      
extern SemaphoreHandle_t xDownButtonLongSemaphore;    
extern SemaphoreHandle_t xLeftButtonLongSemaphore;    
extern SemaphoreHandle_t xRightButtonLongSemaphore; 

extern SemaphoreHandle_t xIsChargingSemaphore;
extern SemaphoreHandle_t xNotChargingSemaphore;

extern SemaphoreHandle_t xLEDCSemaphore;

// Queues
extern QueueHandle_t xAdcBatReadingQueue;
extern QueueHandle_t xAdcBatBluetoothQueue;
extern QueueHandle_t xLEDQueue;

/**
 * @brief  Create the GPIO expander task.
 *         Internally it calls gpio_utils_init(), then
 *         polls P0.0–P0.7 and mirrors each bit to P1.0–P1.7.
 */
void gpio_task_create(void);

#endif // GPIO_TASK_H
