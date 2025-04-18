#ifndef BLUETOOTH_TASK_H
#define BLUETOOTH_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void ble_hid_demo_task(void *pvParameters);
void ble_hid_task_start_up(void);
void ble_hid_task_shut_down(void);

#endif // BLUETOOTH_TASK_H