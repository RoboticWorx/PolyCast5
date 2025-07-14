# Collect every .c under src/ (recursively)
file(GLOB_RECURSE SRCS
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     "src/*.c")

idf_component_register(
    SRCS ${SRCS}
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	common lvgl sx126x infrared espnow lora wifi gpio tools bootloader_support spiffs
    
    PRIV_REQUIRES
)