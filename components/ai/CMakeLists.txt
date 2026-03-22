# Collect every .c under src/ (recursively)
file(GLOB_RECURSE SRCS
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     "src/*.c")

idf_component_register(
    SRCS ${SRCS}
     
    INCLUDE_DIRS 
        "include"
    
    REQUIRES
        common
    
    PRIV_REQUIRES
        gpio wifi bluetooth espnow nvs_flash esp_http_server esp_wifi esp_netif esp_http_client esp-tls mbedtls lwip tcp_transport http_parser esp_event esp_timer esp_driver_gpio esp_driver_i2s
)
