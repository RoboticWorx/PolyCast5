#ifndef AI_TASK_H
#define AI_TASK_H

#include "freertos/idf_additions.h"

#define AI_DONE_THINKING_BIT   (1U << 0) // Successful completion
#define AI_THINKING_FAILED_BIT (1U << 1) // Generic failure
#define AI_RATE_LIMITED_BIT    (1U << 2) // Out of API credits
#define AI_NO_MATCH_BIT        (1U << 3) // No saved entry matched a cred/custom lookup
#define AI_BUSY_BIT            (1U << 4) // Set while ai_task runs a command; xAiCmdQueue may still hold the next one
extern EventGroupHandle_t xAiEventGroup;

extern QueueHandle_t xAiCmdQueue;

// Bumped by lcd_task on every AI page exit, so a request still running then never reaches a later visit
extern volatile uint32_t ai_visit_gen;

/** 
 * @brief Create and start the ai_task
 */
void ai_task_create(void);

#endif // AI_TASK_H