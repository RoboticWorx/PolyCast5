#include "polycast5_macros.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/idf_additions.h"
#include "portmacro.h"

#include "nvs.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_err.h"

#include "qrcodegen.h"
#include "st7789.h"
#include "tca9535.h"

#include "core/lv_obj.h"
#include "core/lv_obj_pos.h"
#include "font/lv_symbol_def.h"
#include "widgets/label/lv_label.h"
#include "draw/lv_image_decoder_private.h"
#include "draw/lv_image_decoder.h"
#include "misc/lv_timer.h"

#include "lcd_asset_macros.h"
#include "lcd_bluetooth_funcs.h"
#include "lcd_gpio_funcs.h"
#include "lcd_utils.h"
#include "wifi_funcs.h"
#include "wifi_task.h"
#include "infrared_funcs.h"
#include "infrared_task.h"
#include "gpio_funcs.h"
#include "gpio_task.h"
#include "lora_task.h"
#include "espnow_task.h"
#include "ai_task.h"

#define DRAW_LINES 20
#define FLUSH_CHUNK 2

#define SWIPE_SPEED 1200
#define SCROLL_SPEED 400
#define IR_LABELS_OFFSET 20

#define SELECTION_DEFAULT_IDX 3 // Default starting selection menu index

#define SELECTION_SCROLLBAR_OFFSET -36
#define SELECTION_SCROLLBAR_CONT_HEIGHT 106
#define SELECTION_SCROLLBAR_THUMB_HEIGHT 20

#define LCD_ANIM_NS "anim_data"
#define LCD_ANIM_KEY "selected"

#define SEL_MENU_NS "sel_menu"
#define SEL_MENU_INDEX_KEY "sel_idx"

#define LCD_FIRST_BOOT_NS "first_boot"
#define LCD_FIRST_BOOT_KEY "exists"


/* Hotkey macros */
#define HOTKEY_SHORT_HOME_IDX 0
#define HOTKEY_LONG_HOME_IDX 1
#define HOTKEY_LONG_LEFT_IDX 2
#define HOTKEY_LONG_SELECT_IDX 3
#define HOTKEY_SHORT_RIGHT_IDX 4
#define HOTKEY_LONG_RIGHT_IDX 5


/* Animation macros */
#ifdef POLYCAST5_EN_PYRAMID_ANIM
#define NUM_ANIMS 4
#else
#define NUM_ANIMS 3
#endif

#define CITY_FRAME_PERIOD 120 // 160
#define BLACK_HOLE_FRAME_PERIOD 120
#define MATRIX_RAIN_FRAME_PERIOD 100

#ifdef POLYCAST5_EN_PYRAMID_ANIM
#define PYRAMID_FRAME_PERIOD 120
#endif

#define CITY_FRAME_CNT 60
#define BLACK_HOLE_FRAME_CNT 18
#define MATRIX_RAIN_FRAME_CNT 42

#ifdef POLYCAST5_EN_PYRAMID_ANIM
#define PYRAMID_FRAME_CNT 56
#else
#define PYRAMID_FRAME_CNT 0
#endif

// Number each sequentially
enum
{
	CITY,
	BLACK_HOLE,
	MATRIX_RAIN,
	#ifdef POLYCAST5_EN_PYRAMID_ANIM
	PYRAMID
	#endif
};

extern wifi_login_t selected_network;
extern bool monitoring_packets;

uint32_t pin_attempts = 0;
bool pin_signing_in = false;
bool lcd_wifi_connected = false;

static bool pin_to_selection_page = true; // Flag on if going to selection or hotkey page from pin

static uint8_t anim_active = 0; // Default determined in lcd_anim_nvs_load

/* LCD */
static const char *TAG = "LCD_FUNCS";

static TFT_t tft;
static lv_display_t *disp; // LVGL display handle

static bool already_scrolling = false;
static bool scrolling_menu = false;
static bool scrolling_up = false;

typedef struct {
	lv_obj_t *top; // Label that sits at the top line
	lv_obj_t *mid; // Label in the center
	lv_obj_t *bot; // Label at the bottom (this one moves)
	const char *txt; // The next string to show
	bool up; // Direction: true=you’re scrolling up, false=scrolling down
} scroll_ctx_t;


/* Animation */
typedef struct {
	lv_obj_t *img; // Single lv_img
	const char **frames; // Pointer to file‐path strings
	uint8_t frame_cnt; // Num frames
	bool pingpong; // False = wrap
	bool forward; // Current direction in pingpong
	uint8_t cur; // Current frame index
	lv_timer_t *timer; // LVGL timer
} anim_t;

// Define animation frame paths
const char *city_paths[CITY_FRAME_CNT] = { // 64.84KB each
	ANIM_CITY_1, ANIM_CITY_2, ANIM_CITY_3,	ANIM_CITY_4, ANIM_CITY_5,
	ANIM_CITY_6, ANIM_CITY_7, ANIM_CITY_8,	ANIM_CITY_9, ANIM_CITY_10,
	ANIM_CITY_11, ANIM_CITY_12, ANIM_CITY_13, ANIM_CITY_14, ANIM_CITY_15,
	ANIM_CITY_16, ANIM_CITY_17, ANIM_CITY_18, ANIM_CITY_19, ANIM_CITY_20,
	ANIM_CITY_21, ANIM_CITY_22, ANIM_CITY_23, ANIM_CITY_24, ANIM_CITY_25,
	ANIM_CITY_26, ANIM_CITY_27, ANIM_CITY_28, ANIM_CITY_29, ANIM_CITY_30,
	ANIM_CITY_31, ANIM_CITY_32, ANIM_CITY_33, ANIM_CITY_34, ANIM_CITY_35,
	ANIM_CITY_36, ANIM_CITY_37, ANIM_CITY_38, ANIM_CITY_39, ANIM_CITY_40,
	ANIM_CITY_41, ANIM_CITY_42, ANIM_CITY_43, ANIM_CITY_44, ANIM_CITY_45,
	ANIM_CITY_46, ANIM_CITY_47, ANIM_CITY_48, ANIM_CITY_49, ANIM_CITY_50,
	ANIM_CITY_51, ANIM_CITY_52, ANIM_CITY_53, ANIM_CITY_54, ANIM_CITY_55,
	ANIM_CITY_56, ANIM_CITY_57, ANIM_CITY_58, ANIM_CITY_59, ANIM_CITY_60,
};

const char *black_hole_paths[BLACK_HOLE_FRAME_CNT] = {
	ANIM_BLACK_HOLE_1, ANIM_BLACK_HOLE_2, ANIM_BLACK_HOLE_3, ANIM_BLACK_HOLE_4, ANIM_BLACK_HOLE_5,
	ANIM_BLACK_HOLE_6, ANIM_BLACK_HOLE_7, ANIM_BLACK_HOLE_8, ANIM_BLACK_HOLE_9, ANIM_BLACK_HOLE_10,
	ANIM_BLACK_HOLE_11, ANIM_BLACK_HOLE_12, ANIM_BLACK_HOLE_13, ANIM_BLACK_HOLE_14, ANIM_BLACK_HOLE_15,
	ANIM_BLACK_HOLE_16, ANIM_BLACK_HOLE_17, ANIM_BLACK_HOLE_18
};
	
const char *matrix_rain_paths[MATRIX_RAIN_FRAME_CNT] = {
	ANIM_MATRIX_RAIN_1, ANIM_MATRIX_RAIN_2, ANIM_MATRIX_RAIN_3, ANIM_MATRIX_RAIN_4,	ANIM_MATRIX_RAIN_5,
	ANIM_MATRIX_RAIN_6, ANIM_MATRIX_RAIN_7, ANIM_MATRIX_RAIN_8, ANIM_MATRIX_RAIN_9,	ANIM_MATRIX_RAIN_10,
	ANIM_MATRIX_RAIN_11, ANIM_MATRIX_RAIN_12, ANIM_MATRIX_RAIN_13, ANIM_MATRIX_RAIN_14, ANIM_MATRIX_RAIN_15,
	ANIM_MATRIX_RAIN_16, ANIM_MATRIX_RAIN_17, ANIM_MATRIX_RAIN_18, ANIM_MATRIX_RAIN_19, ANIM_MATRIX_RAIN_20,
	ANIM_MATRIX_RAIN_21, ANIM_MATRIX_RAIN_22, ANIM_MATRIX_RAIN_23, ANIM_MATRIX_RAIN_24, ANIM_MATRIX_RAIN_25,
	ANIM_MATRIX_RAIN_26, ANIM_MATRIX_RAIN_27, ANIM_MATRIX_RAIN_28, ANIM_MATRIX_RAIN_29, ANIM_MATRIX_RAIN_30,
	ANIM_MATRIX_RAIN_31, ANIM_MATRIX_RAIN_32, ANIM_MATRIX_RAIN_33, ANIM_MATRIX_RAIN_34, ANIM_MATRIX_RAIN_35,
	ANIM_MATRIX_RAIN_36, ANIM_MATRIX_RAIN_37, ANIM_MATRIX_RAIN_38, ANIM_MATRIX_RAIN_39, ANIM_MATRIX_RAIN_40,
	ANIM_MATRIX_RAIN_41, ANIM_MATRIX_RAIN_42
};

#ifdef POLYCAST5_EN_PYRAMID_ANIM
const char *pyramid_paths[PYRAMID_FRAME_CNT] = {
	ANIM_PYRAMID_1, ANIM_PYRAMID_2, ANIM_PYRAMID_3, ANIM_PYRAMID_4, ANIM_PYRAMID_5,
	ANIM_PYRAMID_6, ANIM_PYRAMID_7,  ANIM_PYRAMID_8, ANIM_PYRAMID_9, ANIM_PYRAMID_10,
	ANIM_PYRAMID_11, ANIM_PYRAMID_12, ANIM_PYRAMID_13, ANIM_PYRAMID_14, ANIM_PYRAMID_15,
	ANIM_PYRAMID_16, ANIM_PYRAMID_17, ANIM_PYRAMID_18, ANIM_PYRAMID_19, ANIM_PYRAMID_20,
	ANIM_PYRAMID_21, ANIM_PYRAMID_22, ANIM_PYRAMID_23, ANIM_PYRAMID_24, ANIM_PYRAMID_25,
	ANIM_PYRAMID_26, ANIM_PYRAMID_27, ANIM_PYRAMID_28, ANIM_PYRAMID_29, ANIM_PYRAMID_30,
	ANIM_PYRAMID_31, ANIM_PYRAMID_32, ANIM_PYRAMID_33, ANIM_PYRAMID_34, ANIM_PYRAMID_35,
	ANIM_PYRAMID_36, ANIM_PYRAMID_37, ANIM_PYRAMID_38, ANIM_PYRAMID_39, ANIM_PYRAMID_40,
	ANIM_PYRAMID_41, ANIM_PYRAMID_42, ANIM_PYRAMID_43, ANIM_PYRAMID_44, ANIM_PYRAMID_45,
	ANIM_PYRAMID_46, ANIM_PYRAMID_47, ANIM_PYRAMID_48, ANIM_PYRAMID_49, ANIM_PYRAMID_50,
	ANIM_PYRAMID_51, ANIM_PYRAMID_52, ANIM_PYRAMID_53, ANIM_PYRAMID_54, ANIM_PYRAMID_55,
	ANIM_PYRAMID_56
};
#endif


// Animation structs
static anim_t city_anim = {
	.frames = city_paths,
	.frame_cnt = CITY_FRAME_CNT,
	.pingpong = false,
	.forward = true,
	.cur = 0,
	.img = NULL,
	.timer = NULL
};

static anim_t black_hole_anim = {
	.frames = black_hole_paths,
	.frame_cnt = BLACK_HOLE_FRAME_CNT,
	.pingpong = false,
	.forward = true,
	.cur = 0,
	.img = NULL,
	.timer = NULL
};

static anim_t matrix_rain_anim = {
	.frames = matrix_rain_paths,
	.frame_cnt = MATRIX_RAIN_FRAME_CNT,
	.pingpong = false,
	.forward = true,
	.cur = 0,
	.img = NULL,
	.timer = NULL
};

#ifdef POLYCAST5_EN_PYRAMID_ANIM
static anim_t pyramid_anim = {
	.frames = pyramid_paths,
	.frame_cnt = PYRAMID_FRAME_CNT,
	.pingpong = false,
	.forward = true,
	.cur = 0,
	.img = NULL,
	.timer = NULL
};
#endif


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

		// Window: columns = [x1..x2], rows = [y..y + chunk − 1]
		spi_master_write_command(&tft, 0x2A);
		spi_master_write_addr(&tft, x1 + tft._offsetx, x2 + tft._offsetx);
		spi_master_write_command(&tft, 0x2B);
		spi_master_write_addr(&tft, y + tft._offsety, (y + chunk - 1) + tft._offsety);

		// Push chunk-worth of pixels
		spi_master_write_command(&tft, 0x2C);
		spi_master_write_colors(&tft, color_ptr, (uint32_t)width * chunk);

		// Advance
		color_ptr += (uint32_t)width * chunk;
		y += chunk;
		remaining -= chunk;
	}

	// Tell LVGL we’re done
	lv_disp_flush_ready(d);
	
	xSemaphoreGive(xSPIBusMutex); // Release SPI bus
}

static void lcd_panel_sleep(void)
{
	xSemaphoreTake(xSPIBusMutex, portMAX_DELAY); // Lock SPI bus
	
	// Display off, sleep in
	spi_master_write_command(&tft, 0x28); // DISPOFF
	vTaskDelay(pdMS_TO_TICKS(10));
	spi_master_write_command(&tft, 0x10); // SLPIN
	
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
	
	spi_master_write_command(&tft, 0x21); // INVON
	
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
	
	// Require pin re-entry
	settings_menu.pin_menu.prompt_pin = true;

	// TODO: Noticable delay on LCD
	//xSemaphoreGive(xWifiCycleSemaphore); // Cycle Wi-Fi radio on wake
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

static void warm_img(const char *path) {
	lv_image_decoder_dsc_t dsc;
	if (lv_image_decoder_open(&dsc, path, NULL) == LV_RESULT_OK) {
		lv_image_decoder_close(&dsc);
	}
}

static void warm_anim(const char **paths, int cnt) {
	lv_image_decoder_dsc_t dsc;
	for (int i = 0; i < cnt; ++i) {
		if (lv_image_decoder_open(&dsc, paths[i], NULL) == LV_RESULT_OK) {
			lv_image_decoder_close(&dsc);
		}
	}
}

void lcd_lvgl_init(void)
{
	// Mount SPIFFS so that "/assets/…" works
	esp_vfs_spiffs_conf_t cfg = {
		.base_path = "/assets",
		.partition_label = "assets",
		.max_files = (CITY_FRAME_CNT + BLACK_HOLE_FRAME_CNT + MATRIX_RAIN_FRAME_CNT + PYRAMID_FRAME_CNT) * 6, // Plenty of PSRAM available for now
		.format_if_mount_failed = false
	};
	esp_err_t ret = esp_vfs_spiffs_register(&cfg);
	if (ret != ESP_OK) {
		ESP_LOGE("LCD", "SPIFFS mount failed: %d", ret);
		return;
	} 
	else {
		ESP_LOGI("LCD", "SPIFFS mounted successfully");
	}
	
	// LVGL library init
	lv_init();
	
	// Initialize LVGL's POSIX file system driver (binds to VFS/SPIFFS)
	lv_fs_posix_init();
	
	 // Reserve slots in the decoded-image cache
	lv_image_cache_init((CITY_FRAME_CNT + BLACK_HOLE_FRAME_CNT + MATRIX_RAIN_FRAME_CNT + PYRAMID_FRAME_CNT) * 3);

	// Draw‐buffer: HOR_RES x DRAW_LINES lines
	// Allocate space for 20 lines of 240 px each (~9.6 kB), DMA-capable in DRAM
	static DRAM_ATTR lv_color_t buf[HOR_RES * DRAW_LINES * 2]
			__attribute__((aligned(4)));
		
	static lv_draw_buf_t draw_buf;
	lv_draw_buf_init(&draw_buf, HOR_RES, DRAW_LINES, LV_COLOR_FORMAT_NATIVE, 0, buf, sizeof(buf));
					 
	// Create the display object
	disp = lv_display_create(HOR_RES, VER_RES); // 240x135 logical
	lv_display_set_flush_cb(disp, st7789_flush_cb);
	lv_display_set_draw_buffers(disp, &draw_buf, NULL);

	// 1 ms tick timer feeding lv_tick_inc()
	const esp_timer_create_args_t tick_args = {
		.callback = lv_tick_cb,
		.name = "lv_tick",
		.skip_unhandled_events = true,
	};
	esp_timer_handle_t tick_timer;
	esp_timer_create(&tick_args, &tick_timer);
	esp_timer_start_periodic(tick_timer, 1000);
	
	// Pre-load animations for quick access (but longer boot time)
	warm_anim(city_paths, CITY_FRAME_CNT);
	warm_anim(black_hole_paths, BLACK_HOLE_FRAME_CNT);
	warm_anim(matrix_rain_paths, MATRIX_RAIN_FRAME_CNT);
	#ifdef POLYCAST5_EN_PYRAMID_ANIM
	warm_anim(pyramid_paths, PYRAMID_FRAME_CNT);
	#endif
	
	// Pre-load images too
	warm_img(IMG_DICE_1);
	warm_img(IMG_DICE_2);
	warm_img(IMG_DICE_3);
	warm_img(IMG_DICE_4);
	warm_img(IMG_DICE_5);
	warm_img(IMG_DICE_6);
	
	// And QRs
	warm_img(QR_PC5_BOOT);
	// Other QRs are generated dynamically
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
	if (already_scrolling) {
		return;
	}
	already_scrolling = true;
	
	/* Decide start/end Y */
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

	/* Build the LVGL animation object */
	// Bottom animation
	lv_anim_t a1;
	lv_anim_init(&a1);
	lv_anim_set_var(&a1, menu->lbl_bot);
	if (!scrolling_up) {
		lv_label_set_text(menu->lbl_bot, lv_label_get_text(menu->lbl_mid));
	}
	lv_anim_set_exec_cb(&a1, (lv_anim_exec_xcb_t)lv_obj_set_y);
	lv_anim_set_path_cb(&a1, lv_anim_path_linear);
	lv_anim_set_values(&a1, start_b, end_b);
	lv_anim_set_time(&a1, dur);
	
	// Top animation
	lv_anim_t a2;
	lv_anim_init(&a2);
	lv_anim_set_var(&a2, menu->lbl_top);
	if (scrolling_up) {
		lv_label_set_text(menu->lbl_top, lv_label_get_text(menu->lbl_mid));
	}
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

void lcd_unhide_selection_widgets(ui_menu_t *ui_menu)
{
	// Show center button and it's label
	lv_obj_t *btn_mid = lv_obj_get_parent(ui_menu->lbl_mid);
	lv_obj_remove_flag(btn_mid, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);

	// Show top and bottom labels
	lv_obj_remove_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);

	// Ensure arrows visible
	lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

	// Reset X-coord so they are back on-screen
	lv_obj_set_x(ui_menu->lbl_top, 0);
	lv_obj_set_x(btn_mid, 0);
	lv_obj_set_x(ui_menu->lbl_bot, 0);
	
	// Update scrollbar thumb y (reversed direction, precise double, no jump)
	int max_y = SELECTION_SCROLLBAR_CONT_HEIGHT - SELECTION_SCROLLBAR_THUMB_HEIGHT;
	double fraction = (double)ui_menu->index / (ui_menu->size - 1); // Double for smooth/no jump
	int y = (int)(fraction * max_y + 0.5); // Round for consistency
	lv_obj_set_y(ui_menu->scroll_bar, y + SELECTION_SCROLLBAR_OFFSET);
	
	// Show scrollbar
	lv_obj_remove_flag(ui_menu->scroll_bar, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(ui_menu->scroll_track, LV_OBJ_FLAG_HIDDEN);
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
		ESP_LOGE(TAG, "Failed to save NVS animation");
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
	if (anim_active != CITY) {
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
	if (anim_active != BLACK_HOLE) {
		lv_obj_add_flag(black_hole_anim.img, LV_OBJ_FLAG_HIDDEN);
		lv_timer_pause(black_hole_anim.timer);
	}
	
	/* Matrix rain */
	// Create image
	matrix_rain_anim.img = lv_img_create(ACTIVE_SCR);
	lv_img_set_src(matrix_rain_anim.img, matrix_rain_anim.frames[0]);
	lv_obj_center(matrix_rain_anim.img);
	
	// Create timer
	matrix_rain_anim.timer = lv_timer_create(anim_timer_cb, MATRIX_RAIN_FRAME_PERIOD, &matrix_rain_anim);
	
	// Check if set
	if (anim_active != MATRIX_RAIN) {
		lv_obj_add_flag(matrix_rain_anim.img, LV_OBJ_FLAG_HIDDEN);
		lv_timer_pause(matrix_rain_anim.timer);
	}
	
	/* Pyramid */
	#ifdef POLYCAST5_EN_PYRAMID_ANIM
	// Create image
	pyramid_anim.img = lv_img_create(ACTIVE_SCR);
	lv_img_set_src(pyramid_anim.img, pyramid_anim.frames[0]);
	lv_obj_center(pyramid_anim.img);
	
	// Create timer
	pyramid_anim.timer = lv_timer_create(anim_timer_cb, PYRAMID_FRAME_PERIOD, &pyramid_anim);
	
	// Check if set
	if (anim_active != PYRAMID) {
		lv_obj_add_flag(pyramid_anim.img, LV_OBJ_FLAG_HIDDEN);
		lv_timer_pause(pyramid_anim.timer);
	}
	#endif
}

#ifdef POLYCAST5_PERSIST_SELECTION_INDEX
static void lcd_selection_index_nvs_save(const ui_menu_t *ui_menu)
{
	nvs_handle_t h;
	esp_err_t err;

	// Open NVS
	err = nvs_open(SEL_MENU_NS, NVS_READWRITE, &h);
	if (err == ESP_OK) {
		uint8_t idx = (uint8_t)(ui_menu->index % ui_menu->size);
		
		// Save index on success
		nvs_set_u8(h, SEL_MENU_INDEX_KEY, idx);
		
		// Commit changes
		nvs_commit(h);
	
		// Close NVS
		nvs_close(h);
	}
	else {
		ESP_LOGE(TAG, "lcd_selection_index_nvs_save nvs_open failed: %s", esp_err_to_name(err));
	}
}

void lcd_selection_index_nvs_load(ui_menu_t *ui_menu)
{
	nvs_handle_t h;
	esp_err_t err;

	// Open NVS
	err = nvs_open(SEL_MENU_NS, NVS_READONLY, &h);
	if (err == ESP_OK) {
		uint8_t idx = SELECTION_DEFAULT_IDX;

		// Get index on success
		err = nvs_get_u8(h, SEL_MENU_INDEX_KEY, &idx);
		if (err == ESP_OK) {
			// Check if valid
			if (idx < ui_menu->size) {
				ui_menu->index = (int)idx; // Assign
			}
		}
		else {
			ESP_LOGW(TAG, "lcd_selection_index_nvs_load nvs_get_u8 failed: %s", esp_err_to_name(err));
		}

		// Close NVS
		nvs_close(h);
	}
	else {
		ESP_LOGW(TAG, "lcd_selection_index_nvs_load nvs_open failed: %s", esp_err_to_name(err));
	}
}
#else
static inline void lcd_selection_sync_labels(ui_menu_t *m)
{
	// Update top and bottom labels based on middle index
    int mid = m->index;
    int top = (mid + m->size - 1) % m->size;
    int bot = (mid + 1) % m->size;

	// Set new text
    lv_label_set_text(m->lbl_top, m->options[top]);
    lv_label_set_text(m->lbl_mid, m->options[mid]);
    lv_label_set_text(m->lbl_bot, m->options[bot]);
}
#endif

void lcd_init_selection_labels(ui_menu_t *ui_menu)
{
	// Create selection scrollbar thumb (movable obj)
	ui_menu->scroll_track = lv_obj_create(ACTIVE_SCR);
	lv_obj_set_size(ui_menu->scroll_track, 4, SELECTION_SCROLLBAR_CONT_HEIGHT);
	lv_obj_align(ui_menu->scroll_track, LV_ALIGN_RIGHT_MID, -12, 0);
	
	// Track style
	static lv_style_t track_style;
	lv_style_init(&track_style);
	lv_style_set_bg_opa(&track_style, LV_OPA_100);
	lv_style_set_bg_color(&track_style, lv_color_darken(user_primary_color, 100));
	lv_style_set_radius(&track_style, 3); // Rounded
	lv_style_set_border_width(&track_style, 0); // No border

	lv_obj_add_style(ui_menu->scroll_track, &track_style, LV_PART_MAIN);
	
	// Create selection scrollbar thumb (movable obj)
	ui_menu->scroll_bar = lv_obj_create(ACTIVE_SCR); // Thumb obj
	lv_obj_set_size(ui_menu->scroll_bar, 4, SELECTION_SCROLLBAR_THUMB_HEIGHT);
	lv_obj_align(ui_menu->scroll_bar, LV_ALIGN_RIGHT_MID, -12, 0);
	
	// Style to match main scrollbar
	static lv_style_t bar_style;
	lv_style_init(&bar_style);
	lv_style_set_bg_opa(&bar_style, LV_OPA_80);
	lv_style_set_bg_color(&bar_style, user_secondary_color);
	lv_style_set_radius(&bar_style, 3); // Rounded
	lv_style_set_border_width(&bar_style, 0); // No border

	lv_obj_add_style(ui_menu->scroll_bar, &bar_style, LV_PART_MAIN);
	
	// Update thumb y (reversed direction, precise double, no jump)
	int max_y = SELECTION_SCROLLBAR_CONT_HEIGHT - SELECTION_SCROLLBAR_THUMB_HEIGHT;
	double fraction = (double)ui_menu->index / (ui_menu->size - 1); // Double for smooth/no jump
	int y = (int)(fraction * max_y + 0.5); // Round for consistency
	lv_obj_set_y(ui_menu->scroll_bar, y + SELECTION_SCROLLBAR_OFFSET);
	
	// Align track
	lv_obj_set_y(ui_menu->scroll_track, y + SELECTION_SCROLLBAR_OFFSET + 7);
	
	// Create and format center button
	ui_menu->btn_mid = lv_btn_create(ACTIVE_SCR);
	lcd_format_center_button(ui_menu->btn_mid, user_primary_color, user_secondary_color);

	// Resolve indices
    int size = ui_menu->size;
    int mid = ((ui_menu->index % size) + size) % size;
    int top = (mid - 1 + size) % size; // 0 -> size - 1
    int bot = (mid + 1) % size; // size - 1 -> 0

	// Format labels
	ui_menu->lbl_top = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu->lbl_top, ui_menu->options[top], user_secondary_color,
			&lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

	ui_menu->lbl_mid = lv_label_create(ui_menu->btn_mid);
	lcd_format_label(ui_menu->lbl_mid, ui_menu->options[mid], user_secondary_color, 
			&lv_font_montserrat_30, LV_ALIGN_CENTER, 0, 0);
					 
	ui_menu->lbl_bot = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu->lbl_bot, ui_menu->options[bot], user_secondary_color,
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

	// Hotkey icon
	ui_menu->lbl_hotkey_icon = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu->lbl_hotkey_icon, LV_SYMBOL_EYE_OPEN, user_secondary_color,
			&lv_font_montserrat_18, LV_ALIGN_TOP_LEFT, 4, -1);
					 
	// Hide all for now
	lv_obj_add_flag(ui_menu->btn_mid, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);
	
	lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
	
	lv_obj_add_flag(ui_menu->lbl_hotkey_icon, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->scroll_bar, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->scroll_track, LV_OBJ_FLAG_HIDDEN);
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

void lcd_update_battery(ui_menu_t *ui_menu, uint8_t battery_percentage, bool charging)
{
	// If not charging, update percentage
	if (!charging) {
		lv_obj_align(ui_menu->lbl_battery_icon, LV_ALIGN_TOP_RIGHT, -2, -3); // Revert formatting
		
		char buf[4]; // 3 max + NULL
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
	// Else show CHG symbol instead
	else {
		lv_label_set_text(ui_menu->lbl_battery_txt, ""); // No text
		
		// Format and set CHG symbol
		lv_obj_align(ui_menu->lbl_battery_icon, LV_ALIGN_TOP_RIGHT, -4, 2);
		lv_label_set_text(ui_menu->lbl_battery_icon, LV_SYMBOL_CHARGE);
	}
}

static void lcd_selection_btn_pressed(ui_menu_t *ui_menu, ir_menu_t *ir_menu, lora_menu_t *lora_menu, espnow_menu_t *espnow_menu,
		wifi_menu_t *wifi_menu, tools_menu_t *tools_menu, settings_menu_t *settings_menu, bluetooth_menu_t *bluetooth_menu, gpio_menu_t *gpio_menu)
{
	// Hide selection menu scrollbar
	lv_obj_add_flag(ui_menu->scroll_bar, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->scroll_track, LV_OBJ_FLAG_HIDDEN);
	
	// Hide selection labels
	lv_obj_add_flag(ui_menu->btn_mid, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);
	
	const char *option = lv_label_get_text(ui_menu->lbl_mid);
	
	if (strcmp(option, OPTION_INFRARED) == 0) {	
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Build ir_list
		xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
		lcd_ir_build_current_menu(ir_menu, ir_current_remote);
		xSemaphoreGive(xInfraredDataMutex); // Release IR
		
		// Show IR list
		lv_obj_remove_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);	
		ui_menu->page = INFRARED_PAGE;
	}
	else if (strcmp(option, OPTION_BLUETOOTH) == 0) {
		// Show bluetooth list
		lv_obj_remove_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = BLUETOOTH_PAGE;
	}
	else if (strcmp(option, OPTION_LORA) == 0) {
		// Show LoRa list
		lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);	
		ui_menu->page = LORA_PAGE;
	}
	else if (strcmp(option, OPTION_ESPNOW) == 0) {
		// Show ESP-NOW list
		lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = ESPNOW_PAGE;
	}
	else if (strcmp(option, OPTION_TOOLS) == 0) {
		// Show tools list
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = TOOLS_PAGE;
	}
	else if (strcmp(option, OPTION_SETTINGS) == 0) {
		// Show settings list
		lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = SETTINGS_PAGE;
	}
	else if (strcmp(option, OPTION_WIFI) == 0) {
		// Show Wi-Fi list
		lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = WIFI_PAGE;
	}
	else if (strcmp(option, OPTION_GPIO) == 0) {
		// Show GPIO list
		lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = GPIO_PAGE;
	}
	else {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGW(TAG, "Invalid menu option selected");
		#endif
	}
}

// Mark that first boot did happen
esp_err_t lcd_save_first_boot(void)
{
	nvs_handle_t h;
	
	// Open NVS
	esp_err_t err = nvs_open(LCD_FIRST_BOOT_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "lcd_save_first_boot nvs_open failed: %s", esp_err_to_name(err));

		return err;
	}

	// Store anim_active as a single byte
	err = nvs_set_u8(h, LCD_FIRST_BOOT_KEY, 1);
	if (err == ESP_OK) {
		// Commit to flash
		err = nvs_commit(h);
		
		#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Saved first boot ESP_OK");
		#endif
	}
	else {
		ESP_LOGE(TAG, "lcd_save_first_boot nvs_set_u8 failed: %s", esp_err_to_name(err));
	}
	
	// Close NVS
	nvs_close(h);
	return err;
}

// Check if first boot has happened
bool lcd_is_first_boot(void)
{
	nvs_handle_t h;
	
	// Open NVS
	esp_err_t err = nvs_open(LCD_FIRST_BOOT_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGW(TAG, "lcd_is_first_boot nvs_open failed: %s", esp_err_to_name(err));
		#endif

		// Failed to open -> DNE
		return true;
	}
	
	// Get the uint8
	uint8_t stored = 0;
	err = nvs_get_u8(h, LCD_FIRST_BOOT_KEY, &stored);
	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGW(TAG, "lcd_is_first_boot nvs_get_u8 failed: %s", esp_err_to_name(err));
		#endif

		// Close NVS
		nvs_close(h);

		return true;
	}
	
	#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "Loaded lcd_is_first_boot: %d", stored);
	#endif
	
	// Close NVS
	nvs_close(h);
	return false;
}

static void start_animation(void)
{
	// Start the active
	if (anim_active == CITY) {
		lv_obj_remove_flag(city_anim.img, LV_OBJ_FLAG_HIDDEN);
		lv_timer_resume(city_anim.timer);
	}
	else if (anim_active == BLACK_HOLE) {
		lv_obj_remove_flag(black_hole_anim.img,  LV_OBJ_FLAG_HIDDEN);
		lv_timer_resume(black_hole_anim.timer);
	}
	else if (anim_active == MATRIX_RAIN){
		lv_obj_remove_flag(matrix_rain_anim.img,  LV_OBJ_FLAG_HIDDEN);
		lv_timer_resume(matrix_rain_anim.timer);
	}
	#ifdef POLYCAST5_EN_PYRAMID_ANIM
	else if (anim_active == PYRAMID){
		lv_obj_remove_flag(pyramid_anim.img,  LV_OBJ_FLAG_HIDDEN);
		lv_timer_resume(pyramid_anim.timer);
	}
	#endif
}

static void pause_animations(void)
{
	// Halt all animations
	lv_timer_pause(city_anim.timer);
	lv_timer_pause(black_hole_anim.timer);
	lv_timer_pause(matrix_rain_anim.timer);
	#ifdef POLYCAST5_EN_PYRAMID_ANIM
	lv_timer_pause(pyramid_anim.timer);
	#endif
}

static void stop_animations(void)
{
	pause_animations();

	// Hide paused animations
	lv_obj_add_flag(city_anim.img, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(black_hole_anim.img, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(matrix_rain_anim.img, LV_OBJ_FLAG_HIDDEN);
	#ifdef POLYCAST5_EN_PYRAMID_ANIM
	lv_obj_add_flag(pyramid_anim.img, LV_OBJ_FLAG_HIDDEN);
	#endif
}

static void transition_animation(bool dir)
{	
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

void lcd_apply_scrollbar_style(lv_obj_t *obj)
{
	// Show the bar only when the content is scrollable
	lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);

	// Add extra padding to the main part to reserve space for the scrollbar=
	static lv_style_t main_style;
	static bool main_inited = false;
	if (!main_inited) {
		lv_style_init(&main_style);
		lv_style_set_pad_right(&main_style, 24); // Scrollbar thickness: (4px) + gap (8px) + extra 
		main_inited = true;
	}
	lv_obj_add_style(obj, &main_style, LV_PART_MAIN);
	lv_obj_set_x(obj, lv_obj_get_x(obj) + 6); // Realign to center after padding offset

	// Style the scrollbar track/indicator
	static lv_style_t sb_style;
	static bool sb_inited = false;
	if (!sb_inited) {
		lv_style_init(&sb_style);
		
		// Narrow bar on the right side
		lv_style_set_width(&sb_style, 4); // Scrollbar thickness
		lv_style_set_bg_opa(&sb_style, LV_OPA_60); // Make it clearly visible
		lv_style_set_bg_color(&sb_style, user_secondary_color);
		lv_style_set_radius(&sb_style, 3); // Slightly rounded ends
		
		// Position: pad_right = 0 keeps it flush against the right edge; pad_left adds gap from content
		lv_style_set_pad_left(&sb_style, 12); // Small gap from content
		lv_style_set_pad_right(&sb_style, 0); // Keep it to the right
		sb_inited = true;
	}
	// Apply to the scrollbar part of this object
	lv_obj_add_style(obj, &sb_style, LV_PART_SCROLLBAR);
}

// Draw text as QR into an LVGL canvas (RGB565) -> returns 0 on success
int lcd_draw_qr(lv_obj_t *canvas, const char *text, int size_px, uint8_t **pbuf)
{
	// Validate args
	if (!canvas || !text || !*text || size_px <= 0 || !pbuf) {
		return -1;
	}

	// Allocate work buffers (heap, prefer PSRAM)
	uint8_t *tmp = (uint8_t*)heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	if (!tmp) {
		tmp = (uint8_t*)malloc(qrcodegen_BUFFER_LEN_MAX);
	}
	
	uint8_t *qr = (uint8_t*)heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	if (!qr) {
		qr = (uint8_t*)malloc(qrcodegen_BUFFER_LEN_MAX);
	}
	
	if (!tmp || !qr) {
		free(tmp);
		free(qr);
		return -2;
	}

	// Encode QR
	bool ok = qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
			qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
			
	// Free tmp buffer after encode
	free(tmp);
	if (!ok) {
		free(qr);
		return -3;
	}

	// Recreate canvas buffer every call (simple & safe)
	size_t bytes = (size_t)size_px * (size_px) * 2; // RGB565
	if (*pbuf) {
		// Free old buffer
		free(*pbuf);
		*pbuf = NULL;
	}
	
	*pbuf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	
	if (!*pbuf) {
		*pbuf = (uint8_t*)malloc(bytes);
	}
	
	if (!*pbuf) {
		free(qr);
		return -4;
	}

	// Bind buffer to canvas
	lv_canvas_set_buffer(canvas, *pbuf, size_px, size_px, LV_COLOR_FORMAT_RGB565);

	// Paint white background (0xFFFF)
	memset(*pbuf, 0xFF, bytes);

	// Compute scale (QR modules -> pixels)
	int qr_sz = qrcodegen_getSize(qr);
	int border = 2;
	int mods = qr_sz + border * 2;
	float scale = (float)size_px / (float)mods;

	// Draw black modules
	for (int my = 0; my < mods; ++my) {
		for (int mx = 0; mx < mods; ++mx) {
			// Get module
			bool dark = false;
			
			int qx = mx - border, qy = my - border;
			
			if (qx >= 0 && qx < qr_sz && qy >= 0 && qy < qr_sz) {
				dark = qrcodegen_getModule(qr, qx, qy);
			}
			
			if (!dark) {
				continue;
			}

			// Module -> pixel box
			int x0 = (int)(mx * scale);
			int y0 = (int)(my * scale);
			
			int x1 = (int)((mx + 1) * scale);
			if (x1 <= x0) {
				x1 = x0 + 1;
			}
			
			int y1 = (int)((my + 1) * scale);
			if (y1 <= y0) {
				y1 = y0 + 1;
			}

			// Fill box black (0x0000)
			for (int y = y0; y < y1 && y < size_px; ++y) {
				uint16_t *row = (uint16_t*)(*pbuf + (size_t)y * (size_px * 2));
				for (int x = x0; x < x1 && x < size_px; ++x) {
					row[x] = 0x0000;
				}
			}
		}
	}

	// Free QR map
	free(qr);
	return 0;
}

void lcd_boot_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu)
{
	#define BOOT_PAGE_Y_OFFSET 40
	
	// Statics
	static bool init = false;
	static lv_obj_t *cont = NULL;
	static lv_obj_t *title_lbl = NULL;
	static lv_obj_t *instr_lbl = NULL;
	static lv_obj_t *qr_active;
	
	if (!init) {
		pause_animations();

		// Create a scrollable container for the instructions
		cont = lv_obj_create(ACTIVE_SCR);
		lv_obj_set_size(cont, 210, 106);
		lv_obj_center(cont);
		lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Rounded corners for appeal
		lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
		lv_obj_set_scroll_dir(cont, LV_DIR_VER);
		lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Padding for content

		// Title label
		title_lbl = lv_label_create(cont);
		lv_label_set_text(title_lbl, "READ ME");
		lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
		lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

		// Instructions label (scrollable if text is long)
		instr_lbl = lv_label_create(cont);
		lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(instr_lbl, lv_pct(100)); // Full width for wrapping
		lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_16, 0);
		lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
		lv_obj_align_to(instr_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

		// Create QR (Artboard = 60x60)
		qr_active = lv_img_create(cont);
		lv_img_set_src(qr_active, QR_PC5_BOOT);
		lv_obj_align_to(qr_active, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 233);

		// Set instruction text
		const char *instr_text = 
								"Hello, welcome to PolyCast5! Press the down arrow to scroll.\n\n"
								"Below is some quick info to help you get started!\n\n"
								"To get the most out of your PolyCast5, check out polycast5.com:\n\n"
								"\n\n\n"
								"It has a lot of docs and tutorials to help you unleash this device's full potential.\n\n"
								"Also, in the unlikely case that anything should ever be glitchy, you can do a safe hardware reboot by pressing "
								"the home and right buttons at the same time.\n\n"
								"To continue, please push the right arrow button. This menu will not appear again."
								"";
		
		lv_label_set_text(instr_lbl, instr_text);
	
		init = true;
	}
	
	if (ui_btns->up_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, BOOT_PAGE_Y_OFFSET, LV_ANIM_ON);
	}
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, -BOOT_PAGE_Y_OFFSET, LV_ANIM_ON);
	}
	// Confirm
	else if (ui_btns->right_btn == 1) {
		// Save first boot
		esp_err_t err = lcd_save_first_boot();
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "lcd_save_first_boot failed: %s", esp_err_to_name(err));
		}

		// Delete objects
		lv_obj_delete(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		qr_active = NULL;
		init = false;
		
		start_animation();
		
		// Go home
		ui_menu->page = HOME_PAGE;
	}
	// Power off without switching pages (stay on BOOT_PAGE)
	else if (ui_btns->pwr_btn == 1) {		
		// Transition to sleep
		gpio_set_level(ST7789_LEDA_PIN, 0); // BL low so user doesn't see redraw
		
		go_to_sleep = true;
	}
}

void lcd_home_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
	if (ui_btns->up_btn == 1) {
		transition_animation(true);
	}
	else if (ui_btns->down_btn == 1) {
		transition_animation(false);
	}
	// Request selection page
	else if (ui_btns->select_btn == 1) {
		stop_animations();
		
		// Go to selection page if pin not set
		if (!settings_menu->pin_menu.pin_set || !settings_menu->pin_menu.prompt_pin) {
			#ifndef POLYCAST5_PERSIST_SELECTION_INDEX
			ui_menu->index = SELECTION_DEFAULT_IDX; // Default start
			lcd_selection_sync_labels(ui_menu); // Sync menu from here
			#endif

			// Show selection page
			lcd_unhide_selection_widgets(ui_menu);
				
			// Switch execution to selection page
			ui_menu->page = SELECTION_PAGE;
		}
		// Else prompt pin
		else {
			// Going to pin to selection page
			pin_to_selection_page = true;
			
			// Show arrows
			lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			
			// Show pin prompt
			lv_obj_remove_flag(settings_menu->pin_menu.pin_container, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(settings_menu->pin_menu.lbl_ins, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(settings_menu->pin_menu.lbl_back, LV_OBJ_FLAG_HIDDEN);
			
			// Show wrong attempts
			if (pin_attempts > 0) {
				// Build and set attempts string
				char buf[18];
				snprintf(buf, sizeof(buf), "WRONG: %" PRIu32, pin_attempts);
				lv_label_set_text(settings_menu->pin_menu.lbl_attempts, buf);
				
				// Show
				lv_obj_remove_flag(settings_menu->pin_menu.lbl_attempts, LV_OBJ_FLAG_HIDDEN);
			}
			
			pin_signing_in = true;
	
			ui_menu->page = UNLOCK_PAGE;
		}
	}
	// Request hotkey page
	else if (ui_btns->left_btn == 1) {
		stop_animations();
		
		// Go to hotkey page if pin not set
		if (!settings_menu->pin_menu.pin_set || !settings_menu->pin_menu.prompt_pin) {
			// Show arrows
			lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
	
			// Default index
			hotkey_menu.index = (MAX_HOTKEY_OPTIONS - 1);
			lcd_hotkey_update_menu(&hotkey_menu);
	
			// Switch pages
			ui_menu->page = HOTKEY_PAGE;
		}
		// Else prompt pin
		else {
			// Going to pin to hotkey page
			pin_to_selection_page = false;
			
			// Show arrows
			lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			
			// Show pin prompt
			lv_obj_remove_flag(settings_menu->pin_menu.pin_container, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(settings_menu->pin_menu.lbl_ins, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(settings_menu->pin_menu.lbl_back, LV_OBJ_FLAG_HIDDEN);
			
			// Show wrong attempts
			if (pin_attempts > 0) {
				// Build and set attempts string
				char buf[18];
				snprintf(buf, sizeof(buf), "WRONG: %" PRIu32, pin_attempts);
				lv_label_set_text(settings_menu->pin_menu.lbl_attempts, buf);
				
				// Show
				lv_obj_remove_flag(settings_menu->pin_menu.lbl_attempts, LV_OBJ_FLAG_HIDDEN);
			}
			
			pin_signing_in = true;
	
			ui_menu->page = UNLOCK_PAGE;
		}		
	}
	
	/* HOTKEYS */
	// Note: If you'd like it so you can only use hotkeys if the device is unlocked,
	// simply add '&& (!settings_menu->pin_menu.pin_set || !settings_menu->pin_menu.prompt_pin)' to each hoykey 'else if'
	
	// Long press home
	else if (xHomeButtonLongSemaphore && xSemaphoreTake(xHomeButtonLongSemaphore, 0) == pdTRUE) {
		// If LoRa command exists
		if (hotkey_cmd.has_lora[HOTKEY_LONG_HOME_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xLoraSendEncQueue, &hotkey_cmd.lora_cmd[HOTKEY_LONG_HOME_IDX], portMAX_DELAY);
		}
		// Else ESP-NOW
		else if (hotkey_cmd.has_espnow[HOTKEY_LONG_HOME_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xEspSendCmdQueue, &hotkey_cmd.espnow_cmd[HOTKEY_LONG_HOME_IDX], portMAX_DELAY);
		}
		// Else Infrared
		else if (hotkey_cmd.has_ir[HOTKEY_LONG_HOME_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_PURPLE;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Update current remote
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			ir_current_remote = hotkey_cmd.ir_cmd[HOTKEY_LONG_HOME_IDX].current_remote;
			xSemaphoreGive(xInfraredDataMutex); // Release IR
			
			// Send the command
			xQueueSend(xInfraredSignalToTxQueue, &hotkey_cmd.ir_cmd[HOTKEY_LONG_HOME_IDX].index, portMAX_DELAY);
		}
		else {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Long home hotkey DNE, index='%d' has_lora='%d' has_espnow='%d'", HOTKEY_LONG_HOME_IDX,
					hotkey_cmd.has_lora[HOTKEY_LONG_HOME_IDX], hotkey_cmd.has_espnow[HOTKEY_LONG_HOME_IDX]);
			#endif
		}
	}
	// Long press left
	else if (xLeftButtonLongSemaphore && xSemaphoreTake(xLeftButtonLongSemaphore, 0) == pdTRUE) {
		// If LoRa command exists
		if (hotkey_cmd.has_lora[HOTKEY_LONG_LEFT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xLoraSendEncQueue, &hotkey_cmd.lora_cmd[HOTKEY_LONG_LEFT_IDX], portMAX_DELAY);
		}
		// Else ESP-NOW
		else if (hotkey_cmd.has_espnow[HOTKEY_LONG_LEFT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xEspSendCmdQueue, &hotkey_cmd.espnow_cmd[HOTKEY_LONG_LEFT_IDX], portMAX_DELAY);
		}
		// Else Infrared
		else if (hotkey_cmd.has_ir[HOTKEY_LONG_LEFT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_PURPLE;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Update current remote
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			ir_current_remote = hotkey_cmd.ir_cmd[HOTKEY_LONG_LEFT_IDX].current_remote;
			xSemaphoreGive(xInfraredDataMutex); // Release IR
			
			// Send the command
			xQueueSend(xInfraredSignalToTxQueue, &hotkey_cmd.ir_cmd[HOTKEY_LONG_LEFT_IDX].index, portMAX_DELAY);
		}
		else {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Long left hotkey DNE, index='%d' has_lora='%d' has_espnow='%d'", HOTKEY_LONG_LEFT_IDX,
					hotkey_cmd.has_lora[HOTKEY_LONG_LEFT_IDX], hotkey_cmd.has_espnow[HOTKEY_LONG_LEFT_IDX]);
			#endif
		}
	}
	// Long press right
	else if (xRightButtonLongSemaphore && xSemaphoreTake(xRightButtonLongSemaphore, 0) == pdTRUE) {
		// If LoRa command exists
		if (hotkey_cmd.has_lora[HOTKEY_LONG_RIGHT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xLoraSendEncQueue, &hotkey_cmd.lora_cmd[HOTKEY_LONG_RIGHT_IDX], portMAX_DELAY);
		}
		// Else ESP-NOW
		else if (hotkey_cmd.has_espnow[HOTKEY_LONG_RIGHT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xEspSendCmdQueue, &hotkey_cmd.espnow_cmd[HOTKEY_LONG_RIGHT_IDX], portMAX_DELAY);
		}
		// Else Infrared
		else if (hotkey_cmd.has_ir[HOTKEY_LONG_RIGHT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_PURPLE;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Update current remote
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			ir_current_remote = hotkey_cmd.ir_cmd[HOTKEY_LONG_RIGHT_IDX].current_remote;
			xSemaphoreGive(xInfraredDataMutex); // Release IR
			
			// Send the command
			xQueueSend(xInfraredSignalToTxQueue, &hotkey_cmd.ir_cmd[HOTKEY_LONG_RIGHT_IDX].index, portMAX_DELAY);
		}
		else {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Long right hotkey DNE, index='%d' has_lora='%d' has_espnow='%d'", HOTKEY_LONG_RIGHT_IDX,
					hotkey_cmd.has_lora[HOTKEY_LONG_RIGHT_IDX], hotkey_cmd.has_espnow[HOTKEY_LONG_RIGHT_IDX]);
			#endif
		}
	}
	// Long press select
	else if (xSelectButtonLongSemaphore && xSemaphoreTake(xSelectButtonLongSemaphore, 0) == pdTRUE) {
		// If LoRa command exists
		if (hotkey_cmd.has_lora[HOTKEY_LONG_SELECT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xLoraSendEncQueue, &hotkey_cmd.lora_cmd[HOTKEY_LONG_SELECT_IDX], portMAX_DELAY);
		}
		// Else ESP-NOW
		else if (hotkey_cmd.has_espnow[HOTKEY_LONG_SELECT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xEspSendCmdQueue, &hotkey_cmd.espnow_cmd[HOTKEY_LONG_SELECT_IDX], portMAX_DELAY);
		}
		// Else Infrared
		else if (hotkey_cmd.has_ir[HOTKEY_LONG_SELECT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_PURPLE;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Update current remote
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			ir_current_remote = hotkey_cmd.ir_cmd[HOTKEY_LONG_SELECT_IDX].current_remote;
			xSemaphoreGive(xInfraredDataMutex); // Release IR
			
			// Send the command
			xQueueSend(xInfraredSignalToTxQueue, &hotkey_cmd.ir_cmd[HOTKEY_LONG_SELECT_IDX].index, portMAX_DELAY);
		}
		else {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Long select hotkey DNE, index='%d' has_lora='%d' has_espnow='%d'", HOTKEY_LONG_SELECT_IDX,
					hotkey_cmd.has_lora[HOTKEY_LONG_SELECT_IDX], hotkey_cmd.has_espnow[HOTKEY_LONG_SELECT_IDX]);
			#endif
		}
	}
	// Short press home
	else if (ui_btns->home_btn == 1) {
		// If LoRa command exists
		if (hotkey_cmd.has_lora[HOTKEY_SHORT_HOME_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xLoraSendEncQueue, &hotkey_cmd.lora_cmd[HOTKEY_SHORT_HOME_IDX], portMAX_DELAY);
		}
		// Else ESP-NOW
		else if (hotkey_cmd.has_espnow[HOTKEY_SHORT_HOME_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xEspSendCmdQueue, &hotkey_cmd.espnow_cmd[HOTKEY_SHORT_HOME_IDX], portMAX_DELAY);
		}
		// Else Infrared
		else if (hotkey_cmd.has_ir[HOTKEY_SHORT_HOME_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_PURPLE;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
				
			// Update current remote
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			ir_current_remote = hotkey_cmd.ir_cmd[HOTKEY_SHORT_HOME_IDX].current_remote;
			xSemaphoreGive(xInfraredDataMutex); // Release IR
			
			// Send the command
			xQueueSend(xInfraredSignalToTxQueue, &hotkey_cmd.ir_cmd[HOTKEY_SHORT_HOME_IDX].index, portMAX_DELAY);
		}
		else {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Short home hotkey DNE, index='%d' has_lora='%d' has_espnow='%d'", HOTKEY_SHORT_HOME_IDX,
					hotkey_cmd.has_lora[HOTKEY_SHORT_HOME_IDX], hotkey_cmd.has_espnow[HOTKEY_SHORT_HOME_IDX]);
			#endif
		}
	}
	// Short press right
	else if (ui_btns->right_btn == 1) {
		// If LoRa command exists
		if (hotkey_cmd.has_lora[HOTKEY_SHORT_RIGHT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xLoraSendEncQueue, &hotkey_cmd.lora_cmd[HOTKEY_SHORT_RIGHT_IDX], portMAX_DELAY);
		}
		// Else ESP-NOW
		else if (hotkey_cmd.has_espnow[HOTKEY_SHORT_RIGHT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_TEAL;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Send the command
			xQueueSend(xEspSendCmdQueue, &hotkey_cmd.espnow_cmd[HOTKEY_SHORT_RIGHT_IDX], portMAX_DELAY);
		}
		// Else Infrared
		else if (hotkey_cmd.has_ir[HOTKEY_SHORT_RIGHT_IDX]) {
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_PURPLE;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
					
			// Update current remote
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			ir_current_remote = hotkey_cmd.ir_cmd[HOTKEY_SHORT_RIGHT_IDX].current_remote;
			xSemaphoreGive(xInfraredDataMutex); // Release IR
			
			// Send the command
			xQueueSend(xInfraredSignalToTxQueue, &hotkey_cmd.ir_cmd[HOTKEY_SHORT_RIGHT_IDX].index, portMAX_DELAY);
		}
		else {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Short right hotkey DNE, index='%d' has_lora='%d' has_espnow='%d'", HOTKEY_SHORT_RIGHT_IDX,
					hotkey_cmd.has_lora[HOTKEY_SHORT_RIGHT_IDX], hotkey_cmd.has_espnow[HOTKEY_SHORT_RIGHT_IDX]);
			#endif
		}
	}
}

void lcd_unlock_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
	// Statics
	static int num_filled = 0;
	static int num_boxes = 0;
	static char input_pin[SETTINGS_MAX_PIN_LEN + 1];
	static lv_obj_t *unlock_labels[SETTINGS_MAX_PIN_LEN];
	
	// Pin input
	if ((ui_btns->up_btn == 1 || ui_btns->down_btn == 1 || ui_btns->left_btn == 1 || ui_btns->right_btn == 1) && (num_filled < SETTINGS_MAX_PIN_LEN)) {
		char code = '\0';
		
		// Assign code for unlock_pin
		if (ui_btns->up_btn) {
			code = 'U';
		}
		else if(ui_btns->down_btn) {
			code = 'D';
		}
		else if(ui_btns->left_btn) {
			code = 'L';
		} 
		else if(ui_btns->right_btn) {
			code = 'R';
		}

		// Save and rebuild
		input_pin[num_filled++] = code;
		lcd_settings_rebuild_pin_boxes(settings_menu->pin_menu.pin_container, unlock_labels,
				input_pin, &num_boxes, num_filled);
	}
	// Back
	else if(ui_btns->home_btn) {
		// Back one box
		if (num_filled > 0) {
			input_pin[num_filled--] = '\0'; // Ensure termination
			lcd_settings_rebuild_pin_boxes(settings_menu->pin_menu.pin_container, unlock_labels,
					input_pin, &num_boxes, num_filled);
		}
		// Go home
		else {
			// Hide arrows
			lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
				
			// Hide pin prompt
			lv_obj_add_flag(settings_menu->pin_menu.pin_container, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(settings_menu->pin_menu.lbl_ins, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(settings_menu->pin_menu.lbl_back, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(settings_menu->pin_menu.lbl_attempts, LV_OBJ_FLAG_HIDDEN);
			
			// Reset
			num_filled = num_boxes = 0; // Zero out
			memset(input_pin, 0, sizeof(input_pin));
			
			start_animation();
			
			// Go back
			ui_menu->page = HOME_PAGE;
		}
	}
	// Check against actual
	else if (ui_btns->select_btn == 1) {
		input_pin[num_filled] = '\0'; // Ensure termination
			
		#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Got pin: %s", input_pin);
		#endif

		#ifdef POLYCAST5_PASS_DEBUG
		ESP_LOGI(TAG, "Need pin: %s", settings_menu->pin_menu.unlock_pin);
		#endif
		
		// If PIN is correct
		if (strcmp(input_pin, settings_menu->pin_menu.unlock_pin) == 0) {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGI(TAG, "PIN accepted");
			#endif
			
			// Hide pin prompt
			lv_obj_add_flag(settings_menu->pin_menu.pin_container, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(settings_menu->pin_menu.lbl_ins, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(settings_menu->pin_menu.lbl_back, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(settings_menu->pin_menu.lbl_attempts, LV_OBJ_FLAG_HIDDEN);
			
			// Reset
			num_filled = num_boxes = 0;
			pin_attempts = 0;
			lcd_settings_pin_attempts_nvs_save(); // Saves pin_attempts global
			memset(input_pin, 0, sizeof(input_pin));
			lcd_settings_rebuild_pin_boxes(settings_menu->pin_menu.pin_container, unlock_labels,
				input_pin, &num_boxes, num_filled);
				
			pin_signing_in = false;
			
			// Update options text
			settings_menu->options[0] = SETTINGS_REMOVE_LOCK_TXT;
			lv_list_set_button_text(settings_menu->main_list, settings_menu->btns[0], settings_menu->options[0]);
			
			// Won't prompt again unless power off
			settings_menu->pin_menu.prompt_pin = false;
			
			// If going to selection page
			if (pin_to_selection_page) {
				// Hide right
				lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

				#ifndef POLYCAST5_PERSIST_SELECTION_INDEX
				ui_menu->index = SELECTION_DEFAULT_IDX; // Default start
				lcd_selection_sync_labels(ui_menu); // Sync menu from here
				#endif

				// Show selection page
				lcd_unhide_selection_widgets(ui_menu);
				
				// Switch execution to selection page
				ui_menu->page = SELECTION_PAGE;
			}
			// Else going to hotkey page
			else {
				// Default index
				hotkey_menu.index = (MAX_HOTKEY_OPTIONS - 1);
				lcd_hotkey_update_menu(&hotkey_menu);
		
				// Switch to hotkey page
				ui_menu->page = HOTKEY_PAGE;
			}
		}
		else {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGI(TAG, "PIN denied");
			#endif
			
			// RGB indicator
			uint8_t rgb_state = RGB_BLINK_RED;
			xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
			
			// Outline red
			for (int i = 0; i < num_boxes; ++i) {
				lv_obj_set_style_border_color(lv_obj_get_parent(unlock_labels[i]), lv_palette_main(LV_PALETTE_RED), 0);
			}
			
			pin_attempts++;
			lcd_settings_pin_attempts_nvs_save(); // Saves pin_attempts global
			
			// Build and set attempts string
			char buf[18];
			snprintf(buf, sizeof(buf), "WRONG: %" PRIu32, pin_attempts);
			lv_label_set_text(settings_menu->pin_menu.lbl_attempts, buf);
			
			// Show attempts
			lv_obj_remove_flag(settings_menu->pin_menu.lbl_attempts, LV_OBJ_FLAG_HIDDEN);
		}
	}
	// Power off
	else if (ui_btns->pwr_btn == 1) {
		// Hide pin prompt
		lv_obj_add_flag(settings_menu->pin_menu.pin_container, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(settings_menu->pin_menu.lbl_ins, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(settings_menu->pin_menu.lbl_back, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(settings_menu->pin_menu.lbl_attempts, LV_OBJ_FLAG_HIDDEN);
		
		// Reset
		num_filled = num_boxes = 0;
		memset(input_pin, 0, sizeof(input_pin));
		lcd_settings_rebuild_pin_boxes(settings_menu->pin_menu.pin_container, unlock_labels,
			input_pin, &num_boxes, num_filled);
		
		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
	}
}

void lcd_hotkey_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, hotkey_menu_t *hotkey_menu)
{
	// Exit
	if (ui_btns->right_btn == 1 && hotkey_menu->index == (MAX_HOTKEY_OPTIONS - 1)) {
		// Hide hotkey page
		lv_obj_add_flag(hotkey_menu->cont, LV_OBJ_FLAG_HIDDEN);
		
		// Go back
		lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
	}
	// Select option
	else if (ui_btns->select_btn == 1) {
		// Hide hotkey page
		lv_obj_add_flag(hotkey_menu->cont, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = HOTKEY_OPTION_PAGE;
	}
	// Scroll right
	else if (ui_btns->right_btn == 1) {
		// Update selection
		hotkey_menu->index++;
		lcd_hotkey_update_menu(hotkey_menu);
	}
	// Scroll left
	else if (ui_btns->left_btn == 1) {
		// Update selection
		hotkey_menu->index--;
		lcd_hotkey_update_menu(hotkey_menu);
	}
	// Scroll up
	else if (ui_btns->up_btn == 1) {
		// Update selection
		if (hotkey_menu->index > 2) {
			hotkey_menu->index -= 3;
		}
		else if (hotkey_menu->index < 3) {
			hotkey_menu->index += 3;
		}
		lcd_hotkey_update_menu(hotkey_menu);
	}
	// Scroll down
	else if (ui_btns->down_btn == 1) {
		// Update selection
		if (hotkey_menu->index < 3) {
			hotkey_menu->index += 3;
		}
		else if (hotkey_menu->index > 2) {
			hotkey_menu->index -= 3;
		}
		lcd_hotkey_update_menu(hotkey_menu);
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {		
		// Hide hotkey page
		lv_obj_add_flag(hotkey_menu->cont, LV_OBJ_FLAG_HIDDEN);
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_selection_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, ir_menu_t *ir_menu, lora_menu_t *lora_menu,
		espnow_menu_t *espnow_menu, wifi_menu_t *wifi_menu, tools_menu_t *tools_menu, settings_menu_t *settings_menu,
		bluetooth_menu_t *bluetooth_menu, gpio_menu_t *gpio_menu) 
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
		// Switch to the selected page
		lcd_selection_btn_pressed(ui_menu, ir_menu, lora_menu, espnow_menu, wifi_menu, tools_menu, settings_menu, bluetooth_menu, gpio_menu);
	}
	// Go back
	else if (ui_btns->left_btn == 1) {
		// Reset long semaphores to avoid false triggers
		xQueueReset(xSelectButtonLongSemaphore);
		xQueueReset(xHomeButtonLongSemaphore);
		xQueueReset(xUpButtonLongSemaphore);
		xQueueReset(xDownButtonLongSemaphore);
		xQueueReset(xLeftButtonLongSemaphore);
		xQueueReset(xRightButtonLongSemaphore);

		// Hide selection labels
		lv_obj_add_flag(ui_menu->btn_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->scroll_bar, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->scroll_track, LV_OBJ_FLAG_HIDDEN);
				
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
		lv_obj_add_flag(ui_menu->scroll_bar, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->scroll_track, LV_OBJ_FLAG_HIDDEN);
				
		lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
	}
	// Power off
	else if (ui_btns->pwr_btn == 1) {
		// Hide selection labels
		lv_obj_add_flag(ui_menu->btn_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_mid, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->lbl_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->scroll_bar, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->scroll_track, LV_OBJ_FLAG_HIDDEN);
		
		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
	}

	if (scrolling_menu) {
		if (scrolling_up) {
			ui_menu->index = (ui_menu->index + 1) % ui_menu->size;

			#ifdef POLYCAST5_PERSIST_SELECTION_INDEX
			lcd_selection_index_nvs_save(ui_menu); // Save the index
			#endif

			const char *next_bottom = ui_menu->options[(ui_menu->index + 1) % ui_menu->size];
			lcd_scroll_anim(ui_menu, next_bottom, scrolling_up, SCROLL_SPEED);
			
			// Update thumb y (reversed direction, precise double, no jump)
			int max_y = SELECTION_SCROLLBAR_CONT_HEIGHT - SELECTION_SCROLLBAR_THUMB_HEIGHT;
			double fraction = (double)ui_menu->index / (ui_menu->size - 1); // Double for smooth/no jump
			int y = (int)(fraction * max_y + 0.5); // Round for consistency
			lv_obj_set_y(ui_menu->scroll_bar, y + SELECTION_SCROLLBAR_OFFSET);
		}
		else {
			ui_menu->index = (ui_menu->index + ui_menu->size - 1) % ui_menu->size;

			#ifdef POLYCAST5_PERSIST_SELECTION_INDEX
			lcd_selection_index_nvs_save(ui_menu); // Save the index
			#endif

			const char *next_top = ui_menu->options[(ui_menu->index + ui_menu->size - 1) % ui_menu->size];
			lcd_scroll_anim(ui_menu, next_top, scrolling_up, SCROLL_SPEED);
			
			// Update thumb y (reversed direction, precise double, no jump)
			int max_y = SELECTION_SCROLLBAR_CONT_HEIGHT - SELECTION_SCROLLBAR_THUMB_HEIGHT;
			double fraction = (double)ui_menu->index / (ui_menu->size - 1); // Double for smooth/no jump
			int y = (int)(fraction * max_y + 0.5); // Round for consistency
			lv_obj_set_y(ui_menu->scroll_bar, y + SELECTION_SCROLLBAR_OFFSET);
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

	// Reset long semaphores to avoid false triggers
	xQueueReset(xSelectButtonLongSemaphore);
	xQueueReset(xHomeButtonLongSemaphore);
	xQueueReset(xUpButtonLongSemaphore);
	xQueueReset(xDownButtonLongSemaphore);
	xQueueReset(xLeftButtonLongSemaphore);
	xQueueReset(xRightButtonLongSemaphore);
	
	// Transition to home
	if (home) {		
		start_animation();

		ui_menu->page = HOME_PAGE;
	}
	// Transition to sleep
	else {
		gpio_set_level(ST7789_LEDA_PIN, 0); // BL low so user doesn't see redraw
	
		start_animation();

		ui_menu->page = HOME_PAGE;
		
		lv_refr_now(disp); // Force redraw of homescreen before sleeping
			
		go_to_sleep = true;
	}
}

void lcd_infrared_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, ir_menu_t *ir_menu) 
{	
	static bool initalized = false;
	
	// Do once
	if (!initalized) {
		xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
		lcd_ir_build_current_menu(ir_menu, ir_current_remote);
		xSemaphoreGive(xInfraredDataMutex); // Release IR
		lv_obj_remove_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);	
		
		initalized = true;
	}
	
	// Edit selected
	if (ui_btns->select_btn == 1 && ir_menu->index == 1) {
		lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN); // Hide IR menu
		
		initalized = false; // Reset bool
		
		ui_menu->page = INFRARED_REMOTE_EDIT_PAGE;
	}
	// Add new signal selected
	else if (ui_btns->select_btn == 1 && ir_menu->index == 2) {
		// Abort if we've reached the maximum number of peers
		if (ir_menu->size >= MAX_IR_OPTIONS) {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Max IR menu options reached");
			#endif
			
			// Hide IR menu
			lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// User notice
			lv_obj_t *lbl_rst = lv_label_create(ACTIVE_SCR);
			lcd_format_label(lbl_rst, "Max signals added!", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
			lv_timer_handler();
			vTaskDelay(pdMS_TO_TICKS(1000));
			lv_obj_delete(lbl_rst);
			lcd_clear_pending_inputs = true;
			
			// Show IR menu
			lv_obj_remove_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			return;
		}
		// Else we're good to add another
		else {
			lcd_ir_save_new_signal(ui_menu, ir_menu);
			
			initalized = false;
		}
	}
	// Selected specific signal
	else if (ui_btns->select_btn == 1 && ir_menu->index >= 3) {
		// If recording command as hotkey
		if (!lv_obj_has_flag(ui_menu->lbl_hotkey_icon, LV_OBJ_FLAG_HIDDEN)) {
			// Zero out at start
			memset(&hotkey_cmd.ir_cmd[hotkey_cmd.active_idx], 0, sizeof(ir_cmd_t));
			
			// Save into hotkey struct under selected "Hotx"
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			hotkey_cmd.ir_cmd[hotkey_cmd.active_idx].index = ir_menu->index; // Save signal index
			hotkey_cmd.ir_cmd[hotkey_cmd.active_idx].current_remote = ir_current_remote; // Save remote used
			xSemaphoreGive(xInfraredDataMutex); // Release IR
			
			// Flag that command exists
			hotkey_cmd.has_ir[hotkey_cmd.active_idx] = true;
			// Remove others
			hotkey_cmd.has_espnow[hotkey_cmd.active_idx] = false;
			hotkey_cmd.has_lora[hotkey_cmd.active_idx] = false;
			
			// Hide hotkey icon
			lv_obj_add_flag(ui_menu->lbl_hotkey_icon, LV_OBJ_FLAG_HIDDEN);
			
			// Persist to NVS
			lcd_hotkey_nvs_save(&hotkey_cmd);
		}
		
		// Transmit signal at index
		xQueueSend(xInfraredSignalToTxQueue, &ir_menu->index, portMAX_DELAY);
		
		// RGB indicator
		uint8_t rgb_state = RGB_BLINK_PURPLE;
		xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
	}
	// Back selected
	else if (ui_btns->down_btn == 1) {
		xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
		
		// If at first remote, go back
		if (ir_current_remote == 0) {
			// Hide IR menu
			lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
					
			// Hide right arrow
			lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			
			// Show selection labels
			lcd_unhide_selection_widgets(ui_menu);
			
			ui_menu->page = SELECTION_PAGE;
		}
		// Else go back a remote
		else {
			ir_current_remote--;
			
			// Rebuild menu with new remote
			lcd_ir_build_current_menu(ir_menu, ir_current_remote);
		}
		
		xSemaphoreGive(xInfraredDataMutex); // Release IR
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Hide IR menu
		lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
				
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
	// Switch to next remote
	else if (ui_btns->up_btn == 1) {
		// Increment current_remote with wrap
		xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
		ir_current_remote = (ir_current_remote + 1) % num_remotes;
		
		// Rebuild menu with new remote
		lcd_ir_build_current_menu(ir_menu, ir_current_remote);
		xSemaphoreGive(xInfraredDataMutex); // Release IR
		lcd_ir_update_menu(ir_menu);
	}
	// Scroll down
	else if (ui_btns->right_btn == 1) {
		// Update selection
		ir_menu->index++;
		lcd_ir_update_menu(ir_menu);
	}
	// Scroll up
	else if (ui_btns->left_btn == 1) {
		// Update selection
		ir_menu->index--;
		lcd_ir_update_menu(ir_menu);
	}
}

void lcd_lora_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu) 
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
		// Abort if we've reached the maximum number of peers
		// Compare with total user plugs: Total size - "Add PolyPlug" + 1 (since not yet size++) -> just lora_menu->size
		if (lora_menu->size >= MAX_LORA_OPTIONS) {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Max LoRa PolyPlug entries reached");
			#endif
			
			// Hide LoRa menu
			lv_obj_add_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// User notice
			lv_obj_t *lbl_rst = lv_label_create(ACTIVE_SCR);
			lcd_format_label(lbl_rst, "Max Plugs added!", user_secondary_color,
					 &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, 0);
			lv_timer_handler();
			vTaskDelay(pdMS_TO_TICKS(1000));
			lv_obj_delete(lbl_rst);
			lcd_clear_pending_inputs = true;
			
			// Show LoRa menu
			lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			return;
		}
		// Else we're good to add another
		else {
			xSemaphoreGive(xWifiDisconnectSemaphore); // Disconnect from Wi-Fi if connected
			
			// Show right arrow
			lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			
			// Hide LoRa menu
			lv_obj_add_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Switch to add page
			ui_menu->page = LORA_ADD_PAGE;
		}
	}
	// PolyPlug selected
	else if (ui_btns->select_btn == 1) {
		// Hide LoRa menu
		lv_obj_add_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Show submenu
		lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

		ui_menu->page = LORA_SUBPAGE;
	
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide LoRa menu
		lv_obj_add_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		lcd_unhide_selection_widgets(ui_menu);
		
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

void lcd_espnow_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
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
		// Abort if we've reached the maximum number of peers
		if (espnow_menu->size >= MAX_ESPNOW_OPTIONS) {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Max ESP-NOW entries reached");
			#endif
			
			// Hide ESP-NOW menu
			lv_obj_add_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// User notice
			lv_obj_t *lbl_rst = lv_label_create(ACTIVE_SCR);
			lcd_format_label(lbl_rst, "Max ESP32s added!", user_secondary_color,
					&lv_font_montserrat_20, LV_ALIGN_CENTER, 0, 0);
			lv_timer_handler();
			vTaskDelay(pdMS_TO_TICKS(1000));
			lv_obj_delete(lbl_rst);
			lcd_clear_pending_inputs = true;
			
			// Show ESP-NOW menu
			lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			return;
		}
		// Else we're good to add another
		else {
			// Hide ESP-NOW menu
			lv_obj_add_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Reset static
			do_once = false;
			
			// Show right arrow
			lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			
			ui_menu->page = ESPNOW_RX_MAC_PAGE;
		}
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
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		ui_menu->page = ESPNOW_OPTION_PAGE;
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide ESP-NOW menu
		lv_obj_add_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		lcd_unhide_selection_widgets(ui_menu);
		
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

void lcd_wifi_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
	// Statics
	static bool do_once = false;
	static lv_obj_t *lbl_conf;
	static wifi_ping_t wifi_ping = {0};
	static lv_obj_t *gateway_ping_lbl = NULL;
	static lv_obj_t *dns_ping_lbl = NULL;
	
	// Only execute once
	if (!do_once) {
		// Show Wi-Fi list
		lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Reset ping struct
		memset(&wifi_ping, 0, sizeof(wifi_ping_t));

		// Create ping labels
		gateway_ping_lbl = lv_label_create(ACTIVE_SCR);
		lcd_format_label(gateway_ping_lbl, "", user_secondary_color,
				&lv_font_montserrat_14, LV_ALIGN_BOTTOM_LEFT, 2, 2);
		dns_ping_lbl = lv_label_create(ACTIVE_SCR);
		lcd_format_label(dns_ping_lbl, "", user_secondary_color,
				&lv_font_montserrat_14, LV_ALIGN_BOTTOM_RIGHT, -2, 2);
		
		do_once = true;
	}

	// Check for network ping results
	if (xQueueReceive(xWifiPingQueue, &wifi_ping, 0) == pdTRUE) {
		// Set text
		lv_label_set_text_fmt(gateway_ping_lbl, "Router: %" PRId32 " ms", wifi_ping.rtt_gateway);
		lv_label_set_text_fmt(dns_ping_lbl, "DNS: %" PRId32 " ms", wifi_ping.rtt_dns);

		// Realign labels
		lv_obj_align(gateway_ping_lbl, LV_ALIGN_BOTTOM_LEFT, 2, 2);
		lv_obj_align(dns_ping_lbl, LV_ALIGN_BOTTOM_RIGHT, -2, 2);
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
		
		lcd_wifi_connected = true;
	}
	if (xSemaphoreTake(xWifiNetworkDisconnectedSemaphore, 0) == pdTRUE) {
		lv_obj_t *lbl = lv_obj_get_child(wifi_menu->btns[0], 0);
		lv_label_set_text(lbl, "Connect to network");
		
		lcd_wifi_connected = false;
	}
	
	// If update is available
	if (xSemaphoreTake(xWifiOtaAvailableSemaphore, 0) == pdTRUE) {
		// Hide Wi-Fi menu
		lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Delete ping labels
		lv_obj_delete(gateway_ping_lbl);
		lv_obj_delete(dns_ping_lbl);

		// Reset statics
		do_once = false;
		gateway_ping_lbl = dns_ping_lbl = NULL;

		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = WIFI_OTA_CONFIRM_PAGE;
	}
	// Else normal Wi-Fi page
	else {
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
			if (lcd_wifi_connected) {
				xSemaphoreGive(xWifiDisconnectSemaphore);
			}
			// Already disconnected
			else {
				// Hide Wi-Fi menu
				lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
				
				// Show scan menu
				lv_obj_remove_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);

				// Delete ping labels
				lv_obj_delete(gateway_ping_lbl);
				lv_obj_delete(dns_ping_lbl);

				// Reset statics
				do_once = false;
				gateway_ping_lbl = dns_ping_lbl = NULL;
				
				ui_menu->page = WIFI_SCAN_PAGE;
			}
		}
		// Monitor packets
		else if (ui_btns->select_btn == 1 && wifi_menu->index == 1) {
			// Hide Wi-Fi menu
			lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
				
			// Show scan menu
			lv_obj_remove_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Delete ping labels
			lv_obj_delete(gateway_ping_lbl);
			lv_obj_delete(dns_ping_lbl);

			// Reset statics
			do_once = false;
			gateway_ping_lbl = dns_ping_lbl = NULL;
			
			monitoring_packets = true;
			
			ui_menu->page = WIFI_SCAN_PAGE;
		}
		// Sync with PolyPlug
		else if (ui_btns->select_btn == 1 && wifi_menu->index == 2) {
			// Hide Wi-Fi menu
			lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			if (lcd_wifi_connected) {
				// Delete ping labels
				lv_obj_delete(gateway_ping_lbl);
				lv_obj_delete(dns_ping_lbl);

				// Reset statics
				do_once = false;
				gateway_ping_lbl = dns_ping_lbl = NULL;
				
				// Show right arrow
				lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
				
				ui_menu->page = WIFI_SYNC_PAGE;
			}
			else {
				lbl_conf = lv_label_create(ACTIVE_SCR);
				
				lcd_format_label(lbl_conf, "Please connect to\n  a network first!", user_secondary_color,
							&lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
				
				lv_timer_handler();
				vTaskDelay(pdMS_TO_TICKS(1000));
				
				lv_obj_delete(lbl_conf);
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
			
			if (lcd_wifi_connected) {
				// Delete ping labels
				lv_obj_delete(gateway_ping_lbl);
				lv_obj_delete(dns_ping_lbl);

				// Reset statics
				do_once = false;
				gateway_ping_lbl = dns_ping_lbl = NULL;
				
				// Hide top and bot arrows
				lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
				lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
				
				// Show Wi-Fi send page
				lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send_ins, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send_cmd, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send_box, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_edit, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(wifi_menu->wifi_submenu.arrow_top, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(wifi_menu->wifi_submenu.arrow_bot, LV_OBJ_FLAG_HIDDEN);
				
				// Show right arrow
				lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
				
				ui_menu->page = WIFI_SEND_PAGE;
			}
			else {
				lbl_conf = lv_label_create(ACTIVE_SCR);
				
				lcd_format_label(lbl_conf, "Please connect to\n  a network first!", user_secondary_color,
							&lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
				
				lv_timer_handler();
				vTaskDelay(pdMS_TO_TICKS(1000));
				
				lv_obj_delete(lbl_conf);
				lbl_conf = NULL;
				
				lcd_clear_pending_inputs = true;
				
				// Show Wi-Fi menu
				lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			}
		}
		// Ping network
		else if (ui_btns->right_btn == 1) {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGI(TAG, "Requesting Wi-Fi ping");
			#endif

			xSemaphoreGive(xWifiPingSemaphore);
		}
		// Back selected
		else if (ui_btns->left_btn == 1) {
			// Hide Wi-Fi menu
			lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Show selection labels
			lcd_unhide_selection_widgets(ui_menu);
			
			// Delete ping labels
			lv_obj_delete(gateway_ping_lbl);
			lv_obj_delete(dns_ping_lbl);

			// Reset statics
			do_once = false;
			gateway_ping_lbl = dns_ping_lbl = NULL;
			
			// Switch pages
			ui_menu->page = SELECTION_PAGE;
		}
		// Home or power off selected
		else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
			// Hide Wi-Fi menu
			lv_obj_add_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Delete ping labels
			lv_obj_delete(gateway_ping_lbl);
			lv_obj_delete(dns_ping_lbl);

			// Reset statics
			do_once = false;
			gateway_ping_lbl = dns_ping_lbl = NULL;
			
			lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
		}
	}
}

void lcd_tools_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
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
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_DICE_PAGE;
	}
	// Tetris selected
	else if (ui_btns->select_btn == 1 && tools_menu->index == 2) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_TETRIS_PAGE;
	}
	// Random number generator selected
	else if (ui_btns->select_btn == 1 && tools_menu->index == 3) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_NUM_GEN_PAGE;
	}
	// Read the docs selected
	else if (ui_btns->select_btn == 1 && tools_menu->index == 4) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Hide up/down arrow
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_DOCS_PAGE;
	}
	// BTC address selected
	else if (ui_btns->select_btn == 1 && tools_menu->index == 5) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;

		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_BTC_ADDR_PAGE;
	}
	// Pomodoro timer selected
	else if (ui_btns->select_btn == 1 && tools_menu->index == 6) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_POMODORO_PAGE;
	}
	// SRS memory assist selected
	else if (ui_btns->select_btn == 1 && tools_menu->index == 7) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_HOW_SRS_PAGE;
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		lcd_unhide_selection_widgets(ui_menu);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SELECTION_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Hide tools menu
		lv_obj_add_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_settings_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
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
	// Set unlock pin selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 0) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Removing set pin
		if (settings_menu->pin_menu.pin_set) {
			settings_menu->options[0] = SETTINGS_SET_LOCK_TXT;
			settings_menu->pin_menu.pin_set = false;
			lv_list_set_button_text(settings_menu->main_list, settings_menu->btns[0], settings_menu->options[0]);
			
			// Update NVS
			memset(settings_menu->pin_menu.unlock_pin, 0, sizeof(settings_menu->pin_menu.unlock_pin));
			lcd_settings_pin_nvs_save(settings_menu);
		}
		// Setting pin
		else {
			// Show right arrow
			lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
						
			// Switch pages
			ui_menu->page = SETTINGS_PIN_PAGE;
		}
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
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = SETTINGS_COLORS_PAGE;
	}
	// Adjust haptics selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 2) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SETTINGS_HAPTIC_PAGE;
	}
	// Adjust sleep timer selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 3) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SETTINGS_SLEEP_TIMER_PAGE;
	}
	// Adjust RGB LED selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 4) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = SETTINGS_RGB_LED_PAGE;
	}
	// Adjust LCD selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 5) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SETTINGS_LCD_PAGE;
	}
	// Tips and tricks selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 6) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SETTINGS_HELP_PAGE;
	}
	// System check selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 7) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SETTINGS_SYSTEM_PAGE;
	}
	// Reboot selected
	else if (ui_btns->select_btn == 1 && settings_menu->index == 8) {
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
	else if (ui_btns->select_btn == 1 && settings_menu->index == 9) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Hide arrows
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = SETTINGS_FACTORY_RST_PAGE;
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		lcd_unhide_selection_widgets(ui_menu);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SELECTION_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Hide settings menu
		lv_obj_add_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_bluetooth_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	// Statics
	static bool do_once = false;
	
	// Only execute once
	if (!do_once) {
		// Show bluetooth list
		lv_obj_remove_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		do_once = true;
	}
	
	// Up button pressed
	if (ui_btns->up_btn == 1) {
		// Update selection
		bluetooth_menu->index--;
		lcd_bluetooth_update_menu(bluetooth_menu);
	}
	// Down button pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		bluetooth_menu->index++;
		lcd_bluetooth_update_menu(bluetooth_menu);
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide bluetooth menu
		lv_obj_add_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		lcd_unhide_selection_widgets(ui_menu);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SELECTION_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Hide bluetooth menu
		lv_obj_add_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
	// How it works selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->index == 0) {
		// Hide bluetooth menu
		lv_obj_add_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = BLUETOOTH_HOW_PAGE;
	}
	// Auto keyboard selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->index == 1) {
		// Hide bluetooth menu
		lv_obj_add_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Reset static
		do_once = false;

		// Show bluetooth keyboard menu
		lv_obj_remove_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = BLUETOOTH_KEYBOARD_PAGE;
	}
	// AI keyboard selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->index == 2) {
		xQueueSend(xAiCmdQueue, "please open notepad and write some pseudocode explaining how dijkstras algorithm works in detail", pdMS_TO_TICKS(100));
	}
	// Media controller selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->index == 3) {
		// Hide bluetooth menu
		lv_obj_add_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Hide arrows
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);	

		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = BLUETOOTH_MEDIA_CLASSIC_PAGE;
	}
	// Page scroll selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->index == 4) {
		// Hide bluetooth menu
		lv_obj_add_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Hide arrows
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);	

		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = BLUETOOTH_MEDIA_SCROLL_PAGE;
	}
	// Presentation mode selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->index == 5) {
		// Hide bluetooth menu
		lv_obj_add_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Hide arrows
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);	

		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = BLUETOOTH_MEDIA_PRESENTATION_PAGE;
	}
	// Camera clicker selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->index == 6) {
		// Hide bluetooth menu
		lv_obj_add_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Hide arrows
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);	

		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = BLUETOOTH_MEDIA_CAMERA_PAGE;
	}
	// Socials scroller selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->index == 7) {
		// Hide bluetooth menu
		lv_obj_add_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Hide arrows
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);	

		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = BLUETOOTH_MEDIA_SOCIALS_PAGE;
	}
	// Known devices selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->index == 8) {
		// Hide bluetooth menu
		lv_obj_add_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = BLUETOOTH_KNOWN_DEVICES_PAGE;
	}
}

void lcd_gpio_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
	// Statics
	static bool do_once = false;
	
	// Only execute once
	if (!do_once) {
		// Show bluetooth list
		lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		do_once = true;
	}
	
	// Up button pressed
	if (ui_btns->up_btn == 1) {
		// Update selection
		gpio_menu->index--;
		lcd_gpio_update_menu(gpio_menu);
	}
	// Down button pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		gpio_menu->index++;
		lcd_gpio_update_menu(gpio_menu);
	}
	// How it works selected
	else if (ui_btns->select_btn == 1 && gpio_menu->index == 0) {
		// Hide GPIO menu
		lv_obj_add_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = GPIO_HOW_PAGE;
	}
	// Terminal selected
	else if (ui_btns->select_btn == 1 && gpio_menu->index == 1) {
		// Hide GPIO menu
		lv_obj_add_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = GPIO_TERMINAL_PAGE;
	}
	// I2C scanner selected
	else if (ui_btns->select_btn == 1 && gpio_menu->index == 2) {
		// Hide GPIO menu
		lv_obj_add_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = GPIO_SCANNER_PAGE;
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Hide GPIO menu
		lv_obj_add_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show selection labels
		lcd_unhide_selection_widgets(ui_menu);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = SELECTION_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Hide GPIO menu
		lv_obj_add_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}


