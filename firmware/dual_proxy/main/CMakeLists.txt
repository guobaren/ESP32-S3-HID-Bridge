idf_component_register(
    SRCS
        "dual_proxy_app.c"
        "bridge_protocol.c"
        "dual_input_aggregator.c"
        "dual_status_led.c"
        "dual_status_led_logic.c"
        "usb_cdc_control.c"
        "usb_cdc_control_logic.c"
        "uart0_control.c"
        "uart0_protocol_router.c"
        "m_udp_smoothing.c"
        "makcu_ascii_logic.c"
        "makcu_v4_logic.c"
        "uart0_output.c"
        "stats_snapshot.c"
        "stats_snapshot_codec.c"
        "diag_stream.c"
        "uart1_link.c"
        "hid_device_profile.c"
        "hid_clone_descriptor.c"
        "hid_report_layout.c"
        "hid_host_mouse.c"
        "vendor_urb.c"
        "pc_hid_output.c"
        "../../main/mouse_motion_smoother.c"
    INCLUDE_DIRS "." "../../main"
    REQUIRES esp_driver_uart esp_system esp_timer esp_tinyusb led_strip usb_host_hid
)

option(DUAL_PROXY_ENABLE_MAKCU_ASCII_API
    "Enable the legacy opt-in Makcu V3-style ASCII subset on M UART0" OFF)
if(DUAL_PROXY_ENABLE_MAKCU_ASCII_API)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        DUAL_PROXY_ENABLE_MAKCU_ASCII_API=1)
endif()

option(DUAL_PROXY_ENABLE_MAKCU_V4_API
    "Enable Makcu V4 mouse ASCII and MAK_API interfaces on M UART0" ON)
if(DUAL_PROXY_ENABLE_MAKCU_V4_API)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        DUAL_PROXY_ENABLE_MAKCU_V4_API=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        DUAL_PROXY_ENABLE_MAKCU_V4_API=0)
endif()
