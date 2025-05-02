#include "lcd_task.h"
#include "lcd_funcs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ACTIVE_SCR (lv_disp_get_scr_act(lcd_get_display()))

static void lcd_task(void *pvParameters)
{
    (void)pvParameters;
    
    lv_obj_set_style_bg_color(ACTIVE_SCR, lv_color_hex(0x0047FF), 0);
    lv_obj_set_style_bg_opa(ACTIVE_SCR, LV_OPA_COVER, 0); // Ensure the background is fully opaque

    lv_obj_t *label = lv_label_create(ACTIVE_SCR);
    lv_label_set_text(label, "Introducing PolyCast5");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
    
    

    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void lcd_task_create(void)
{
    xTaskCreatePinnedToCore(
        lcd_task,        /* task code */
        "lcd_task",     /* name */
        4096*2,            /* stack bytes */
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        0);              /* run on PRO CPU (core0) */
}