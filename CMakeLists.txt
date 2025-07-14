# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# App name
project(PolyCast5)

# Build a SPIFFS image
spiffs_create_partition_image(assets "${CMAKE_SOURCE_DIR}/assets" FLASH_IN_PROJECT)