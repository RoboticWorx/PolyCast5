#ifndef GPIO_TASK_H
#define GPIO_TASK_H

/**
 * @brief  Create the GPIO expander task.
 *         Internally it calls GPIO_Init(), then
 *         polls P0.0–P0.7 and mirrors each bit to P1.0–P1.7.
 */
void gpio_task_create(void);

#endif // GPIO_TASK_H
