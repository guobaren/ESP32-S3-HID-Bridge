set(main_requires
    esp_driver_uart esp_system esp_timer esp_tinyusb led_strip usb_host_hid)
if(CONFIG_DUAL_PROXY_USE_CHERRYUSB)
    list(APPEND main_requires cherryusb)
endif()

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
        "button_state_logic.c"
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
        "mouse_motion_smoother.c"
    INCLUDE_DIRS "."
    REQUIRES ${main_requires}
)

if(CONFIG_DUAL_PROXY_USE_CHERRYUSB)
    target_sources(${COMPONENT_LIB} PRIVATE "hid_host_cherryusb.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE ".")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ld_include_hid")

    # CherryUSB 1.5.2~2 HID 类在报告描述符读取失败时没有阶段错误信息。
    # 仅在 CherryUSB 可选构建中替换唯一对应源文件，不修改托管组件。
    idf_component_get_property(cherryusb_component_lib
        cherry-embedded__cherryusb COMPONENT_LIB)
    if(NOT cherryusb_component_lib OR NOT TARGET ${cherryusb_component_lib})
        message(FATAL_ERROR "CherryUSB HID diagnostics: component library target was not found")
    endif()
    get_target_property(cherryusb_component_sources ${cherryusb_component_lib} SOURCES)
    if(NOT cherryusb_component_sources)
        message(FATAL_ERROR "CherryUSB HID diagnostics: component SOURCES property is empty")
    endif()

    set(cherryusb_hid_source_count 0)
    set(cherryusb_sources_without_hid_class "")
    foreach(cherryusb_source IN LISTS cherryusb_component_sources)
        string(REPLACE "\\" "/" cherryusb_source_normalized "${cherryusb_source}")
        if(cherryusb_source_normalized MATCHES "(^|/)class/hid/usbh_hid[.]c$")
            math(EXPR cherryusb_hid_source_count "${cherryusb_hid_source_count} + 1")
        else()
            list(APPEND cherryusb_sources_without_hid_class "${cherryusb_source}")
        endif()
    endforeach()
    if(NOT cherryusb_hid_source_count EQUAL 1)
        message(FATAL_ERROR "CherryUSB HID diagnostics: expected one upstream class source, found ${cherryusb_hid_source_count}")
    endif()

    list(APPEND cherryusb_sources_without_hid_class
        "${CMAKE_CURRENT_LIST_DIR}/../cherryusb_overrides/usbh_hid.c")
    set_property(TARGET ${cherryusb_component_lib} PROPERTY SOURCES
        "${cherryusb_sources_without_hid_class}")
    message(STATUS "CherryUSB HID diagnostics: using project-local usbh_hid.c overlay")
endif()

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

option(DUAL_PROXY_ENABLE_BUTTON_STATE
    "Enable the independent UART0 physical mouse button state stream" ON)
if(DUAL_PROXY_ENABLE_BUTTON_STATE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        DUAL_PROXY_ENABLE_BUTTON_STATE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        DUAL_PROXY_ENABLE_BUTTON_STATE=0)
endif()
