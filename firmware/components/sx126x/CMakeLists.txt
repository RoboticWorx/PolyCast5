
idf_component_register(

    SRCS
        "src/lr_fhss_mac.c"
        "src/sx126x_driver_version.c"
        "src/sx126x_hal.c"
        "src/sx126x_lr_fhss.c"
        "src/sx126x.c"
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos
    
    PRIV_REQUIRES
    	gpio lora lcd
)
