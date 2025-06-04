
idf_component_register(

    SRCS
    	"src/aes.c"
        "src/lora_funcs.c"
        "src/lora_task.c"
                
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos sx126x
    
    PRIV_REQUIRES
    	gpio lcd espnow
)
