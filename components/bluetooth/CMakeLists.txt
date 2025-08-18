# Collect every .c under src/ (recursively)
file(GLOB_RECURSE SRCS
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     "src/*.c")

idf_component_register(
    SRCS ${SRCS}
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	common esp_hid bt
    
    PRIV_REQUIRES
    	nvs_flash esp_http_server esp_wifi esp_netif gpio
)
