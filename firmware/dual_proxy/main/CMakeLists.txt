idf_component_register(
    SRCS
        "dual_proxy_app.c"
        "bridge_protocol.c"
        "dual_input_aggregator.c"
        "dual_status_led.c"
        "dual_status_led_logic.c"
        "usb_cdc_control.c"
        "usb_cdc_control_logic.c"
        "uart1_link.c"
        "hid_host_mouse.c"
        "pc_hid_output.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart esp_system esp_timer esp_tinyusb led_strip usb_host_hid
)
