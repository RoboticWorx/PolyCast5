#ifndef AI_TASK_H
#define AI_TASK_H

#include "freertos/idf_additions.h"

extern QueueHandle_t xAiCmdQueue;

/** 
 * @brief Create and start the ai_task
 */
void ai_task_create(void);

#endif // AI_TASK_H