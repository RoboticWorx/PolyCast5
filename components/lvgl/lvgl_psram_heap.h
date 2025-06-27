// Added to implement custom functions for LVGL to draw from PSRAM in order to save DRAM
// Found in src/stdlib/lvgl_psram_heap.c 

#ifndef LVGL_PSRAM_HEAP_H
#define LVGL_PSRAM_HEAP_H

#include <stdlib.h>

void lv_mem_init(void);

void *lv_malloc_core(size_t size);

void lv_free_core(void * ptr);

void *lv_realloc_core(void * ptr, size_t size);

#endif // LVGL_PSRAM_HEAP_H
