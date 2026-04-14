/**
 * PolyCast5 LVGL Simulator
 *
 * Renders PolyCast5 screens in an SDL window for website screenshots.
 * Uses the same LVGL source as the firmware with SDL backend.
 *
 * Navigation:
 *   Left/Right arrows — cycle through screens
 *   Up/Down arrows    — navigate menu items (with scroll animation)
 *   1-9 number keys   — jump to a specific screen
 *   Click + drag      — scroll lists (Settings, LoRa, etc.)
 *   Escape            — exit
 *
 * The window opens at 4x zoom (960x540) for visibility.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include <SDL2/SDL.h>

#include "screens.h"

#define ZOOM 4

/* Screen registry — add new screens here */
typedef void (*screen_fn_t)(void);

typedef struct {
    const char  *name;
    screen_fn_t  render;
} screen_entry_t;

static const screen_entry_t screens[] = {
    { "Selection",         screen_selection          },
    { "Bluetooth",         screen_bluetooth          },
    { "Infrared",          screen_infrared           },
    { "Infrared Add Sig",  screen_infrared_add_signal},
    { "LoRa",              screen_lora               },
    { "Tools",             screen_tools              },
    { "Wi-Fi Beacon",      screen_wifi_beacon        },
    { "Wi-Fi Data",        screen_wifi_data          },
    { "Settings",          screen_settings           },
};

#define NUM_SCREENS (sizeof(screens) / sizeof(screens[0]))

static int current_screen = 0;
static volatile int pending_screen = -1; /* Set by event filter, applied in main loop */
static volatile int pending_nav    =  0; /* -1 = up, +1 = down */
static volatile int quit_requested =  0;

static void load_screen(int index)
{
    lv_obj_clean(lv_scr_act());
    screen_menu_reset();
    screens[index].render();
    current_screen = index;
    printf("[%d/%d] %s\n", index + 1, (int)NUM_SCREENS, screens[index].name);
}

/**
 * SDL event filter — intercepts events BEFORE LVGL's SDL driver consumes them.
 * This is needed because lv_timer_handler() polls and discards all SDL events.
 */
static int event_filter(void *userdata, SDL_Event *e)
{
    (void)userdata;

    if (e->type == SDL_QUIT) {
        quit_requested = 1;
        return 0; /* Consume the event */
    }

    if (e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.sym) {
            case SDLK_ESCAPE:
                quit_requested = 1;
                return 0;
            case SDLK_LEFT:
                pending_screen = (current_screen - 1 + NUM_SCREENS) % NUM_SCREENS;
                return 0;
            case SDLK_RIGHT:
                pending_screen = (current_screen + 1) % NUM_SCREENS;
                return 0;
            case SDLK_UP:
                pending_nav = -1;
                return 0;
            case SDLK_DOWN:
                pending_nav = 1;
                return 0;
            default:
                if (e->key.keysym.sym >= SDLK_1 && e->key.keysym.sym <= SDLK_9) {
                    int idx = e->key.keysym.sym - SDLK_1;
                    if (idx < (int)NUM_SCREENS) {
                        pending_screen = idx;
                    }
                    return 0;
                }
                break;
        }
    }

    return 1; /* Pass other events through to LVGL */
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    lv_init();

    lv_display_t *disp = lv_sdl_window_create(HOR_RES, VER_RES);
    lv_sdl_window_set_zoom(disp, ZOOM);
    lv_sdl_window_set_title(disp, "PolyCast5 Simulator");

    /* Register mouse so click+drag can scroll lists (LoRa, Settings, etc.) */
    lv_sdl_mouse_create();

    /* Install event filter to catch keys before LVGL eats them */
    SDL_SetEventFilter(event_filter, NULL);

    printf("PolyCast5 Simulator — %dx%d @ %dx zoom\n", HOR_RES, VER_RES, ZOOM);
    printf("Left/Right arrows to navigate, 1-%d to jump, Esc to quit.\n\n", (int)NUM_SCREENS);

    load_screen(0);

    while (!quit_requested) {
        lv_timer_handler();

        /* Apply pending screen change (set by event filter) */
        if (pending_screen >= 0) {
            load_screen(pending_screen);
            pending_screen = -1;
        }

        /* Apply pending menu navigation */
        if (pending_nav != 0) {
            screen_menu_navigate(pending_nav);
            pending_nav = 0;
        }

        SDL_Delay(5);
    }

    return 0;
}
