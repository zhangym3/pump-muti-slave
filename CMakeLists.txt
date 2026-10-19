cmake_minimum_required(VERSION 3.16)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/.mesh-sdk/bt")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pump_muti_slave)
