
idf_component_register(

    SRCS
        "src/tca9535.c"
        "src/gpio_funcs.c"
        "src/gpio_task.c"
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos common nvs_flash
    
    PRIV_REQUIRES
    	lcd
)
