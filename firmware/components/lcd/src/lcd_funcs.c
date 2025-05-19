#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "esp_log.h"

#include "st7789.h"

#include "lcd_funcs.h"
#include "lcd_task.h"
#include "gpio_task.h"
#include "infrared_funcs.h"

#define DRAW_LINES   20
#define FLUSH_CHUNK  2

#define SWIPE_SPEED 1200
#define SCROLL_SPEED 400
#define IR_LABELS_OFFSET 20

static const char *TAG = "LCD_FUNCS";


static TFT_t tft;
static lv_display_t *disp; // LVGL display handle

static bool already_scrolling = false;

typedef struct {
    lv_obj_t * top;    // the label that sits at the top line
    lv_obj_t * mid;    // the label in the center
    lv_obj_t * bot;    // the label at the bottom (this one moves)
    const char * txt;  // the next string to show
    bool up;           // direction: true=you’re scrolling up, false=scrolling down
} scroll_ctx_t;

static bool scrolling_menu = false;
static bool scrolling_up = false;	
	

static void st7789_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t *color_ptr = (const uint16_t *)px_map;
    int16_t x1 = area->x1, x2 = area->x2;
    int16_t y1 = area->y1, y2 = area->y2;
    int16_t width = x2 - x1 + 1;
    int16_t remaining = y2 - y1 + 1;
    int16_t y = y1;

    while (remaining > 0) {
        int16_t chunk = remaining > FLUSH_CHUNK ? FLUSH_CHUNK : remaining;

        // 1) Window: columns = [x1..x2], rows = [y..y+chunk−1]
        spi_master_write_command(&tft, 0x2A);
        spi_master_write_addr(&tft, x1 + tft._offsetx, x2 + tft._offsetx);
        spi_master_write_command(&tft, 0x2B);
        spi_master_write_addr(&tft, y + tft._offsety, (y + chunk - 1) + tft._offsety);

        // 2) Push chunk-worth of pixels
        spi_master_write_command(&tft, 0x2C);
        spi_master_write_colors(&tft, color_ptr, (uint32_t)width * chunk);

        // Advance
        color_ptr += (uint32_t)width * chunk;
        y += chunk;
        remaining -= chunk;
    }

    // 3) Tell LVGL we’re done
    lv_disp_flush_ready(d);
}

static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

void lcd_init_driver(void)
{
    // Panel power-up delay (50 ms)
    vTaskDelay(pdMS_TO_TICKS(50));

    // SPI bus + device init
    spi_master_init(&tft,
                    SPI_MOSI_PIN, SPI_SCLK_PIN,
                    ST7789_CS_PIN, ST7789_DC_PIN,
                    ST7789_RST_PIN, ST7789_LEDK_PIN);
    spi_clock_speed(40 * 1000 * 1000);  // 40 MHz

    // ST7789 panel init (nopnop2002 driver)
    lcdInit(&tft, HOR_RES, VER_RES, 0, 0);

    // Hardware 270° rotation (90° CCW)
    spi_master_write_command(&tft, 0x36); // MADCTL
    spi_master_write_data_byte(&tft, 0xA0); // MY=1, MV=1: 0xA0 for 270deg, 0xC0 for 180deg, 0x60 for 90deg

    // Restore portrait offsets
    tft._offsetx = 40;
    tft._offsety = 52; // 52 if 270 
}

void lcd_lvgl_init(void)
{
    // LVGL library init
    lv_init();

    // Draw‐buffer: HOR_RES × DRAW_LINES lines
    // Allocate space for 20 lines of 240 px each (≈9.6 kB), DMA-capable in DRAM
    static DRAM_ATTR lv_color_t buf[HOR_RES * DRAW_LINES * 2]
        __attribute__((aligned(4)));
    static lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf,
                     HOR_RES,      // 240 px logical width
                     DRAW_LINES,   // 20 lines
                     LV_COLOR_FORMAT_NATIVE,
                     0, buf, sizeof(buf));
                     

    // Create the “display” object
    disp = lv_display_create(HOR_RES, VER_RES);  // 240×135 logical
    lv_display_set_flush_cb(disp, st7789_flush_cb);
    lv_display_set_draw_buffers(disp, &draw_buf, NULL);

    // 1 ms tick timer feeding lv_tick_inc()
    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_cb,
        .name     = "lv_tick"
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);
}

void lcd_format_label(lv_obj_t *label, const char *text, lv_color_t color,
					  const lv_font_t *font, lv_align_t alignment,
					  lv_coord_t x_offset, lv_coord_t y_offset)
{
	lv_label_set_text(label, text);
	lv_obj_set_style_text_color(label, color, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_align(label, alignment, x_offset, y_offset);
}


void lcd_scroll_up(lv_obj_t *lbl_top, lv_obj_t *lbl_mid, lv_obj_t *lbl_bot, const char *new_bot_text)
{    
    lv_label_set_text(lbl_top, lv_label_get_text(lbl_mid));
    lv_label_set_text(lbl_mid, lv_label_get_text(lbl_bot));
    lv_label_set_text(lbl_bot, new_bot_text);
    
}

void lcd_scroll_down(lv_obj_t *lbl_top, lv_obj_t *lbl_mid, lv_obj_t *lbl_bot, const char *new_top_text)
{
    lv_label_set_text(lbl_bot, lv_label_get_text(lbl_mid));
    lv_label_set_text(lbl_mid, lv_label_get_text(lbl_top));
    lv_label_set_text(lbl_top, new_top_text);
}

void lcd_format_center_button(lv_obj_t *btn_mid, lv_color_t user_primary_color, lv_color_t user_secondary_color)
{
	lv_obj_set_size(btn_mid, 175, 45);
	lv_obj_align(btn_mid, LV_ALIGN_CENTER, 0, 0);
	
	lv_color_t darker_user_primary_color = lv_color_darken(user_primary_color, 40); // % darker 
	lv_color_t darker_user_secondary_color = lv_color_darken(user_secondary_color, 20);
	static lv_style_t lbl_mid_style;
	lv_style_init(&lbl_mid_style);
	lv_style_set_radius(&lbl_mid_style, 8); // rounded corners
	lv_style_set_bg_color(&lbl_mid_style, darker_user_primary_color);
	lv_style_set_bg_grad_color(&lbl_mid_style, user_primary_color);
	lv_style_set_bg_grad_dir(&lbl_mid_style, LV_GRAD_DIR_VER);
	lv_style_set_border_width(&lbl_mid_style, 2);
	lv_style_set_border_color(&lbl_mid_style, darker_user_secondary_color);
	lv_style_set_shadow_spread(&lbl_mid_style, 3);
	lv_style_set_shadow_width(&lbl_mid_style, 6);
	lv_style_set_shadow_offset_x(&lbl_mid_style, 3);
	lv_style_set_shadow_offset_y(&lbl_mid_style, 3);
	lv_style_set_shadow_color(&lbl_mid_style, lv_color_hex(0x000000));
	lv_obj_add_style(btn_mid, &lbl_mid_style, 0);
}

static void scroll_ready_cb(lv_anim_t * a)
{
	// Able to start new animation
	already_scrolling = false;
	
	// Adjust labels for scroll up or down
    scroll_ctx_t * ctx = (scroll_ctx_t *)a->user_data;
    if (ctx->up) {
        lcd_scroll_up(ctx->top, ctx->mid, ctx->bot, ctx->txt);
        lv_obj_align(ctx->bot, LV_ALIGN_BOTTOM_MID, 0, -15);
    }
    else {
        lcd_scroll_down(ctx->top, ctx->mid, ctx->bot, ctx->txt);
        lv_obj_align(ctx->top, LV_ALIGN_TOP_MID, 0, 15);
    }
    
    // Delete when done
    lv_anim_del(ctx->bot, (lv_anim_exec_xcb_t)lv_obj_set_y);
	lv_anim_del(ctx->top, (lv_anim_exec_xcb_t)lv_obj_set_y);
    free(ctx);
}

void lcd_scroll_anim(ui_menu_t *menu, const char *txt, bool scrolling_up, uint32_t speed_px_s)
{
	// If already in animation, don't make a new one
	if (already_scrolling) 
		return;
    already_scrolling = true;
    
	// Decide start/end Y
	// Bottom element
    const lv_coord_t start_b = scrolling_up ? -15 : -25;
    const lv_coord_t end_b = scrolling_up ? -25 : -15;
    // Top element
    const lv_coord_t start_t = scrolling_up ? 25 : 15;
    const lv_coord_t end_t = scrolling_up ? 15 : 35;


    // Compute how long the move should take (ms)
    const uint32_t dist = LV_ABS(end_b - start_b);
    const uint32_t dur  = (dist * 1000U) / speed_px_s;


    // Allocate and populate callback context
    scroll_ctx_t *ctx = malloc(sizeof(*ctx));
    *ctx = (scroll_ctx_t){
      .top = menu->lbl_top,
      .mid = menu->lbl_mid,
      .bot = menu->lbl_bot,
      .txt = txt,
      .up  = scrolling_up
    };


    // Build the LVGL animation object
    // Bottom animation
	lv_anim_t a1;
	lv_anim_init(&a1);
	lv_anim_set_var(&a1, menu->lbl_bot);
	if (!scrolling_up)
		lv_label_set_text(menu->lbl_bot, lv_label_get_text(menu->lbl_mid));
	lv_anim_set_exec_cb(&a1, (lv_anim_exec_xcb_t)lv_obj_set_y);
	lv_anim_set_path_cb(&a1, lv_anim_path_linear);
	lv_anim_set_values(&a1, start_b, end_b);
	lv_anim_set_time(&a1, dur);
	
	// Top animation
	lv_anim_t a2;
	lv_anim_init(&a2);
	lv_anim_set_var(&a2, menu->lbl_top);
	if (scrolling_up)
		lv_label_set_text(menu->lbl_top, lv_label_get_text(menu->lbl_mid));
	lv_anim_set_exec_cb(&a2, (lv_anim_exec_xcb_t)lv_obj_set_y);
	lv_anim_set_path_cb(&a2, lv_anim_path_linear);
	lv_anim_set_values(&a2, start_t, end_t);
	lv_anim_set_time(&a2, dur);


	// Hook up the ready callback
    lv_anim_set_ready_cb(&a1,  scroll_ready_cb);
    lv_anim_set_user_data(&a1, ctx);
 
    lv_anim_set_user_data(&a2, ctx);


    // Enqueue it
    lv_anim_start(&a1);
    lv_anim_start(&a2);
}

static void infrared_swipe_done_cb(lv_timer_t * t) {
	ui_menu_t *menu = (ui_menu_t *)lv_timer_get_user_data(t);
	
    lv_timer_del(t); // Only once
    
    // Update IR
    lcd_ir_update_menu(&ir_menu);
    menu->page = INFRARED_PAGE;
}

void lcd_selection_btn_pressed(ui_menu_t *menu)
{
    const char *option = lv_label_get_text(menu->lbl_mid);

	if (strcmp(option, "Infrared") == 0) {
		xSemaphoreGive(xEnableInfraredSemaphore);
		lcd_swipe_anim(menu, 1, SWIPE_SPEED);
		
		// Non-blocking delay
		lv_timer_t * timer = lv_timer_create(infrared_swipe_done_cb, 250, menu);
		lv_timer_set_repeat_count(timer, 1);  // One-shot timer
	}
	else if (strcmp(option, "Bluetooth") == 0) {
		lcd_swipe_anim(menu, 1, SWIPE_SPEED);
		menu->page = BLUETOOTH_PAGE;
	}
	else if (strcmp(option, "LoRa") == 0) {
		lcd_swipe_anim(menu, 1, SWIPE_SPEED);
		menu->page = LORA_PAGE;
	}
	else if (strcmp(option, "ESP-NOW") == 0) {
		lcd_swipe_anim(menu, 1, SWIPE_SPEED);
		menu->page = ESPNOW_PAGE;
	}
	else if (strcmp(option, "Tools") == 0) {
		lcd_swipe_anim(menu, 1, SWIPE_SPEED);
		menu->page = SETTINGS_PAGE;
	}
	else if (strcmp(option, "Settings") == 0) {
		lcd_swipe_anim(menu, 1, SWIPE_SPEED);
		menu->page = TOOLS_PAGE;
	}
	else if (strcmp(option, "Wi-Fi") == 0) {
		lcd_swipe_anim(menu, 1, SWIPE_SPEED);
		menu->page = WIFI_PAGE;
	}
	else {
		ESP_LOGW(TAG, "Invalid menu option selected");
	}
}

static void swipe_ready_cb(lv_anim_t * a)
{
	scroll_ctx_t * ctx = (scroll_ctx_t *)a->user_data;
    lv_obj_add_flag(ctx->bot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ctx->mid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ctx->top, LV_OBJ_FLAG_HIDDEN);
    
    free(ctx);
}

void lcd_swipe_anim(ui_menu_t *menu, bool swipe_left, uint32_t speed_px_s)
{
	lv_obj_remove_flag(ACTIVE_SCR, LV_OBJ_FLAG_SCROLLABLE); // Disable scroll-bar
	
	// Get center button
    lv_obj_t * btn_mid = lv_obj_get_parent(menu->lbl_mid);

    // Animation objects
    lv_obj_t * objs[3] = {
      menu->lbl_top,
      btn_mid,
      menu->lbl_bot
    };

    // Compute start/end
    lv_coord_t start_x = 0;
    lv_coord_t end_x = swipe_left ? -240 : 240;
    uint32_t dist = LV_ABS(end_x - start_x);
    uint32_t dur = (dist * 1000U) / speed_px_s;

    // For callback
    scroll_ctx_t *ctx = malloc(sizeof(*ctx));
    *ctx = (scroll_ctx_t){ .top = menu->lbl_top,
                           .mid = btn_mid,
                           .bot = menu->lbl_bot };

	// Loop through objects and animate each
    for (int i = 0; i < 3; i++) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objs[i]);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
        lv_anim_set_values(&a, start_x, end_x);
        lv_anim_set_time(&a, dur);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        if (i == 2) { // Only needed for one
            lv_anim_set_ready_cb(&a, swipe_ready_cb);
            lv_anim_set_user_data(&a, ctx);
        } else {
            lv_anim_set_user_data(&a, ctx);
        }
        lv_anim_start(&a);
    }
}

static void unhide_selection_widgets(ui_menu_t *m)
{
    // Show center button and it's label
    lv_obj_t *btn_mid = lv_obj_get_parent(m->lbl_mid);
    lv_obj_remove_flag(btn_mid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(m->lbl_mid, LV_OBJ_FLAG_HIDDEN);

    // Show top and bottom labels
    lv_obj_remove_flag(m->lbl_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(m->lbl_bot, LV_OBJ_FLAG_HIDDEN);

    // Ensure arrows visible
    lv_obj_remove_flag(m->arrow_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(m->arrow_bot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(m->arrow_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(m->arrow_right, LV_OBJ_FLAG_HIDDEN);

    // Reset X-coord so they are back on-screen
    lv_obj_set_x(m->lbl_top, 0);
    lv_obj_set_x(btn_mid, 0);
    lv_obj_set_x(m->lbl_bot, 0);
}

void lcd_selection_page_selected(ui_menu_t *menu, ui_btns_t *ui_btns) 
{
	if (ui_btns->up_btn == 1) {
		scrolling_menu = true;
		scrolling_up = false;
	}
	else if (ui_btns->down_btn == 1) {
		scrolling_menu = true;
		scrolling_up = true;
	}
	else if (ui_btns->right_btn == 1) {
		lcd_selection_btn_pressed(menu);
	}
	else if (ui_btns->left_btn == 1) {
		menu->page = 0;
	}

	if (scrolling_menu) {
		if (scrolling_up) {
			menu->index = (menu->index + 1) % menu->size;
			const char *next_bottom = menu->options[(menu->index + 1) % menu->size];
			lcd_scroll_anim(menu, next_bottom, scrolling_up, SCROLL_SPEED);
		}
		else {
			menu->index = (menu->index + menu->size - 1) % menu->size;
			const char *next_top = menu->options[(menu->index + menu->size - 1) % menu->size];
			lcd_scroll_anim(menu, next_top, scrolling_up, SCROLL_SPEED);
		}
		scrolling_menu = false;
	}
}

void lcd_infrared_page_selected(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns) 
{
	// Show IR list
	lv_obj_remove_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
	
	// New remote selected
	if (ui_btns->up_btn == 1 && ir_menu->index == 0) {
				
		lcd_ir_save_new_signal(ui_menu, ir_menu);
		
	}
	// Edit remote selected
	else if (ui_btns->up_btn == 1 && ir_menu->index == 1) {
		
		lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN); // Hide IR menu
		
		ui_menu->page = INFRARED_REMOTE_EDIT_PAGE;
		
	}
	// Selected specific remote
	else if (ui_btns->up_btn == 1) {
		
		xQueueSend(xSignalToTXQueue, &ir_menu->index, 0);
	}
	// Back selected
	else if (ui_btns->down_btn == 1) {
		
		// Hide IR menu
		lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		unhide_selection_widgets(ui_menu);
		
		ui_menu->page = SELECTION_PAGE;


	}
	// Down button pressed
	else if (ui_btns->right_btn == 1) {
		// Update selection
		ir_menu->index++;
		lcd_ir_update_menu(ir_menu);
	}
	// Up button pressed
	else if (ui_btns->left_btn == 1) {
		// Update selection
		ir_menu->index--;
		lcd_ir_update_menu(ir_menu);
	}
}