#ifndef STUB_FREERTOS_H
#define STUB_FREERTOS_H
#include <stdint.h>
#include "esp_attr.h" /* as the real chain does, via portmacro.h */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
typedef uint32_t TickType_t;
#endif
