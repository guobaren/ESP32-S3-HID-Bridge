idf_component_register(
    SRCS
        "app_main.c"
        "ble_output.c"
        "bridge_protocol.c"
        "device_discovery.c"
        "esp_hid_gap.c"
        "hid_output.c"
        "input_session.c"
        "output_mode_selector.c"
        "output_router.c"
        "secure_channel.c"
        "status_led.c"
        "usb_cdc_input.c"
        "usb_device_profile.c"
        "usb_output_liveness.c"
        "wifi_input.c"
        "wifi_manager.c"
        "wifi_provisioning.c"
        "wifi_target_output.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_uart
        esp_driver_gpio
        esp_event
        esp_timer
        esp_hid
        esp_http_server
        esp_netif
        esp_tinyusb
        esp_wifi
        lwip
        led_strip
        mbedtls
        nvs_flash
        bt
)

if(CONFIG_BT_NIMBLE_ENABLED)
    set(nimble_gap_source
        "${IDF_PATH}/components/bt/host/nimble/nimble/nimble/host/src/ble_gap.c")
    set(nimble_gap_compat_script
        "${CMAKE_CURRENT_LIST_DIR}/../cmake/nimble_gap_peripheral_connect_compat.cmake")
    set(nimble_gap_compat_source
        "${CMAKE_CURRENT_BINARY_DIR}/nimble_gap_compat/ble_gap.c")

    include("${nimble_gap_compat_script}")
    hid_bridge_generate_nimble_gap_compat(
        "${nimble_gap_source}"
        "${nimble_gap_compat_source}")

    # 让上游源码或补丁脚本变化时自动重新配置并重新生成兼容源文件。
    # include() 已自动跟踪补丁脚本；这里只额外跟踪被读取的上游 C 源码。
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${nimble_gap_source}")

    # 保留 bt 组件原有编译定义、头文件路径和链接关系，仅替换这一份源文件。
    idf_component_get_property(bt_component_library bt COMPONENT_LIB)
    get_target_property(bt_component_sources ${bt_component_library} SOURCES)
    list(REMOVE_ITEM bt_component_sources "${nimble_gap_source}")
    set_property(TARGET ${bt_component_library} PROPERTY SOURCES "${bt_component_sources}")
    target_sources(${bt_component_library} PRIVATE "${nimble_gap_compat_source}")
    # 原始 ble_gap.c 依赖同目录的 Host 私有头；生成副本需要显式保留该搜索路径。
    get_filename_component(nimble_gap_source_directory "${nimble_gap_source}" DIRECTORY)
    target_include_directories(${bt_component_library} PRIVATE
        "${nimble_gap_source_directory}")
endif()
