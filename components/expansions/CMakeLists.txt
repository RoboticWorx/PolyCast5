# Collect every .c under src/ (recursively)
file(GLOB_RECURSE SRCS
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     "src/*.c")

# ST's VL53LX bare driver, vendored verbatim in vl53lx/core, plus our ESP-IDF platform
# layer in vl53lx/port. None of ST's calibration entry points is referenced now - crosstalk,
# RefSpad and offset alike - so --gc-sections drops all three (the crosstalk one alone was
# worth ~8.8 KiB). See the header of src/vl53l4cx.c before adding another.
file(GLOB_RECURSE VL53LX_SRCS
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     "vl53lx/core/*.c"
     "vl53lx/port/*.c")

idf_component_register(
    SRCS ${SRCS} ${VL53LX_SRCS}

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "vl53lx/core"
        "vl53lx/port"

    REQUIRES
        common

    PRIV_REQUIRES
        gpio esp_driver_i2c nvs_flash
)

# ST's sources are vendored unmodified and are not warning-clean under this project's
# flags. Scope the suppressions to their files only, so our own code keeps every warning.
set_source_files_properties(${VL53LX_SRCS}
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-but-set-variable;-Wno-unused-variable;-Wno-missing-field-initializers;-Wno-sign-compare;-Wno-implicit-fallthrough")
