# Collect every .c (recursively). Globbed at configure time, so a new file builds only once CMake re-runs
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
        common lvgl sx126x infrared espnow lora wifi bluetooth ai gpio expansions mirror bootloader_support joltwallet__littlefs esp_driver_spi esp_driver_i2c esp_driver_gpio
    
    PRIV_REQUIRES
)