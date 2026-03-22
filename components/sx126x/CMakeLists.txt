# Collect every .c under src/ (recursively)
file(GLOB_RECURSE SRCS
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     "src/*.c")

idf_component_register(
    SRCS ${SRCS}
        
    INCLUDE_DIRS 
        "include"
    
    REQUIRES
        common esp_driver_gpio esp_driver_spi

    PRIV_REQUIRES
        lora gpio
)
