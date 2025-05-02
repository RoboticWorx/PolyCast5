#include "lcd_funcs.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"

#define HOR_RES      240
#define VER_RES      135
#define DRAW_LINES   20
#define FLUSH_CHUNK  2

static TFT_t         tft;
static lv_display_t *disp;       /* LVGL display handle             */

static void st7789_flush_cb(lv_display_t *d,
                            const lv_area_t *area,
                            uint8_t *px_map)
{
    const uint16_t *color_ptr = (const uint16_t *)px_map;
    int16_t x1 = area->x1, x2 = area->x2;
    int16_t y1 = area->y1, y2 = area->y2;
    int16_t width     = x2 - x1 + 1;
    int16_t remaining = y2 - y1 + 1;
    int16_t y         = y1;

    while (remaining > 0) {
        int16_t chunk = remaining > FLUSH_CHUNK ? FLUSH_CHUNK : remaining;

        /* 1) Window: columns = [x1..x2], rows = [y..y+chunk−1] */
        spi_master_write_command(&tft, 0x2A);
        spi_master_write_addr(&tft,
                              x1 + tft._offsetx,
                              x2 + tft._offsetx);
        spi_master_write_command(&tft, 0x2B);
        spi_master_write_addr(&tft,
                              y + tft._offsety,
                              (y + chunk - 1) + tft._offsety);

        /* 2) Push chunk-worth of pixels */
        spi_master_write_command(&tft, 0x2C);
        spi_master_write_colors(&tft,
                                color_ptr,
                                (uint32_t)width * chunk);

        /* advance */
        color_ptr += (uint32_t)width * chunk;
        y         += chunk;
        remaining -= chunk;
    }

    /* 3) Tell LVGL we’re done */
    lv_disp_flush_ready(d);
}

static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

void lcd_init_driver(void)
{
    /* 1) Panel power-up delay (50 ms) */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2) SPI bus + device init */
    spi_master_init(&tft,
                    SPI_MOSI_PIN, SPI_SCLK_PIN,
                    ST7789_CS_PIN, ST7789_DC_PIN,
                    ST7789_RST_PIN, ST7789_LEDK_PIN);
    spi_clock_speed(40 * 1000 * 1000);  // 40 MHz

    /* 3) ST7789 panel init (nopnop2002 driver) */
    lcdInit(&tft, HOR_RES, VER_RES, 0, 0);

    /* 4) Hardware‐side 90° rotation via MADCTL */
    spi_master_write_command(&tft, 0x36);      // MADCTL
    spi_master_write_data_byte(&tft, 0x60);    // MX=1, MV=1 → 90° CW

    /* 5) Offsets for your specific 135×240 module in landscape */
    tft._offsetx = 40;
    tft._offsety = 53;
}

void lcd_lvgl_init(void)
{
    /* 1) LVGL library init */
    lv_init();

    /* 2) Draw‐buffer: HOR_RES × DRAW_LINES lines */
    /* Allocate space for 20 lines of 240 px each (≈9.6 kB), DMA-capable in DRAM */
    static DRAM_ATTR lv_color_t buf[HOR_RES * DRAW_LINES * 2]
        __attribute__((aligned(4)));
    static lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf,
                     HOR_RES,      // 240 px logical width
                     DRAW_LINES,   // 20 lines
                     LV_COLOR_FORMAT_NATIVE,
                     0, buf, sizeof(buf));
                     

    /* 3) Create the “display” object */
    disp = lv_display_create(HOR_RES, VER_RES);  // 240×135 logical
    lv_display_set_flush_cb(disp, st7789_flush_cb);
    lv_display_set_draw_buffers(disp, &draw_buf, NULL);

    /* 4) 1 ms tick timer feeding lv_tick_inc() */
    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_cb,
        .name     = "lv_tick"
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);
}

lv_display_t *lcd_get_display(void)
{
    return disp;
}
