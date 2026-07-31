idf_component_register(
    SRCS
        "app_main.c"
        "bridge_protocol.c"
        "hid_output.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_tinyusb
)
