# Collect every .c under src/ (recursively)
file(GLOB_RECURSE SRCS
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     "src/*.c")

idf_component_register(
    SRCS ${SRCS}
                
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos nvs_flash common
    
    PRIV_REQUIRES
    	gpio lcd
)
