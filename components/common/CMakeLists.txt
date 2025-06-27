
idf_component_register(

    SRCS
        #"src/espnow_funcs.c"
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos nvs_flash
    
    PRIV_REQUIRES
    	espnow infrared lora lvgl sx126x gpio lcd
)
