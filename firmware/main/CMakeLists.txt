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

# 使用项目本地 esp_tinyusb 驱动封装；只有确认 Device task、PM 与 PHY 已停止后才允许切换角色。
idf_component_get_property(tinyusb_component_lib
    espressif__esp_tinyusb COMPONENT_LIB)
if(NOT tinyusb_component_lib OR NOT TARGET ${tinyusb_component_lib})
    message(FATAL_ERROR "TinyUSB lifecycle overlay: component target was not found")
endif()
get_target_property(tinyusb_component_sources ${tinyusb_component_lib} SOURCES)
if(NOT tinyusb_component_sources)
    message(FATAL_ERROR "TinyUSB lifecycle overlay: component SOURCES property is empty")
endif()
set(tinyusb_driver_source_count 0)
set(tinyusb_sources_without_lifecycle_overlay "")
foreach(tinyusb_source IN LISTS tinyusb_component_sources)
    string(REPLACE "\\" "/" tinyusb_source_normalized "${tinyusb_source}")
    if(tinyusb_source_normalized MATCHES "(^|/)tinyusb[.]c$")
        math(EXPR tinyusb_driver_source_count "${tinyusb_driver_source_count} + 1")
    else()
        list(APPEND tinyusb_sources_without_lifecycle_overlay "${tinyusb_source}")
    endif()
endforeach()
if(NOT tinyusb_driver_source_count EQUAL 1)
    message(FATAL_ERROR "TinyUSB lifecycle overlay: expected one driver source, found ${tinyusb_driver_source_count}")
endif()
list(APPEND tinyusb_sources_without_lifecycle_overlay
    "${CMAKE_CURRENT_LIST_DIR}/../tinyusb_overrides/tinyusb.c")
set_property(TARGET ${tinyusb_component_lib} PROPERTY SOURCES
    "${tinyusb_sources_without_lifecycle_overlay}")
message(STATUS "TinyUSB lifecycle: using project-local uninstall overlay")

if(CONFIG_DUAL_PROXY_USE_CHERRYUSB)
    target_sources(${COMPONENT_LIB} PRIVATE
        "hid_host_cherryusb.c"
        "usb_host_diagnostics.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "."
        "${CMAKE_CURRENT_LIST_DIR}/../cherryusb_overrides")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ld_include_hid")

    # CherryUSB 生命周期与诊断仅通过本地覆盖源替换，不修改托管组件。
    idf_component_get_property(cherryusb_component_lib
        cherry-embedded__cherryusb COMPONENT_LIB)
    idf_component_get_property(cherryusb_component_dir
        cherry-embedded__cherryusb COMPONENT_DIR)
    if(NOT cherryusb_component_lib OR NOT TARGET ${cherryusb_component_lib})
        message(FATAL_ERROR "CherryUSB HID diagnostics: component library target was not found")
    endif()
    get_target_property(cherryusb_component_sources ${cherryusb_component_lib} SOURCES)
    if(NOT cherryusb_component_sources)
        message(FATAL_ERROR "CherryUSB HID diagnostics: component SOURCES property is empty")
    endif()

    set(cherryusb_hid_source_count 0)
    set(cherryusb_dwc2_source_count 0)
    set(cherryusb_hub_source_count 0)
    set(cherryusb_core_source_count 0)
    set(cherryusb_glue_source_count 0)
    set(cherryusb_sources_without_diagnostics "")
    foreach(cherryusb_source IN LISTS cherryusb_component_sources)
        string(REPLACE "\\" "/" cherryusb_source_normalized "${cherryusb_source}")
        if(cherryusb_source_normalized MATCHES "(^|/)class/hid/usbh_hid[.]c$")
            math(EXPR cherryusb_hid_source_count "${cherryusb_hid_source_count} + 1")
        elseif(cherryusb_source_normalized MATCHES "(^|/)port/dwc2/usb_hc_dwc2[.]c$")
            math(EXPR cherryusb_dwc2_source_count "${cherryusb_dwc2_source_count} + 1")
        elseif(cherryusb_source_normalized MATCHES "(^|/)class/hub/usbh_hub[.]c$")
            math(EXPR cherryusb_hub_source_count "${cherryusb_hub_source_count} + 1")
        elseif(cherryusb_source_normalized MATCHES "(^|/)core/usbh_core[.]c$")
            math(EXPR cherryusb_core_source_count "${cherryusb_core_source_count} + 1")
        elseif(cherryusb_source_normalized MATCHES "(^|/)port/dwc2/usb_glue_esp[.]c$")
            math(EXPR cherryusb_glue_source_count "${cherryusb_glue_source_count} + 1")
        else()
            list(APPEND cherryusb_sources_without_diagnostics "${cherryusb_source}")
        endif()
    endforeach()
    if(NOT cherryusb_hid_source_count EQUAL 1)
        message(FATAL_ERROR "CherryUSB HID diagnostics: expected one upstream class source, found ${cherryusb_hid_source_count}")
    endif()
    if(NOT cherryusb_dwc2_source_count EQUAL 1)
        message(FATAL_ERROR "CherryUSB DWC2 diagnostics: expected one upstream port source, found ${cherryusb_dwc2_source_count}")
    endif()
    if(NOT cherryusb_hub_source_count EQUAL 1)
        message(FATAL_ERROR "CherryUSB hub diagnostics: expected one upstream class source, found ${cherryusb_hub_source_count}")
    endif()
    if(NOT cherryusb_core_source_count EQUAL 1)
        message(FATAL_ERROR "CherryUSB lifecycle: expected one upstream core source, found ${cherryusb_core_source_count}")
    endif()
    if(NOT cherryusb_glue_source_count EQUAL 1)
        message(FATAL_ERROR "CherryUSB lifecycle: expected one ESP glue source, found ${cherryusb_glue_source_count}")
    endif()

    list(APPEND cherryusb_sources_without_diagnostics
        "${CMAKE_CURRENT_LIST_DIR}/../cherryusb_overrides/usbh_hid.c"
        "${CMAKE_CURRENT_LIST_DIR}/../cherryusb_overrides/cherryusb_hid_control.c"
        "${CMAKE_CURRENT_LIST_DIR}/../cherryusb_overrides/cherryusb_report_retry.c"
        "${CMAKE_CURRENT_LIST_DIR}/../cherryusb_overrides/usb_hc_dwc2.c"
        "${CMAKE_CURRENT_LIST_DIR}/../cherryusb_overrides/usbh_hub.c"
        "${CMAKE_CURRENT_LIST_DIR}/../cherryusb_overrides/usbh_core.c"
        "${CMAKE_CURRENT_LIST_DIR}/../cherryusb_overrides/usb_glue_esp.c")
    set_property(TARGET ${cherryusb_component_lib} PROPERTY SOURCES
        "${cherryusb_sources_without_diagnostics}")
    target_include_directories(${cherryusb_component_lib} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}"
        "${cherryusb_component_dir}/port/dwc2")
    message(STATUS "CherryUSB lifecycle: using project-local HID, DWC2, hub, core and ESP glue overlays")
endif()

option(DUAL_PROXY_ENABLE_MAKCU_ASCII_API
    "Enable the legacy opt-in Makcu V3-style ASCII subset on M UART0" ON)
if(DUAL_PROXY_ENABLE_MAKCU_ASCII_API)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        DUAL_PROXY_ENABLE_MAKCU_ASCII_API=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        DUAL_PROXY_ENABLE_MAKCU_ASCII_API=0)
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
