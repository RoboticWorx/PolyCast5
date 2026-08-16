#include "polycast5_macros.h"

#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"

#include "esp_log.h"
#include "esp_mac.h"

#include "core/lv_obj.h"
#include "core/lv_obj_pos.h"
#include "font/lv_font.h"
#include "misc/lv_area.h"
#include "widgets/label/lv_label.h"

#include "wifi_csi.h"
#include "wifi_csi_ruview.h"
#include "wifi_task.h"
#include "wifi_utils.h" // WIFI_CONN_TIMEOUT_MS

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
 *
 * @param [in] start True to start a session, false to stop the running one
 * @param [in] source wifi_csi_source_t selecting passive capture or an associated link
 * @param [in] consumers WIFI_CSI_CONSUMER_* bitmask
 */
static void lcd_csi_send_cmd(bool start, uint8_t source, uint8_t consumers)
{
    wifi_csi_cmd_t cmd = {0};

    cmd.start = start ? 1 : 0;
    cmd.source = source;
    cmd.consumers = consumers;
    cmd.channel = csi_channels[csi_channel_idx];
    cmd.sound_interval_ms = 20;
    cmd.host_port = 5005; // Stock sensing-server port
    cmd.host_ip[0] = '\0'; // Empty means broadcast on this subnet, so no address entry is needed

    // Last MAC byte keeps several PolyCast5s apart in the host's per-node state without asking
    // the user to assign anything
    uint8_t mac[6] = {0};

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        cmd.node_id = mac[5];
    }

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
        // The sensing page uses all three: up/down retune, right hands the stream to a RuView host.
        // The router hid up/down on the way in, so put them back.
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

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

        // Restore the selection arrows the router hid on the way in, and retire the right arrow so
        // the Tools list does not come back offering a direction it has no page for
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

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

        lcd_csi_send_cmd(true, WIFI_CSI_SRC_PROMISC, WIFI_CSI_CONSUMER_LOCAL);

        init = true;
    }

    // Latest status wins, the producer overwrites a depth-one queue
    wifi_csi_status_t st;

    if (xWifiCsiStatusQueue && xQueueReceive(xWifiCsiStatusQueue, &st, 0) == pdTRUE) {
        // There is no motion detector yet, so this reports capture health and nothing more. QUIET
        // and MOTION would both be lies here: the only thing csi_task can currently tell us is
        // whether frames are arriving. Restore the sensing words with the local DSP milestone.
        const char *state_txt = "NO DATA";

        switch (st.state) {
            case WIFI_CSI_STATE_QUIET:   state_txt = "CAPTURING"; break;
            case WIFI_CSI_STATE_INVALID: state_txt = "NO SIGNAL"; break;
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

    if (ui_btns->right_btn == 1) { // Hand the stream to a RuView host instead
        lcd_csi_send_cmd(false, WIFI_CSI_SRC_PROMISC, WIFI_CSI_CONSUMER_LOCAL);

        // The RuView page only takes LEFT, so retire the arrows this page was using
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        state_lbl = rate_lbl = health_lbl = chan_lbl = NULL;
        init = false;

        // Switch pages
        ui_menu->page = TOOLS_CSI_RUVIEW_PAGE;
        return;
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
        lcd_csi_send_cmd(true, WIFI_CSI_SRC_PROMISC, WIFI_CSI_CONSUMER_LOCAL);

        char chan_buf[16];
        snprintf(chan_buf, sizeof(chan_buf), "CH %u", csi_channels[csi_channel_idx]);
        lv_label_set_text(chan_lbl, chan_buf);
    } else if (ui_btns->left_btn) { // Go back
        lcd_csi_send_cmd(false, WIFI_CSI_SRC_PROMISC, WIFI_CSI_CONSUMER_LOCAL);
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        state_lbl = rate_lbl = health_lbl = chan_lbl = NULL;
        init = false;

        // Restore the selection arrows the router hid on the way in, and retire the right arrow so
        // the Tools list does not come back offering a direction it has no page for
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Show tools menu
        lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Switch back
        ui_menu->page = TOOLS_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        lcd_csi_send_cmd(false, WIFI_CSI_SRC_PROMISC, WIFI_CSI_CONSUMER_LOCAL);
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

void lcd_csi_ruview_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
    // Statics
    static bool init = false;
    static bool streaming = false;
    static bool connect_failed = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *state_lbl = NULL;
    static lv_obj_t *host_lbl = NULL;
    static lv_obj_t *tx_lbl = NULL;
    static lv_obj_t *rate_lbl = NULL;

    if (!init) {
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

        state_lbl = lv_label_create(cont);
        lcd_format_label(state_lbl, "CONNECTING", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 0);

        host_lbl = lv_label_create(cont);
        lcd_format_label(host_lbl, "", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 0, -8);

        rate_lbl = lv_label_create(cont);
        lcd_format_label(rate_lbl, "", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 0, 8);

        tx_lbl = lv_label_create(cont);
        lcd_format_label(tx_lbl, "", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        lv_timer_handler();

        init = true;
        streaming = false;
        connect_failed = false;
    }

    // Streaming needs an IP stack, so the link has to be up before the session can start. Same
    // shape as the Claude Usage page: show progress, ask for a reconnect, wait with an exit path.
    // Attempted at most once per visit, because the wait blocks for up to WIFI_CONN_TIMEOUT_MS and
    // the page handler runs every 200 ms: retrying on every pass would wedge the UI in a loop of
    // twenty-second stalls.
    if (!streaming && !connect_failed) {
        if (!(xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT)) {
            xEventGroupSetBits(xWifiEventGroup, WIFI_RECONNECT_BIT);

            uint8_t res = lcd_wait_for_bit_better(xWifiEventGroup, WIFI_CONNECTED_BIT,
                    WIFI_CONN_TIMEOUT_MS);

            if (res == LCD_WAIT_FOR_BIT_BETTER_EXIT) {
                ui_btns->left_btn = 1; // Fall through to the shared exit path below
            } else if (res != LCD_WAIT_FOR_BIT_BETTER_SUCCESS) {
                connect_failed = true;
                lv_label_set_text(state_lbl, "NO WI-FI");
                lv_label_set_text(host_lbl, "Join a network, then SELECT");
            }
        }

        if (xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT) {
            lcd_csi_send_cmd(true, WIFI_CSI_SRC_ASSOC_PING, WIFI_CSI_CONSUMER_RUVIEW);

            // Only that the request went out. Whether anything actually leaves the device is
            // decided by the tx counter below, not by having asked.
            lv_label_set_text(state_lbl, "STARTING");
            streaming = true;
        }
    } else if (connect_failed && ui_btns->select_btn == 1) {
        // Explicit retry, so a failed join is recoverable without leaving the page
        connect_failed = false;
        lv_label_set_text(state_lbl, "CONNECTING");
        lv_label_set_text(host_lbl, "");
    }

    if (streaming) {
        wifi_csi_ruview_stats_t rv;
        wifi_csi_ruview_get_stats(&rv);

        // Claim STREAMING only once frames have actually gone out. A session that failed to start,
        // or a command the depth-one queue dropped, otherwise reads as success on screen.
        lv_label_set_text(state_lbl, rv.sent ? "STREAMING" : "STARTING");

        // Destination, so a silent host is obviously a target problem rather than a capture one
        char host_buf[40];
        snprintf(host_buf, sizeof(host_buf), "%u.%u.%u.%u:%u n%u",
                (unsigned)((rv.dest_ip >> 24) & 0xFF), (unsigned)((rv.dest_ip >> 16) & 0xFF),
                (unsigned)((rv.dest_ip >> 8) & 0xFF), (unsigned)(rv.dest_ip & 0xFF),
                rv.dest_port, rv.node_id);
        lv_label_set_text(host_lbl, host_buf);

        char tx_buf[48];
        snprintf(tx_buf, sizeof(tx_buf), "tx %"PRIu32"  drop %"PRIu32"/%"PRIu32,
                rv.sent, rv.dropped_socket, rv.dropped_rate);
        lv_label_set_text(tx_lbl, tx_buf);

        wifi_csi_status_t st;

        if (xWifiCsiStatusQueue && xQueueReceive(xWifiCsiStatusQueue, &st, 0) == pdTRUE) {
            char rate_buf[40];
            snprintf(rate_buf, sizeof(rate_buf), "%u Hz  %u sc  %d dBm",
                    st.stats.frames_per_sec, st.stats.n_subcarriers, st.stats.rssi);
            lv_label_set_text(rate_lbl, rate_buf);
        }
    }

    if (ui_btns->left_btn) { // Go back
        lcd_csi_send_cmd(false, WIFI_CSI_SRC_ASSOC_PING, WIFI_CSI_CONSUMER_RUVIEW);
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);

        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        state_lbl = host_lbl = tx_lbl = rate_lbl = NULL;
        init = false;
        streaming = false;
        connect_failed = false;

        // Restore the selection arrows the router hid on the way in, and retire the right arrow so
        // the Tools list does not come back offering a direction it has no page for
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        ui_menu->page = TOOLS_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        lcd_csi_send_cmd(false, WIFI_CSI_SRC_ASSOC_PING, WIFI_CSI_CONSUMER_RUVIEW);
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);

        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        state_lbl = host_lbl = tx_lbl = rate_lbl = NULL;
        init = false;
        streaming = false;
        connect_failed = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}
