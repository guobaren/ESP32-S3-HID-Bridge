idf_component_register(
    SRCS "connection_validation.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart esp_system esp_timer esp_tinyusb usb_host_hid
)
