
idf_component_register(

    SRCS
        "src/bluetooth_funcs.c"
        "src/bluetooth_task.c"
                
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos nvs_flash
    
    PRIV_REQUIRES
    	gpio lcd
)
