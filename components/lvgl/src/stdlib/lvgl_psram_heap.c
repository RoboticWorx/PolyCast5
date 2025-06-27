#include "esp_heap_caps.h"
#include "lvgl_psram_heap.h"

/* LVGL calls this once at start-up. Nothing to do for a system malloc. */
void lv_mem_init(void)
{
    /* no-op */
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void * ptr)
{
    heap_caps_free(ptr);
}

void *lv_realloc_core(void * ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
