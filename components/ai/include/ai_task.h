#ifndef AI_TASK_H
#define AI_TASK_H

#include "esp_err.h"
#include "freertos/idf_additions.h"

#define AI_DONE_THINKING_BIT   (1U << 0) // Successful completion
#define AI_THINKING_FAILED_BIT (1U << 1) // Failure: ai_task_get_fail() says where and why
#define AI_NO_MATCH_BIT        (1U << 3) // No saved entry matched a cred/custom lookup
#define AI_BUSY_BIT            (1U << 4) // Set while ai_task runs a command; xAiCmdQueue may still hold the next one
extern EventGroupHandle_t xAiEventGroup;

extern QueueHandle_t xAiCmdQueue;

// Bumped by lcd_task on every AI page exit, so a request still running then never reaches a later visit
extern volatile uint32_t ai_visit_gen;

// Which step of an AI keyboard request failed
typedef enum {
    AI_FAIL_STAGE_MIC = 0, // Recording
    AI_FAIL_STAGE_STT,     // Speech-to-text
    AI_FAIL_STAGE_CHAT,    // AI answer to type
    AI_FAIL_STAGE_LOOKUP,  // Username/password/custom lookup
} ai_fail_stage_t;

typedef struct {
    ai_fail_stage_t stage;
    esp_err_t err; // AI_ERR_* (ai_utils.h) or an ESP-IDF error
} ai_fail_t;

/**
 * @brief Where and why the last AI keyboard request failed
 *
 * @returns The failure behind the last AI_THINKING_FAILED_BIT
 */
ai_fail_t ai_task_get_fail(void);

/** 
 * @brief Create and start the ai_task
 */
void ai_task_create(void);

#endif // AI_TASK_H
