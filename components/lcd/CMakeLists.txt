file(GLOB_RECURSE SRCS
	RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
	"src/*.c"
	"media/city/src/*.c"
	"media/black_hole/src/*.c"
	"media/other/src/*.c"
	"media/qr/src/*.c"
)

idf_component_register(
    SRCS ${SRCS}
        
    INCLUDE_DIRS 
    	"include"
    	"media/qr/include"
    	"media/city/include"
    	"media/black_hole/include"
    	"media/other/include"
    
    REQUIRES
    	common lvgl sx126x infrared espnow lora wifi gpio tools bootloader_support
    
    PRIV_REQUIRES
)