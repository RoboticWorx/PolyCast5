# Collect every .c (recursively)
file(GLOB_RECURSE SRCS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    "src/*.c"
    "media/src/*.c"
    "fonts/src/*.c"
)

idf_component_register(
    SRCS ${SRCS}
        
    INCLUDE_DIRS
        "include"
        "media/include"
        "fonts"

    REQUIRES
        common lvgl sx126x infrared espnow lora wifi bluetooth ai gpio bootloader_support spiffs esp_driver_spi esp_driver_i2c esp_driver_gpio
    
    PRIV_REQUIRES
)