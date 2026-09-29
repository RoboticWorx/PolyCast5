#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "polycast5_macros.h"

#include "lcd_utils.h"
#include "lcd_text_input.h"
#include "lcd_mirror_page.h"

#include "mirror.h"

// The sink takes a plain int so the mirror component never has to see lcd's enum, which
// is what keeps the two components from requiring each other
static void text_key_shim(int key)
{
    lcd_text_input_remote_key((lcd_ti_remote_key_t)key);
}

// The mirror types into whichever entry screen is open; lcd_text_input owns the rules
static const mirror_text_sink_t s_text_sink = {
    .type = lcd_text_input_remote_type,
    .key = text_key_shim,
    .state = lcd_text_input_remote_state,
};

void lcd_mirror_page_init(void)
{
    mirror_set_text_sink(&s_text_sink);
}

#define MIRROR_PAGE_URL "polycast5.com/pages/screen-mirror"
#define MIRROR_PAGE_HINT "SELECT stop   LEFT back"

#define MIRROR_PAGE_STOPPING -2 // Page-only view, never a mirror_state_t value
#define MIRROR_PAGE_RESERVED -3 // Page-only: WAITING, relay locked to the approved viewer

static lv_obj_t *lbl_title = NULL;
static lv_obj_t *lbl_code = NULL;
static lv_obj_t *lbl_url = NULL;
static lv_obj_t *lbl_state = NULL;
static lv_obj_t *lbl_hint = NULL;

void lcd_wifi_screen_mirror_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    static bool init = false;
    static int last_state = -1; // -1 forces the first repaint
    static char last_code[sizeof(mirror_code)] = { 0 };
    static char last_check[sizeof(mirror_check)] = { 0 };
    static bool stopping = false; // mirror_state lags a stop until mirror_task's teardown ends

    if (!init) {
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

        lbl_title = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_title, "Screen Mirror", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 4);

        lbl_code = lv_label_create(ACTIVE_SCR);
        lv_obj_set_style_text_align(lbl_code, LV_TEXT_ALIGN_CENTER, 0);
        lcd_format_label(lbl_code, "", user_secondary_color,
                &lv_font_montserrat_30, LV_ALIGN_CENTER, 0, -14);

        lbl_url = lv_label_create(ACTIVE_SCR);
        lv_obj_set_style_text_align(lbl_url, LV_TEXT_ALIGN_CENTER, 0);
        lcd_format_label(lbl_url, MIRROR_PAGE_URL, user_secondary_color,
                &lv_font_montserrat_12, LV_ALIGN_CENTER, 0, 14);

        lbl_state = lv_label_create(ACTIVE_SCR);
        lv_obj_set_style_text_align(lbl_state, LV_TEXT_ALIGN_CENTER, 0);
        lcd_format_label(lbl_state, "Connecting...", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, -18);

        lbl_hint = lv_label_create(ACTIVE_SCR);
        lv_obj_set_style_text_align(lbl_hint, LV_TEXT_ALIGN_CENTER, 0);
        lcd_format_label(lbl_hint, MIRROR_PAGE_HINT, user_secondary_color,
                &lv_font_montserrat_12, LV_ALIGN_BOTTOM_MID, 0, -2);

        // Starting is idempotent, so returning to this page while live just shows status.
        // The shadow holds only what was flushed before the session, so repaint all of it
        (void)mirror_start();
        lv_obj_invalidate(ACTIVE_SCR);

        last_state = -1;
        last_code[0] = '\0';
        last_check[0] = '\0';
        stopping = false; // mirror_start() waited out any earlier teardown

        init = true;
    }

    // Buttons act on the state the user was looking at, so a viewer attaching under a
    // SELECT meant as "stop" cannot turn it into an approval, nor one replacing the viewer
    // on screen be approved unseen
    const int shown = last_state;
    char shown_check[sizeof(last_check)];

    memcpy(shown_check, last_check, sizeof(shown_check));

    if (stopping && !mirror_is_active()) {
        stopping = false;
    }

    // Repaint only on a change: this page is mirrored too, and a label rewritten every
    // 200 ms would dirty its tiles forever
    const mirror_state_t st = mirror_state;
    const int view = stopping ? MIRROR_PAGE_STOPPING :
            (st == MIRROR_WAITING && mirror_relay_locked()) ? MIRROR_PAGE_RESERVED : (int)st;

    if (view != last_state || strcmp(last_code, mirror_code) != 0 ||
            strcmp(last_check, mirror_check) != 0) {
        last_state = view;
        snprintf(last_code, sizeof(last_code), "%s", mirror_code);
        snprintf(last_check, sizeof(last_check), "%s", mirror_check);

        const char *url = MIRROR_PAGE_URL;
        const char *hint = MIRROR_PAGE_HINT;

        switch (view) {
            case MIRROR_PAGE_STOPPING:
                lv_label_set_text(lbl_code, "");
                lv_label_set_text(lbl_state, "Stopping...");
                hint = "";
                break;

            case MIRROR_STARTING:
                lv_label_set_text(lbl_code, "");
                lv_label_set_text(lbl_state, "Connecting...");
                break;

            case MIRROR_WAITING:
                lv_label_set_text(lbl_code, last_code);
                lv_label_set_text(lbl_state, "Enter this code online");
                break;

            case MIRROR_PAGE_RESERVED:
                // The relay now refuses every other browser; a stop gets a fresh code
                lv_label_set_text(lbl_code, last_code);
                lv_label_set_text(lbl_state, "Viewer left");
                url = "Rejoin from the approved browser";
                break;

            case MIRROR_PENDING:
                lv_label_set_text(lbl_code, last_check);
                lv_label_set_text(lbl_state, "Viewer wants control");
                url = "Must match the viewer's screen";
                hint = "SELECT allow   RIGHT deny";
                break;

            case MIRROR_LIVE:
                lv_label_set_text(lbl_code, LV_SYMBOL_EYE_OPEN);
                lv_label_set_text(lbl_state, "LIVE - viewer has control");
                break;

            case MIRROR_ERROR:
                lv_label_set_text(lbl_code, "");
                lv_label_set_text(lbl_state,
                        mirror_error[0] ? mirror_error : "Could not start");
                break;

            case MIRROR_OFF:
            default:
                lv_label_set_text(lbl_code, "");
                lv_label_set_text(lbl_state,
                        mirror_error[0] ? mirror_error : "Stopped");
                break;
        }

        lv_label_set_text(lbl_url, url);
        lv_label_set_text(lbl_hint, hint);
    }

    if (ui_btns->select_btn == 1 && !stopping) {
        if (shown == MIRROR_PENDING) {
            if (st == MIRROR_PENDING) {
                mirror_viewer_approve(shown_check);
            }
        } else if (st == MIRROR_OFF || st == MIRROR_ERROR) {
            (void)mirror_start(); // Retry
            lv_obj_invalidate(ACTIVE_SCR);
        } else {
            mirror_stop(MIRROR_BYE_USER);
            stopping = true;
        }
    } else if (ui_btns->right_btn == 1 && shown == MIRROR_PENDING && !stopping) {
        // Deny ends the session, so the next start issues a fresh code
        if (mirror_is_active()) {
            mirror_stop(MIRROR_BYE_DENIED);
            stopping = true;
        }
    } else if (ui_btns->left_btn == 1) {
        // Leaving the page does not stop the session: mirroring is a global mode, which is
        // the whole point. The status-bar indicator is what says it is still running
        lcd_mirror_page_teardown();
        init = false;

        // Hand the Wi-Fi menu back its scroll arrows, which this page hid. Its own init
        // does not restore them, so every page returning there does this
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        ui_menu->page = WIFI_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
        lcd_mirror_page_teardown();
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_mirror_page_teardown(void)
{
    lv_obj_t **labels[] = { &lbl_title, &lbl_code, &lbl_url, &lbl_state, &lbl_hint };

    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        if (*labels[i]) {
            lv_obj_delete(*labels[i]);
            *labels[i] = NULL;
        }
    }
}
