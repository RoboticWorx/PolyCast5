# PolyCast5 LVGL Simulator

Renders PolyCast5 device screens in a desktop SDL window. Useful for viewing/testing screen layout without needing the physical device.

Uses the same LVGL 9.2.2 source from `components/lvgl/` with the SDL2 backend instead of the ST7789 display driver. The display is 240x135 (matching the device) at 4x zoom for visibility.

## Prerequisites

- MSYS2 with UCRT64 toolchain
- SDL2 and CMake packages:

```bash
pacman -S mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-cmake
```

## Build

Run in an **MSYS2 UCRT64** terminal:

```bash
cd /c/Projects/ESP/PolyCast5/simulator
cmake -G "Unix Makefiles" -B build
cmake --build build
```

## Run

```bash
./build/polycast5_sim.exe
```

### Navigation

| Key | Action |
|-----|--------|
| Left/Right arrows | Cycle through screens |
| 1-9 number keys | Jump to a specific screen |
| Escape | Quit |

The terminal prints which screen is currently displayed.

## Adding screens

Each screen is a standalone function that uses pure LVGL calls to recreate what the firmware renders on the device.

1. Find the screen's setup code in `components/lcd/src/lcd_*.c`
2. Copy the LVGL widget-creation calls into a new function in `screens.c`
3. Replace ESP-IDF calls (NVS reads, semaphores, queues) with hardcoded values
4. Declare the function in `screens.h`
5. Add it to the `screens[]` array in `main.c`

### Example

```c
// screens.c
void screen_example(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, USER_PRIMARY_COLOR, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Hello");
    lv_obj_set_style_text_color(lbl, USER_SECONDARY_COLOR, 0);
    lv_obj_center(lbl);
}
```

```c
// main.c — add to screens[] array
{ "Example", screen_example },
```

## Files

| File | Purpose |
|------|---------|
| `main.c` | SDL + LVGL initialization, keyboard navigation loop |
| `screens.c` | Screen rendering functions (pure LVGL, no ESP-IDF) |
| `screens.h` | Screen function declarations and shared defines (colors, resolution) |
| `lv_conf.h` | LVGL configuration matching device settings (16-bit color, fonts, SDL driver) |
| `CMakeLists.txt` | Build system — compiles LVGL from `components/lvgl/` and links SDL2 |
