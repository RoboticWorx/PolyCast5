
idf_component_register(

    SRCS
        "src/fontx.c"
        "src/lcd_ir_funcs.c"
        "src/lcd_lora_funcs.c"
        "src/lcd_espnow_funcs.c"
        "src/lcd_funcs.c"
        "src/lcd_task.c"
        "src/st7789.c"
        
        "media/city/src/anim_city_1.c"
        "media/city/src/anim_city_2.c"
        "media/city/src/anim_city_3.c"
        "media/city/src/anim_city_4.c"
        "media/city/src/anim_city_5.c"
        "media/city/src/anim_city_6.c"
        "media/city/src/anim_city_7.c"
        "media/city/src/anim_city_8.c"
        "media/city/src/anim_city_9.c"
        "media/city/src/anim_city_10.c"
        "media/city/src/anim_city_11.c"
        "media/city/src/anim_city_12.c"
        "media/city/src/anim_city_13.c"
        "media/city/src/anim_city_14.c"
        "media/city/src/anim_city_15.c"
        "media/city/src/anim_city_16.c"
        "media/city/src/anim_city_17.c"
        "media/city/src/anim_city_18.c"
        "media/city/src/anim_city_19.c"
        "media/city/src/anim_city_20.c"
        
    INCLUDE_DIRS 
    	"include"
    	"media/city/include"
    
    REQUIRES
    	driver freertos lvgl sx126x
    
    PRIV_REQUIRES
    	gpio sx126x infrared espnow lora
)
