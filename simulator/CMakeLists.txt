cmake_minimum_required(VERSION 3.16)
project(polycast5_sim C)

set(CMAKE_C_STANDARD 11)

# Path to LVGL source (reuse from firmware)
set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/lvgl")

# Find SDL2
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# --- Build LVGL as a library ---
file(GLOB_RECURSE LVGL_SOURCES "${LVGL_DIR}/src/*.c")

# Remove platform-specific drivers we don't need (keeps compile clean)
list(FILTER LVGL_SOURCES EXCLUDE REGEX "drivers/wayland/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "drivers/x11/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "drivers/windows/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "drivers/nuttx/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "drivers/display/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "drivers/evdev/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "drivers/libinput/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "drivers/glfw/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "drivers/opengles/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "libs/thorvg/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "others/vg_lite_tvg/")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "stdlib/lvgl_psram_heap")

add_library(lvgl STATIC ${LVGL_SOURCES})

target_include_directories(lvgl PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"   # For lv_conf.h
    "${LVGL_DIR}"                   # For lvgl.h
)

target_include_directories(lvgl SYSTEM PUBLIC
    ${SDL2_INCLUDE_DIRS}
)

target_compile_definitions(lvgl PUBLIC
    LV_CONF_INCLUDE_SIMPLE=1
    LV_LVGL_H_INCLUDE_SIMPLE=1
)

target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})

# --- Build simulator executable ---
add_executable(polycast5_sim
    main.c
    screens.c
    ../components/lcd/media/src/img_save_new_remote.c
)

target_include_directories(polycast5_sim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LVGL_DIR}"
)

target_link_libraries(polycast5_sim PRIVATE lvgl)
