# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c         # List the source files of this component
        #"${CMAKE_SOURCE_DIR}/drivers/sx126x/lr_fhss_mac.c"
        #"${CMAKE_SOURCE_DIR}/drivers/sx126x/sx126x_driver_version.c"
        #"${CMAKE_SOURCE_DIR}/drivers/sx126x/sx126x_hal.c"
        #"${CMAKE_SOURCE_DIR}/drivers/sx126x/sx126x_lr_fhss.c"
        #"${CMAKE_SOURCE_DIR}/drivers/sx126x/sx126x.c"
        
        #"${CMAKE_SOURCE_DIR}/components/lora/src/lora_task.c"
        #"${CMAKE_SOURCE_DIR}/components/lora/src/lora_funcs.c"
        #"${CMAKE_SOURCE_DIR}/components/lora/src/aes.c"
        
        #"${CMAKE_SOURCE_DIR}/components/lcd/src/lcd_funcs.c"
        #"${CMAKE_SOURCE_DIR}/components/lcd/src/lcd_task.c"
        #"${CMAKE_SOURCE_DIR}/components/lcd/src/st7789.c"
        #"${CMAKE_SOURCE_DIR}/components/lcd/src/fontx.c"
        
        #"${CMAKE_SOURCE_DIR}/components/infrared/src/infrared_funcs.c"
        #"${CMAKE_SOURCE_DIR}/components/infrared/src/infrared_task.c"
        
        #"${CMAKE_SOURCE_DIR}/components/gpio/src/tca9535.c"
        #"${CMAKE_SOURCE_DIR}/components/gpio/src/gpio_funcs.c"
        #"${CMAKE_SOURCE_DIR}/components/gpio/src/gpio_task.c"
        
        #"${CMAKE_SOURCE_DIR}/components/bluetooth/src/bluetooth_funcs.c"
        #"${CMAKE_SOURCE_DIR}/components/bluetooth/src/bluetooth_task.c"
        #"${CMAKE_SOURCE_DIR}/components/bluetooth/src/esp_hid_gap.c"
        
        #"${CMAKE_SOURCE_DIR}/components/lcd/media/src/resolutiontest.c"
        
    INCLUDE_DIRS "."    # List public include directories
    	#"${CMAKE_SOURCE_DIR}/drivers/sx126x"
    	#"${CMAKE_SOURCE_DIR}/components/lora/include"
    	#"${CMAKE_SOURCE_DIR}/components/lcd/include"
    	#"${CMAKE_SOURCE_DIR}/components/lcd/media/include"
    	#"${CMAKE_SOURCE_DIR}/components/infrared/include"
    	#"${CMAKE_SOURCE_DIR}/components/gpio/include"
    	#"${CMAKE_SOURCE_DIR}/components/bluetooth/include"
    
    REQUIRES            # List the public requirements (component names)
    	gpio lcd lora infrared espnow
    	driver freertos nvs_flash bt esp_hid esp_driver_gpio console esp_driver_uart esp_psram
    	
    PRIV_REQUIRES       # List the private requirements
)
