#include "polycast5_macros.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "portmacro.h"

#include "misc/lv_timer.h"

#include "nvs.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "st7789.h"
#include "tca9535.h"

#include "lcd_utils.h"
#include "wifi_funcs.h"
#include "wifi_task.h"
#include "infrared_funcs.h"
#include "gpio_funcs.h"
#include "gpio_task.h"

#include "anim_city.h"
#include "anim_black_hole.h"

#define DRAW_LINES   20
#define FLUSH_CHUNK  2

#define SWIPE_SPEED 1200
#define SCROLL_SPEED 400
#define IR_LABELS_OFFSET 20

#define LCD_ANIM_NS "lc_an_ns"
#define LCD_ANIM_KEY "lc_an_ke"


/* Animation macros */
#define CITY_FRAME_PERIOD 120 // 160
#define BLACK_HOLE_FRAME_PERIOD 120

#ifdef POLYCAST5_BUILD_FULL_ANIMS
	#define CITY_FRAME_CNT 60
	#define BLACK_HOLE_FRAME_CNT 18
#else
	#define CITY_FRAME_CNT 5
	#define BLACK_HOLE_FRAME_CNT 5
#endif

// Cycle order
#define CITY 0
#define BLACK_HOLE 1

static uint8_t anim_active = 0; // Default determined in lcd_anim_nvs_load

/* LCD */
static const char *TAG = "LCD_FUNCS";

static TFT_t tft;
static lv_display_t *disp; // LVGL display handle

static bool already_scrolling = false;
static bool scrolling_menu = false;
static bool scrolling_up = false;	

extern wifi_login_t selected_network;
extern bool monitoring_packets;

typedef struct {
    lv_obj_t *top;    // the label that sits at the top line
    lv_obj_t *mid;    // the label in the center
    lv_obj_t *bot;    // the label at the bottom (this one moves)
    const char *txt;  // the next string to show
    bool up;          // direction: true=you’re scrolling up, false=scrolling down
} scroll_ctx_t;


/* Animation */
typedef struct {
    lv_obj_t *img; // Single lv_img
    const lv_img_dsc_t **frames; // Pointer to the file‐scope array
    uint8_t frame_cnt; // Num frames
    bool pingpong; // False = wrap
    bool forward; // Current direction in pingpong
    uint8_t cur; // Current frame index
    lv_timer_t *timer; // LVGL timer
} anim_t;

// Define animation frames
#ifdef POLYCAST5_BUILD_FULL_ANIMS
	static const lv_img_dsc_t *city_frames[CITY_FRAME_CNT] = { // 64.84KB each
		// 64.84KB each
		&anim_city_1,  &anim_city_2,  &anim_city_3,	 &anim_city_4,	&anim_city_5,
		&anim_city_6,  &anim_city_7,  &anim_city_8,	 &anim_city_9,	&anim_city_10,
		&anim_city_11, &anim_city_12, &anim_city_13, &anim_city_14, &anim_city_15,
		&anim_city_16, &anim_city_17, &anim_city_18, &anim_city_19, &anim_city_20,
		&anim_city_21, &anim_city_22, &anim_city_23, &anim_city_24, &anim_city_25,
		&anim_city_26, &anim_city_27, &anim_city_28, &anim_city_29, &anim_city_30,
		&anim_city_31, &anim_city_32, &anim_city_33, &anim_city_34, &anim_city_35,
		&anim_city_36, &anim_city_37, &anim_city_38, &anim_city_39, &anim_city_40,
		&anim_city_41, &anim_city_42, &anim_city_43, &anim_city_44, &anim_city_45,
		&anim_city_46, &anim_city_47, &anim_city_48, &anim_city_49, &anim_city_50,
		&anim_city_51, &anim_city_52, &anim_city_53, &anim_city_54, &anim_city_55,
		&anim_city_56, &anim_city_57, &anim_city_58, &anim_city_59, &anim_city_60,
	};
	
	static const lv_img_dsc_t *black_hole_frames[BLACK_HOLE_FRAME_CNT] = {
			&anim_black_hole_1,	 &anim_black_hole_2,  &anim_black_hole_3,
			&anim_black_hole_4,	 &anim_black_hole_5,  &anim_black_hole_6,
			&anim_black_hole_7,	 &anim_black_hole_8,  &anim_black_hole_9,
			&anim_black_hole_10, &anim_black_hole_11, &anim_black_hole_12,
			&anim_black_hole_13, &anim_black_hole_14, &anim_black_hole_15,
			&anim_black_hole_16, &anim_black_hole_17, &anim_black_hole_18
	};
#else
	static const lv_img_dsc_t *city_frames[CITY_FRAME_CNT] = {
			&anim_city_1, &anim_city_2, &anim_city_3,
			&anim_city_4, &anim_city_5
	};
	
	static const lv_img_dsc_t *black_hole_frames[BLACK_HOLE_FRAME_CNT] = {
			&anim_black_hole_1,	 &anim_black_hole_2,  &anim_black_hole_3,
			&anim_black_hole_4,	 &anim_black_hole_5
	};
#endif

// Animation structs
static anim_t city_anim = {
    .frames = city_frames,
    .frame_cnt = CITY_FRAME_CNT,
    .pingpong = false,
    .forward = true,
    .cur = 0,
    .img = NULL,
    .timer = NULL
};

static anim_t black_hole_anim = {
    .frames = black_hole_frames,
    .frame_cnt = BLACK_HOLE_FRAME_CNT,
    .pingpong = false,
    .forward = true,
    .cur = 0,
    .img = NULL,
    .timer = NULL
};
	

static void st7789_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
	xSemaphoreTake(xSPIBusMutex, portMAX_DELAY); // Lock SPI bus
	
    uint16_t *color_ptr = (uint16_t *)px_map; // const const
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
    
    xSemaphoreGive(xSPIBusMutex); // Release SPI bus
}

static void lcd_panel_sleep(void)
{
	xSemaphoreTake(xSPIBusMutex, portMAX_DELAY); // Lock SPI bus
	
    // Display off, sleep in
    spi_master_write_command(&tft,0x28); // DISPOFF
    vTaskDelay(pdMS_TO_TICKS(10));
    spi_master_write_command(&tft,0x10); // SLPIN
    
    xSemaphoreGive(xSPIBusMutex); // Release SPI bus
}

static void lcd_panel_wake(void)
{
	xSemaphoreTake(xSPIBusMutex, portMAX_DELAY); // Lock SPI bus
	
    spi_master_write_command(&tft,0x11); // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120)); 
 
    // Pixel format back to 16-bit 565
    spi_master_write_command(&tft, 0x3A); // COLMOD
    spi_master_write_data_byte(&tft, 0x55); // 0x55 = 16-bit

    // Hardware rotation
    spi_master_write_command(&tft, 0x36); // MADCTL
    spi_master_write_data_byte(&tft, 0x60); // MY=1, MV=1: 0xA0 for 270deg, 0xC0 for 180deg, 0x60 for 90deg
    
    spi_master_write_command(&tft, 0x21); // INVON  ()
    
    spi_master_write_command(&tft,0x29); // DISPON
        
    xSemaphoreGive(xSPIBusMutex); // Release SPI bus
}

void lcd_device_sleep(void)
{
	xQueueReset(xWifiCanSleepSemaphore);
	xSemaphoreGive(xWifiDisconnectSemaphore); // Disconnect from Wi-Fi if connected
	
	lcd_panel_sleep(); // Put ST7789 to sleep
	gpio_set_level(ST7789_LEDA_PIN, 1); // BL low
	
	// Don't auto wake
	while (gpio_read_input(USER_BUTTON_POWER) != 1) {
		vTaskDelay(pdMS_TO_TICKS(25));
		lv_timer_handler();
	}
	
	// Wait for Wi-Fi to shut off if on
	xSemaphoreTake(xWifiCanSleepSemaphore, pdMS_TO_TICKS(1000));

	xSemaphoreTake(xSPIBusMutex, portMAX_DELAY); // Lock SPI bus
	xSemaphoreTake(xI2CBusMutex, portMAX_DELAY); // Lock I2C bus

	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Entering light sleep");
	#endif

	ESP_ERROR_CHECK(esp_light_sleep_start());

	xSemaphoreGive(xSPIBusMutex); // Release SPI bus
	xSemaphoreGive(xI2CBusMutex); // Release I2C bus

	lcd_panel_wake(); // Wake up ST7789
	gpio_set_level(ST7789_LEDA_PIN, 0); // BL high
	
	xSemaphoreGive(xStartAdcBatSemaphore); // Start new battery ADC reading
	
	// Don't auto sleep
	while (gpio_read_input(USER_BUTTON_POWER) != 1) {
		vTaskDelay(pdMS_TO_TICKS(25));
		lv_timer_handler();
	}

	xQueueReset(xPowerButtonSemaphore); // Clear xPowerButtonSemaphore	
	
	go_to_sleep = false; // Clear sleep flag
	lcd_clear_pending_inputs = true; // Clear if action button pressed to wake
}

void lcd_init_driver(void)
{
    // Panel power-up delay (50 ms)
    vTaskDelay(pdMS_TO_TICKS(50));

    // SPI bus + device init
    spi_master_init(&tft, SPI_MOSI_PIN, SPI_SCLK_PIN, ST7789_CS_PIN, ST7789_DC_PIN, ST7789_RST_PIN, ST7789_LEDA_PIN);
    spi_clock_speed(40 * 1000 * 1000);  // 40 MHz

    // ST7789 panel init
    lcdInit(&tft, HOR_RES, VER_RES, 0, 0);

    // Hardware rotation
    spi_master_write_command(&tft, 0x36); // MADCTL
    spi_master_write_data_byte(&tft, 0x60); // MY=1, MV=1: 0xA0 for 270deg, 0xC0 for 180deg, 0x60 for 90deg

    // Restore portrait offsets
    tft._offsetx = 40;
    tft._offsety = 53; // 52 if 270 | 53 if 90
}

static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
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
    lv_draw_buf_init(&draw_buf, HOR_RES, DRAW_LINES, LV_COLOR_FORMAT_NATIVE, 0, buf, sizeof(buf));
                     
    // Create the “display” object
    disp = lv_display_create(HOR_RES, VER_RES); // 240x135 logical
    lv_display_set_flush_cb(disp, st7789_flush_cb);
    lv_display_set_draw_buffers(disp, &draw_buf, NULL);

    // 1 ms tick timer feeding lv_tick_inc()
    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_cb,
        .name     = "lv_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);
}

void lcd_ns_nvs_clear(const char* ns)
{
    nvs_handle_t h;
    
    // Clear all NVS
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); // Wipes only keys in this namespace
        nvs_commit(h);
        nvs_close(h);
    }
}

void lcd_format_label(lv_obj_t *label, const char *text, lv_color_t color, const lv_font_t *font, lv_align_t alignment, lv_coord_t x_offset, lv_coord_t y_offset)
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

/* Write the current anim_active into flash */
static esp_err_t lcd_anim_nvs_save(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(LCD_ANIM_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    // Store anim_active as a single byte
    err = nvs_set_u8(h, LCD_ANIM_KEY, anim_active);
    if (err == ESP_OK) {
        // Commit to flash
        err = nvs_commit(h);
        
        #ifdef POLYCAST5_DEBUG
    		ESP_LOGI(TAG, "Saved NVS animation: %u", anim_active);
		#endif
    }
    else {
		#ifdef POLYCAST5_DEBUG
    		ESP_LOGI(TAG, "Failed to save NVS animation");
		#endif
	}
	
	// Close NVS
    nvs_close(h);
    return err;
}

/* Load the current anim_active from flash */
static esp_err_t lcd_anim_nvs_load(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(LCD_ANIM_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
	
	// Get the uint8
    uint8_t stored = 0;
    err = nvs_get_u8(h, LCD_ANIM_KEY, &stored);
    switch (err) {
        case ESP_OK:
            anim_active = stored;
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            // First‐boot or key erased -> default
            anim_active = CITY;
            err = ESP_OK;
            break;
        default:
            break;
    }
    
    #ifdef POLYCAST5_DEBUG
    	ESP_LOGI(TAG, "Loaded NVS animation: %u", anim_active);
	#endif
	
	// Close NVS
    nvs_close(h);
    return err;
}

static void anim_timer_cb(lv_timer_t *t)
{
    anim_t *anim = (anim_t *)lv_timer_get_user_data(t);
    uint8_t current = anim->cur;

    if (anim->pingpong) { // If ping ponging
        if (anim->forward) { // Going forward
            if (current + 1 < anim->frame_cnt) {
				current++; // Iterate frame
			}
            else { // When reached end
				anim->forward = false; // Switch dir
				current--; // Decrement frame
			}
        }
        else { // Going back
            if(current > 0) {
				current--; // Decrement frame
			} 
            else { // When reached start
				anim->forward = true; // Switch dir
				current++;
			}
        }
    }
    else { // Wrapping
        current = (current + 1) % anim->frame_cnt; // Iterate with wrap
    }
	
	// Set frame
    anim->cur = current;
    lv_img_set_src(anim->img, anim->frames[current]);
}

void lcd_init_images()
{
	// Load selected from NVS
	lcd_anim_nvs_load();
	
	/* City */
	// Create image
    city_anim.img = lv_img_create(ACTIVE_SCR);
    lv_img_set_src(city_anim.img, city_anim.frames[0]);
    lv_obj_center(city_anim.img);
    
    // Create timer
    city_anim.timer = lv_timer_create(anim_timer_cb, CITY_FRAME_PERIOD, &city_anim);
    
    // Check if set
    if(anim_active != CITY) {
		lv_obj_add_flag(city_anim.img, LV_OBJ_FLAG_HIDDEN);
		lv_timer_pause(city_anim.timer);
	}

    /* Black hole */
    // Create image
    black_hole_anim.img = lv_img_create(ACTIVE_SCR);
    lv_img_set_src(black_hole_anim.img, black_hole_anim.frames[0]);
    lv_obj_center(black_hole_anim.img);
    
    // Create timer
    black_hole_anim.timer = lv_timer_create(anim_timer_cb, BLACK_HOLE_FRAME_PERIOD, &black_hole_anim);
    
    // Check if set
    if(anim_active != BLACK_HOLE) {
		lv_obj_add_flag(black_hole_anim.img, LV_OBJ_FLAG_HIDDEN);
		lv_timer_pause(black_hole_anim.timer);
	}
}

static void start_animation(void)
{
    // Start the active
    if(anim_active == CITY) {
        lv_obj_remove_flag(city_anim.img, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(city_anim.timer);
    }
    else {
        lv_obj_remove_flag(black_hole_anim.img,  LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(black_hole_anim.timer);
    }
}

static void stop_animations(void)
{
    lv_timer_pause(city_anim.timer);
    lv_timer_pause(black_hole_anim.timer);

    lv_obj_add_flag(city_anim.img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(black_hole_anim.img, LV_OBJ_FLAG_HIDDEN);
}

static void transition_animation(bool dir)
{
	#define NUM_ANIMS 2
	
	stop_animations();
	
	if (dir) {
		anim_active = (anim_active + 1) % NUM_ANIMS; // + 1 with wrap
	}
	else {
		anim_active = (anim_active + NUM_ANIMS - 1) % NUM_ANIMS; // - 1 with wrap
	}
	
    start_animation();
    
    // Save choice to NVS
    lcd_anim_nvs_save();
}

void lcd_home_page_selected(ui_menu_t *ui_menu, ui_btns_t *ui_btns)
{
	
	if (ui_btns->up_btn == 1) {
		transition_animation(true);
	}
	else if (ui_btns->down_btn == 1) {
		transition_animation(false);
	}
	else if (ui_btns->select_btn == 1) {
		stop_animations();
		
		unhide_selection_widgets(ui_menu);
		
		// Show selection labels
		lv_obj_remove_flag(ui_menu->btn_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		ui_menu->page = SELECTION_PAGE;
	}
	else if (ui_btns->left_btn == 1) {
		
	}
	else if (ui_btns->right_btn == 1) {
		
	}
}

void lcd_init_selection_labels(ui_menu_t *ui_menu)
{
	// Create and format center button
    ui_menu->btn_mid = lv_btn_create(ACTIVE_SCR);
    lcd_format_center_button(ui_menu->btn_mid, user_primary_color, user_secondary_color);

	// Format labels
	ui_menu->lbl_top = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu->lbl_top, "Bluetooth", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

	ui_menu->lbl_mid = lv_label_create(ui_menu->btn_mid);
	lcd_format_label(ui_menu->lbl_mid, "PolyPlug",
					 user_secondary_color, &lv_font_montserrat_30,
					 LV_ALIGN_CENTER, 0, 0);
					 
	ui_menu->lbl_bot = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu->lbl_bot, "ESP32", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -15);
	
	// Arrows		 
	ui_menu->arrow_top = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu->arrow_top, LV_SYMBOL_UP, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 0);
					 
	ui_menu->arrow_left = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu->arrow_left, LV_SYMBOL_LEFT, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 4, 0);

	ui_menu->arrow_right = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu->arrow_right, LV_SYMBOL_RIGHT, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_RIGHT_MID, -4, 0);

	ui_menu->arrow_bot = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu->arrow_bot, LV_SYMBOL_DOWN, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, 0);

	// Battery icon
	ui_menu->lbl_battery_txt = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu->lbl_battery_txt, "...", user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -28, 0);

	ui_menu->lbl_battery_icon = lv_label_create(ACTIVE_SCR); // 3, 2, 1, EMPTY
	lcd_format_label(ui_menu->lbl_battery_icon, LV_SYMBOL_BATTERY_FULL, user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_RIGHT, -2, -3);
					 
	// Hide all for now
	lv_obj_add_flag(ui_menu->btn_mid, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
}

void lcd_clear_user_in()
{
	ui_btns.up_btn = 0;
	ui_btns.down_btn = 0;
	ui_btns.left_btn = 0;
	ui_btns.right_btn = 0;
	ui_btns.select_btn = 0;
	ui_btns.home_btn = 0;
	ui_btns.pwr_btn = 0;
}

void lcd_update_battery(ui_menu_t *ui_menu, uint8_t battery_percentage)
{
	char buf[4]; // 3 + 1 max
	snprintf(buf, sizeof(buf), "%u", battery_percentage);
	lv_label_set_text(ui_menu->lbl_battery_txt, buf);
	
	// Update icon based on battery level
	if (battery_percentage >= 80) { // 80-100
		lv_label_set_text(ui_menu->lbl_battery_icon, LV_SYMBOL_BATTERY_FULL);
	}
	else if (battery_percentage >= 60) { // 60-79
		lv_label_set_text(ui_menu->lbl_battery_icon, LV_SYMBOL_BATTERY_3);
	}
	else if (battery_percentage >= 40) { // 40-59
		lv_label_set_text(ui_menu->lbl_battery_icon, LV_SYMBOL_BATTERY_2);
	}
	else if (battery_percentage >= 20) { // 20-39
		lv_label_set_text(ui_menu->lbl_battery_icon, LV_SYMBOL_BATTERY_1);
	}
	else { // 0-19
		lv_label_set_text(ui_menu->lbl_battery_icon, LV_SYMBOL_BATTERY_EMPTY);
	}
}

static void lcd_selection_btn_pressed(ui_menu_t *ui_menu, ir_menu_t* ir_menu, lora_menu_t* lora_menu, espnow_menu_t* espnow_menu, wifi_menu_t* wifi_menu, tools_menu_t* tools_menu, settings_menu_t* settings_menu)
{
	// Hide selection labels
	lv_obj_add_flag(ui_menu->btn_mid, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);
		
    const char *option = lv_label_get_text(ui_menu->lbl_mid);
    
   	if (strcmp(option, "Infrared") == 0) {	
		// Show IR list
		lv_obj_remove_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);	
		ui_menu->page = INFRARED_PAGE;
	}
	else if (strcmp(option, "Bluetooth") == 0) {
		ui_menu->page = BLUETOOTH_PAGE;
	}
	else if (strcmp(option, "PolyPlug") == 0) {
		// Show LoRa list
		lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);	
		ui_menu->page = LORA_PAGE;
	}
	else if (strcmp(option, "ESP32") == 0) {
		// Show ESP-NOW list
		lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = ESPNOW_PAGE;
	}
	else if (strcmp(option, "Tools") == 0) {
		// Show tools list
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = TOOLS_PAGE;
	}
	else if (strcmp(option, "Settings") == 0) {
		// Show settings list
		lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = SETTINGS_PAGE;
	}
	else if (strcmp(option, "Wi-Fi") == 0) {
		// Show Wi-Fi list
		lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = WIFI_PAGE;
	}
	else {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Invalid menu option selected");
		#endif
	}
}

void lcd_selection_page_selected(ui_menu_t *ui_menu, ui_btns_t *ui_btns, ir_menu_t* ir_menu, lora_menu_t* lora_menu, espnow_menu_t* espnow_menu, wifi_menu_t* wifi_menu, tools_menu_t* tools_menu, settings_menu_t* settings_menu) 
{
	if (ui_btns->up_btn == 1) {
		scrolling_menu = true;
		scrolling_up = false;
	}
	else if (ui_btns->down_btn == 1) {
		scrolling_menu = true;
		scrolling_up = true;
	}
	else if (ui_btns->select_btn == 1) {
		lcd_selection_btn_pressed(ui_menu, ir_menu, lora_menu, espnow_menu, wifi_menu, tools_menu, settings_menu);
	}
	// Go back
	else if (ui_btns->left_btn == 1) {
		// Hide selection labels
		lv_obj_add_flag(ui_menu->btn_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
				
		start_animation();

		ui_menu->page = HOME_PAGE;
	}
	// Go home
	else if (ui_btns->home_btn == 1) {
		// Hide selection labels
		lv_obj_add_flag(ui_menu->btn_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
				
		lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
	}
	// Power off
	else if (ui_btns->pwr_btn == 1) {
		// Hide selection labels
		lv_obj_add_flag(ui_menu->btn_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
	}

	if (scrolling_menu) {
		if (scrolling_up) {
			ui_menu->index = (ui_menu->index + 1) % ui_menu->size;
			const char *next_bottom = ui_menu->options[(ui_menu->index + 1) % ui_menu->size];
			lcd_scroll_anim(ui_menu, next_bottom, scrolling_up, SCROLL_SPEED);
		}
		else {
			ui_menu->index = (ui_menu->index + ui_menu->size - 1) % ui_menu->size;
			const char *next_top = ui_menu->options[(ui_menu->index + ui_menu->size - 1) % ui_menu->size];
			lcd_scroll_anim(ui_menu, next_top, scrolling_up, SCROLL_SPEED);
		}
		scrolling_menu = false;
	}
}

void lcd_funcs_transition_back(bool home, ui_menu_t *ui_menu)
{
	// Hide arrows
	lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
	
	if (home) { // Transition to home
		start_animation();

		ui_menu->page = HOME_PAGE;
	}
	else { // Transition to sleep
		gpio_set_level(ST7789_LEDA_PIN, 0); // BL low so user doesn't see redraw
	
		start_animation();

		ui_menu->page = HOME_PAGE;
			
		lv_timer_handler();
		go_to_sleep = true;
	}
}

void lcd_infrared_page_selected(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns) 
{	
	static bool initalized = false;
	
	if (!initalized) {
		// Show IR list
		lv_obj_remove_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);	
		
		initalized = true;
	}
	// New remote selected
	if (ui_btns->select_btn == 1 && ir_menu->index == 1) {
		lcd_ir_save_new_signal(ui_menu, ir_menu);
		
		initalized = false;
	}
	// Edit remote selected
	else if (ui_btns->select_btn == 1 && ir_menu->index == 2) {
		lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN); // Hide IR menu
		
		initalized = false; // Reset bool
		
		ui_menu->page = INFRARED_REMOTE_EDIT_PAGE;
	}
	// Selected specific remote
	else if (ui_btns->select_btn == 1 && ir_menu->index != 0) {
		xQueueSend(xInfraredSignalToTxQueue, &ir_menu->index, 0);
	}
	// Back selected
	else if (ui_btns->down_btn == 1) {
		// Hide IR menu
		lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		initalized = false; // Reset bool
		
		// Show selection labels
		unhide_selection_widgets(ui_menu);
		
		ui_menu->page = SELECTION_PAGE;
	}
	// Home selected
	else if (ui_btns->home_btn == 1) {
		// Hide IR menu
		lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		initalized = false; // Reset bool
		
		lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
	}
	// Power off selected
	else if (ui_btns->pwr_btn == 1) {
		// Hide IR menu
		lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		initalized = false; // Reset bool
		
		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
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

void lcd_lora_page_selected(ui_menu_t *ui_menu, lora_menu_t *lora_menu, ui_btns_t *ui_btns) 
{
	// Only execute once
	static bool do_once = false;
	if (!do_once) {
		// Show LoRa list
		lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);	
		
		do_once = true;
	}
	
	// Up button pressed
	if (ui_btns->up_btn == 1) {
		// Update selection
		lora_menu->index--;
		lcd_lora_update_menu(lora_menu);
	}
	// Down button pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		lora_menu->index++;
		lcd_lora_update_menu(lora_menu);
	}
	// Add PolyPlug selected
	else if (ui_btns->select_btn == 1 && lora_menu->index == 0) {
		xSemaphoreGive(xWifiDisconnectSemaphore); // Disconnect from Wi-Fi if connected
		
		// Hide LoRa menu
		lv_obj_add_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		lcd_lora_create_enc_key(ui_menu, lora_menu);
	
	}
	// PolyPlug selected
	else if (ui_btns->select_btn == 1) {
		// Hide LoRa menu
		lv_obj_add_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Show submenu
		lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		ui_menu->page = LORA_SUBPAGE;
	
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide LoRa menu
		lv_obj_add_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		unhide_selection_widgets(ui_menu);
		
		// Reset static
		do_once = false;
		
		ui_menu->page = SELECTION_PAGE;
	}
	else if (ui_btns->home_btn == 1) {
		// Hide LoRa menu
		lv_obj_add_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
	}
	else if (ui_btns->pwr_btn == 1) {
		// Hide LoRa menu
		lv_obj_add_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
	}
}

void lcd_espnow_page_selected(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu, ui_btns_t *ui_btns)
{
	// Statics
	static bool do_once = false;
	
	// Only execute once
	if (!do_once) {
		// Show ESP-NOW list
		lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		do_once = true;
	}
	
	// Up button pressed
	if (ui_btns->up_btn == 1) {
		// Update selection
		espnow_menu->index--;
		lcd_espnow_update_menu(espnow_menu);
	}
	// Down button pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		espnow_menu->index++;
		lcd_espnow_update_menu(espnow_menu);
	}
	// Add ESP32 selected
	else if (ui_btns->select_btn == 1 && espnow_menu->index == 0) {
		// Hide ESP-NOW menu
		lv_obj_add_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		ui_menu->page = ESPNOW_RX_MAC_PAGE;
	}
	// Specific selected
	else if (ui_btns->select_btn == 1) {
		xSemaphoreGive(xWifiDisconnectSemaphore); // Disconnect from Wi-Fi if connected
		
		// Hide ESP-NOW menu
		lv_obj_add_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show ESP-NOW submenu
		lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_send_tx, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_send_rx, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_send_cmd, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_send_box, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_send, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_edit, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(espnow_menu->espnow_submenu.arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(espnow_menu->espnow_submenu.arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Hide up and down arrows
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		ui_menu->page = ESPNOW_OPTION_PAGE;
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide ESP-NOW menu
		lv_obj_add_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		unhide_selection_widgets(ui_menu);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SELECTION_PAGE;
	}
	// Home selected
	else if (ui_btns->home_btn == 1) {
		// Hide ESP-NOW menu
		lv_obj_add_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
	}
	// Power off selected
	else if (ui_btns->pwr_btn == 1) {
		// Hide ESP-NOW menu
		lv_obj_add_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
	}
}

void lcd_wifi_page_selected(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu, ui_btns_t *ui_btns)
{
	// Statics
	static bool do_once, connected = false;
	static lv_obj_t *lbl_conf;
	
	// Only execute once
	if (!do_once) {
		// Show Wi-Fi list
		lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		do_once = true;
	}
	
	// Update label based on connection
	if (xSemaphoreTake(xWifiConnectingSemaphore, 0) == pdTRUE) {
		lv_obj_t *lbl = lv_obj_get_child(wifi_menu->btns[0], 0);
		lv_label_set_text(lbl, "Connecting...");
	}
	if (xSemaphoreTake(xWifiNetworkConnectedSemaphore, 0) == pdTRUE) {
		char buf[44];
		snprintf(buf, sizeof(buf), "Connected: %s", selected_network.ssid);
		
		lv_obj_t *lbl = lv_obj_get_child(wifi_menu->btns[0], 0);
		lv_label_set_text(lbl, buf);
		
		connected = true;
	}
	if (xSemaphoreTake(xWifiNetworkDisconnectedSemaphore, 0) == pdTRUE) {
		lv_obj_t *lbl = lv_obj_get_child(wifi_menu->btns[0], 0);
		lv_label_set_text(lbl, "Connect to network");
		
		connected = false;
	}
	
	// Up button pressed
	if (ui_btns->up_btn == 1) {
		// Update selection
		wifi_menu->index--;
		lcd_wifi_update_menu(wifi_menu);
	}
	// Down button pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		wifi_menu->index++;
		lcd_wifi_update_menu(wifi_menu);
	}
	// Connect to network
	else if (ui_btns->select_btn == 1 && wifi_menu->index == 0) {
		// If connected to a network
		if (connected) {
	        xSemaphoreGive(xWifiDisconnectSemaphore);
	    }
	    // Already disconnected
	    else {
			// Hide Wi-Fi menu
			lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Show scan menu
			lv_obj_remove_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Reset static
			do_once = false;
			
			ui_menu->page = WIFI_SCAN_PAGE;
		}
	}
	// Monitor packets
	else if (ui_btns->select_btn == 1 && wifi_menu->index == 1) {
		// Hide Wi-Fi menu
		lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
		// Show scan menu
		lv_obj_remove_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
			
		// Reset static
		do_once = false;
		
		monitoring_packets = true;
			
		ui_menu->page = WIFI_SCAN_PAGE;
	}
	// Sync with PolyPlug
	else if (ui_btns->select_btn == 1 && wifi_menu->index == 2) {
		// Hide Wi-Fi menu
		lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
				
		if (connected) {
			// Reset static
			do_once = false;
			
			ui_menu->page = WIFI_SYNC_PAGE;
		}
		else {
			lbl_conf = lv_label_create(ACTIVE_SCR);
			
			lcd_format_label(lbl_conf, "Please connect to\n  a network first!", user_secondary_color,
						 &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
			
			lv_timer_handler();
			vTaskDelay(pdMS_TO_TICKS(1000));
			
			lv_obj_del(lbl_conf);
			lbl_conf = NULL;
			
			lcd_clear_pending_inputs = true;
	        
	        // Show Wi-Fi menu
			lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		}
	}
	// Send over Wi-Fi to specific
	else if (ui_btns->select_btn == 1) {
		// Hide Wi-Fi menu
		lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
				
		if (connected) {
			// Reset static
			do_once = false;
			
			// Hide top and bot arrows
			lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			
			// Show wifi send page
			lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send_ins, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send_cmd, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send_box, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_edit, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(wifi_menu->wifi_submenu.arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(wifi_menu->wifi_submenu.arrow_bot, LV_OBJ_FLAG_HIDDEN);
			
			ui_menu->page = WIFI_SEND_PAGE;
		}
		else {
			lbl_conf = lv_label_create(ACTIVE_SCR);
			
			lcd_format_label(lbl_conf, "Please connect to\n  a network first!", user_secondary_color,
						 &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
			
			lv_timer_handler();
			vTaskDelay(pdMS_TO_TICKS(1000));
			
			lv_obj_del(lbl_conf);
			lbl_conf = NULL;
			
			lcd_clear_pending_inputs = true;
	        
	        // Show Wi-Fi menu
			lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		}
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide Wi-Fi menu
		lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		unhide_selection_widgets(ui_menu);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SELECTION_PAGE;
	}
	// Home selected
	else if (ui_btns->home_btn == 1) {
		// Hide Wi-Fi menu
		lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
	}
	// Power off selected
	else if (ui_btns->pwr_btn == 1) {
		// Hide Wi-Fi menu
		lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
	}
}

void lcd_tools_page_selected(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
	// Statics
	static bool do_once = false;
	
	// Only execute once
	if (!do_once) {
		// Show tools list
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		do_once = true;
	}
	
	// Up button pressed
	if (ui_btns->up_btn == 1) {
		// Update selection
		tools_menu->index--;
		lcd_tools_update_menu(tools_menu);
	}
	// Down button pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		tools_menu->index++;
		lcd_tools_update_menu(tools_menu);
	}
	// Coin flipper selected
	else if (ui_btns->select_btn == 1 && tools_menu->index == 0) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Hide arrows
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_COIN_PAGE;
	}
	// Dice roller selected
	else if (ui_btns->select_btn == 1 && tools_menu->index == 1) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = TOOLS_DICE_PAGE;
	}
	// Read the docs selected
	else if (ui_btns->select_btn == 1 && tools_menu->index == 3) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Hide up/down arrow
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_DOCS_PAGE;
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		unhide_selection_widgets(ui_menu);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SELECTION_PAGE;
	}
	// Home selected
	else if (ui_btns->home_btn == 1) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
	}
	// Power off selected
	else if (ui_btns->pwr_btn == 1) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
	}
}

void lcd_settings_page_selected(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
	// Statics
	static bool do_once = false;
	
	// Only execute once
	if (!do_once) {
		// Show settings list
		lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		do_once = true;
	}
	
	// Up button pressed
	if (ui_btns->up_btn == 1) {
		// Update selection
		settings_menu->index--;
		lcd_settings_update_menu(settings_menu);
	}
	// Down button pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		settings_menu->index++;
		lcd_settings_update_menu(settings_menu);
	}
	// Change colors selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 1) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Hide top and bottom arrows
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = SETTINGS_COLORS_PAGE;
	}
	// Reboot selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 4) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Confirmation text
		lv_obj_t *lbl_rst = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_rst, "Rebooting...", user_secondary_color,
				 &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 0);
		lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(100));
		
		// Reboot
		esp_restart();
	}
	// Factory reset selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 5) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Hide arrows
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = SETTINGS_FACTORY_RST_PAGE;
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		unhide_selection_widgets(ui_menu);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SELECTION_PAGE;
	}
	// Home selected
	else if (ui_btns->home_btn == 1) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
	}
	// Power off selected
	else if (ui_btns->pwr_btn == 1) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
	}
}


