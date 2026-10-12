cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp_tinyusb"
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__tinyusb")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_bandwidth_probe)
if(CONFIG_USB_PROBE_VENDOR)
    idf_component_get_property(probe_tinyusb_lib espressif__tinyusb COMPONENT_LIB)
    target_compile_definitions(${probe_tinyusb_lib} PUBLIC CFG_TUD_VENDOR_RX_NEED_ZLP=1)
endif()
