
idf_component_register(

    SRCS
        "src/fontx.c"
        "src/lcd_funcs.c"
        "src/lcd_task.c"
        "src/st7789.c"
        
        "media/src/resolutiontest.c"
        
    INCLUDE_DIRS 
    	"include"
    	"media/include"
    
    REQUIRES
    	driver freertos lvgl sx126x
    
    PRIV_REQUIRES
    	gpio sx126x infrared
)
