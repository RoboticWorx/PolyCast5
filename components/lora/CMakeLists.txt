# Collect every .c under src/ (recursively)
file(GLOB_RECURSE SRCS
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     "src/*.c")

idf_component_register(
    SRCS ${SRCS}
                
    INCLUDE_DIRS 
        "include"
    
    REQUIRES
        common sx126x

    PRIV_REQUIRES
        espnow gpio bootloader_support mbedtls
        nvs_flash esp_http_server esp_wifi esp_netif
)
