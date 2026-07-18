# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

# Register components
file(GLOB_RECURSE SRCS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.c")

idf_component_register(
    SRCS ${SRCS}
    
    INCLUDE_DIRS
        "."
        
    REQUIRES
        gpio
        lcd
        lora
        infrared
        espnow
        wifi
        bluetooth
        ai
        lvgl
        driver
        freertos
        nvs_flash
        bt
        esp_hid
        esp_driver_gpio
        console
        esp_driver_uart
        esp_psram
        esp_pm
        bootloader_support
        esp_app_format
        efuse
        joltwallet__littlefs
)
