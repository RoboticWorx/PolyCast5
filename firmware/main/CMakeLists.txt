# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c         # list the source files of this component
        "${CMAKE_SOURCE_DIR}/drivers/sx126x/lr_fhss_mac.c"
        "${CMAKE_SOURCE_DIR}/drivers/sx126x/sx126x_driver_version.c"
        "${CMAKE_SOURCE_DIR}/drivers/sx126x/sx126x_hal.c"
        "${CMAKE_SOURCE_DIR}/drivers/sx126x/sx126x_lr_fhss.c"
        "${CMAKE_SOURCE_DIR}/drivers/sx126x/sx126x.c"
        
        "${CMAKE_SOURCE_DIR}/components/lora/src/lora_task.c"
        "${CMAKE_SOURCE_DIR}/components/lora/src/lora_funcs.c"
        "${CMAKE_SOURCE_DIR}/components/lora/src/aes.c"
        
        "${CMAKE_SOURCE_DIR}/components/lcd/src/lcd_funcs.c"
        "${CMAKE_SOURCE_DIR}/components/lcd/src/lcd_task.c"
        
        "${CMAKE_SOURCE_DIR}/components/infrared/src/infrared_funcs.c"
        "${CMAKE_SOURCE_DIR}/components/infrared/src/infrared_task.c"
        
        "${CMAKE_SOURCE_DIR}/components/bluetooth/src/bluetooth_funcs.c"
        "${CMAKE_SOURCE_DIR}/components/bluetooth/src/bluetooth_task.c"
        "${CMAKE_SOURCE_DIR}/components/bluetooth/src/esp_hid_gap.c"
    INCLUDE_DIRS "."    # optional, add here public include directories
    	"${CMAKE_SOURCE_DIR}/drivers/sx126x"
    	"${CMAKE_SOURCE_DIR}/components/lora/include"
    	"${CMAKE_SOURCE_DIR}/components/lcd/include"
    	"${CMAKE_SOURCE_DIR}/components/infrared/include"
    	"${CMAKE_SOURCE_DIR}/components/bluetooth/include"
    PRIV_INCLUDE_DIRS   # optional, add here private include directories
    REQUIRES            # optional, list the public requirements (component names)
    	driver freertos nvs_flash bt esp_hid esp_driver_gpio
    PRIV_REQUIRES       # optional, list the private requirements
)
