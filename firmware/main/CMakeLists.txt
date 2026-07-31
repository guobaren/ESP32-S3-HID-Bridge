idf_component_register(
    SRCS
        "app_main.c"
        "bridge_protocol.c"
        "hid_output.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_uart
        esp_tinyusb
)
