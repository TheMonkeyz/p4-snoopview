cmake_minimum_required(VERSION 3.16)
# ESP32-P4 only (Guition JC1060P470C). sdkconfig.defaults sets CONFIG_IDF_TARGET; pass -D IDF_TARGET=esp32p4 anyway.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(snoopview)
if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "snoopview is for the ESP32-P4 only (got IDF_TARGET=${IDF_TARGET})")
endif()
