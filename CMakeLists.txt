# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Security profile:
# 0 (default): development build — flash encryption stays in DEVELOPMENT mode,
#              so the device can always be re-flashed with new code over USB.
# 1: locked-down build — RELEASE-mode flash encryption + Secure Boot v2.
#    PERMANENT once flashed: the first boot burns eFuses and plaintext USB
#    re-flashing is disabled forever. Requires secure_boot_signing_key.pem in
#    the project root. After toggling (either direction) run `idf.py fullclean`;
#    if you edit sdkconfig.defaults.secure later, also delete sdkconfig.secure —
#    once generated it silently overrides the defaults files.
#    Read www.polycast5.com/blogs/docs/lock-it-down BEFORE setting this to 1.
set(POLYCAST5_SECURE_RELEASE 0)

if(POLYCAST5_SECURE_RELEASE)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.secure")
    set(SDKCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.secure")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set the embedded firmware version and app name
set(PROJECT_VER "0.1.0")
project(PolyCast5)

# Build a LittleFS image
littlefs_create_partition_image(assets "${CMAKE_SOURCE_DIR}/assets" FLASH_IN_PROJECT)