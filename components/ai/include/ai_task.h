#ifndef AI_TASK_H
#define AI_TASK_H

#include "freertos/idf_additions.h"

#define AI_DONE_THINKING_BIT   (1U << 0) // Successful completion
#define AI_THINKING_FAILED_BIT (1U << 1) // Generic failure
#define AI_RATE_LIMITED_BIT    (1U << 2) // Out of API credits
#define AI_NO_MATCH_BIT        (1U << 3) // No saved entry matched a cred/custom lookup
extern EventGroupHandle_t xAiEventGroup;

extern QueueHandle_t xAiCmdQueue;

extern SemaphoreHandle_t xAiSoundHeardSemaphore;

/** 
 * @brief Create and start the ai_task
 */
void ai_task_create(void);

#endif // AI_TASK_H