
idf_component_register(

    SRCS
        "src/infrared_funcs.c"
        "src/infrared_task.c"
                
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos nvs_flash
    
    PRIV_REQUIRES
    	gpio lcd
)
