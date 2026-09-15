#ifndef SIM_COMPAT_H
#define SIM_COMPAT_H

/**
 * Desktop stand-ins for the handful of ESP-IDF and firmware headers that shared LCD modules
 * pull in, so one .c file can build for both the device and this SDL simulator.
 *
 * Only what those modules actually use is here. It is deliberately not a general ESP-IDF shim:
 * anything that needs more than this belongs in the firmware, not in a shared drawing module.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lvgl.h"

/* PSRAM placement is meaningless on a desktop - the attributes just vanish. */
#define POLYCAST5_USE_PSRAM_BSS
#define POLYCAST5_USE_PSRAM_DATA

/* The device puts big buffers in PSRAM explicitly; here every allocation is the same heap. */
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT   0
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void  heap_caps_free(void *p) { free(p); }

#define ESP_LOGE(tag, ...) do { fprintf(stderr, "E (%s) ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define ESP_LOGW(tag, ...) do { fprintf(stderr, "W (%s) ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define ESP_LOGI(tag, ...) do { fprintf(stdout, "I (%s) ", tag); fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); } while (0)

/* On the device this is the microphone level; screens.c fakes a speech envelope instead. */
uint16_t ai_voice_take_level_peak(void);

/* Real ones live in lcd_task.c; screens.c defines them for the simulator. */
extern lv_color_t user_primary_color;
extern lv_color_t user_secondary_color;

#endif /* SIM_COMPAT_H */
