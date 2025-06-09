
idf_component_register(

    SRCS
        #"src/espnow_funcs.c"
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos 
    
    PRIV_REQUIRES
    	espnow gpio infrared lcd lora lvgl sx126x
)
