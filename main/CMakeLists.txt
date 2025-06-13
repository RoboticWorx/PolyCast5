# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c         # List the source files of this component
        
    INCLUDE_DIRS "."    # List public include directories

    REQUIRES            # List the public requirements (component names)
    	gpio lcd lora infrared espnow wifi
    	driver freertos nvs_flash bt esp_hid esp_driver_gpio console esp_driver_uart esp_psram
    	
    PRIV_REQUIRES       # List the private requirements
)
