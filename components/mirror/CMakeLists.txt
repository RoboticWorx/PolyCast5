# Collect every .c under src/ (recursively). Globbed at configure time, so a new file builds only once CMake re-runs
file(GLOB_RECURSE SRCS
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     "src/*.c")

idf_component_register(
    SRCS ${SRCS}

    INCLUDE_DIRS
        "include"

    REQUIRES
        common gpio

    PRIV_REQUIRES
        wifi esp-tls esp_timer nvs_flash
)
