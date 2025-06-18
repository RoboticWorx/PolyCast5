
idf_component_register(

    SRCS
        "src/wifi_funcs.c"
        "src/wifi_task.c"
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos common
    
    PRIV_REQUIRES
    	gpio lcd esp_wifi esp_netif espnow
)
