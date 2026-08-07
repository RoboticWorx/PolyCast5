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
        ai bluetooth lora esp_wifi esp_netif espnow gpio app_update esp_https_ota esp_http_server esp_pm lwip
)