#include "polycast5_macros.h"

#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"

#include "esp_log.h"

#include "core/lv_obj.h"
#include "core/lv_obj_pos.h"
#include "font/lv_font.h"
#include "misc/lv_area.h"
#include "widgets/label/lv_label.h"

#include "wifi_csi.h"
#include "wifi_task.h"

#include "lcd_csi.h"
#include "lcd_tools.h"
#include "lcd_utils.h"
#include "polycast5_fonts.h"

#define TAG "LCD_CSI"

// Channels offered on the sensing page. Non-overlapping 2.4 GHz only for now: 5 GHz CSI on this
// chip still needs the validation harness run against it before it can be trusted.
static const uint8_t csi_channels[] = {1, 6, 11};
#define CSI_CHANNEL_COUNT (sizeof(csi_channels) / sizeof(csi_channels[0]))

static int csi_channel_idx = 0;

/**
 * @brief Post a session request to wifi_task
 */
static void lcd_csi_send_cmd(bool start)
{
    wifi_csi_cmd_t cmd = {0};

    cmd.start = start ? 1 : 0;
    cmd.source = WIFI_CSI_SRC_PROMISC; // No credentials needed, so the page always works
    cmd.consumers = WIFI_CSI_CONSUMER_LOCAL;
    cmd.channel = csi_channels[csi_channel_idx];
    cmd.sound_interval_ms = 20;

    if (xWifiCsiCmdQueue && xQueueSend(xWifiCsiCmdQueue, &cmd, 0) != pdPASS) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "Failed: xWifiCsiCmdQueue");
#endif
    }
}

void lcd_csi_intro_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
    #define CSI_INTRO_Y_OFFSET 40

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *instr_lbl = NULL;

    if (!init) {
        // Create a scrollable container for the instructions
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

        // Title label
        title_lbl = lv_label_create(cont);
        lv_label_set_text(title_lbl, "Motion Sense:");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Instructions label
        instr_lbl = lv_label_create(cont);
        lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(instr_lbl, lv_pct(100));
        lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
        lv_obj_align_to(instr_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

        const char *instr_text = "Press RIGHT to start.\n\n"
                                 "This reads the Wi-Fi already in the air around you. People moving "
                                 "nearby disturb those radio reflections, and that shows up as "
                                 "motion without any camera.\n\n"
                                 "Put the device down and leave it still. It measures the room "
                                 "against itself, so if it moves, the reading is void.\n\n"
                                 "Heads up: this keeps the radio receiving nonstop, so it draws "
                                 "far more power than normal. Use it plugged in for long runs.";

        lv_label_set_text(instr_lbl, instr_text);

        lv_timer_handler();

        init = true;
    }

    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, CSI_INTRO_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -CSI_INTRO_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->right_btn == 1) { // Start sensing
        // Hide right arrow, the sensing page uses up/down for channel instead
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;

        // Switch pages
        ui_menu->page = TOOLS_CSI_LOCAL_PAGE;
    } else if (ui_btns->left_btn) { // Go back
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;

        // Restore the selection arrows the router hid on the way in
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);

        // Show tools menu
        lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Switch back
        ui_menu->page = TOOLS_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_csi_local_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *state_lbl = NULL;
    static lv_obj_t *rate_lbl = NULL;
    static lv_obj_t *health_lbl = NULL;
    static lv_obj_t *chan_lbl = NULL;

    if (!init) {
        // Drop anything the previous visit left behind
        if (xWifiCsiStatusQueue) {
            xQueueReset(xWifiCsiStatusQueue);
        }

        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

        // Big state readout
        state_lbl = lv_label_create(cont);
        lcd_format_label(state_lbl, "STARTING", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 0);

        // Measured capture rate and frame geometry
        rate_lbl = lv_label_create(cont);
        lcd_format_label(rate_lbl, "-- Hz", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_LEFT_MID, 0, 0);

        // Capture health, so a bad session is visible rather than silent
        health_lbl = lv_label_create(cont);
        lcd_format_label(health_lbl, "", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        chan_lbl = lv_label_create(cont);
        lcd_format_label(chan_lbl, "", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, 0, 0);

        char chan_buf[16];
        snprintf(chan_buf, sizeof(chan_buf), "CH %u", csi_channels[csi_channel_idx]);
        lv_label_set_text(chan_lbl, chan_buf);

        lcd_csi_send_cmd(true);

        init = true;
    }

    // Latest status wins, the producer overwrites a depth-one queue
    wifi_csi_status_t st;

    if (xWifiCsiStatusQueue && xQueueReceive(xWifiCsiStatusQueue, &st, 0) == pdTRUE) {
        const char *state_txt = "WAITING";

        switch (st.state) {
            case WIFI_CSI_STATE_QUIET:   state_txt = "QUIET"; break;
            case WIFI_CSI_STATE_MOTION:  state_txt = "MOTION"; break;
            case WIFI_CSI_STATE_INVALID: state_txt = "NO SIGNAL"; break;
            case WIFI_CSI_STATE_MOVED:   state_txt = "MOVED"; break;
            case WIFI_CSI_STATE_EXPIRED: state_txt = "ENDED"; break;
            default: break;
        }

        lv_label_set_text(state_lbl, state_txt);

        char rate_buf[40];
        snprintf(rate_buf, sizeof(rate_buf), "%u Hz  %u sc  %d dBm",
                st.stats.frames_per_sec, st.stats.n_subcarriers, st.stats.rssi);
        lv_label_set_text(rate_lbl, rate_buf);

        char health_buf[64];
        snprintf(health_buf, sizeof(health_buf), "rx %"PRIu32" drop %"PRIu32" inv %"PRIu32" mis %"PRIu32,
                st.stats.captured, st.stats.dropped, st.stats.invalid, st.stats.mismatch);
        lv_label_set_text(health_lbl, health_buf);
    }

    // Up/down retunes: the capture has to restart because the baseline is channel-specific
    if (ui_btns->up_btn == 1 || ui_btns->down_btn == 1) {
        csi_channel_idx += (ui_btns->up_btn == 1) ? -1 : 1;

        if (csi_channel_idx < 0) {
            csi_channel_idx = CSI_CHANNEL_COUNT - 1;
        } else if (csi_channel_idx >= (int)CSI_CHANNEL_COUNT) {
            csi_channel_idx = 0;
        }

        // One command only: the queue is depth one and non-blocking, so a stop followed by a
        // start would drop the start and silently kill capture. Starting a session already tears
        // the previous one down.
        lcd_csi_send_cmd(true);

        char chan_buf[16];
        snprintf(chan_buf, sizeof(chan_buf), "CH %u", csi_channels[csi_channel_idx]);
        lv_label_set_text(chan_lbl, chan_buf);
    } else if (ui_btns->left_btn) { // Go back
        lcd_csi_send_cmd(false);
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        state_lbl = rate_lbl = health_lbl = chan_lbl = NULL;
        init = false;

        // Restore the selection arrows the router hid on the way in
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);

        // Show tools menu
        lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Switch back
        ui_menu->page = TOOLS_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        lcd_csi_send_cmd(false);
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        state_lbl = rate_lbl = health_lbl = chan_lbl = NULL;
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}
