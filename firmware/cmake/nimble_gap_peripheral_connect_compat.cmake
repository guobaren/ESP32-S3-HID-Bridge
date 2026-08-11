# ESP-IDF v6.0.2 的 NimBLE Host 会在外设 ACL 建立后先读取远端版本与特性，
# 直到可选查询完成才向应用上报 CONNECT。部分 Windows 蓝牙控制器重启后不回应
# LL_PERIPHERAL_FEATURE_REQ，导致应用来不及启动加密，ACL 即监督超时。
#
# 本补丁只改变外设角色：ACL 建立后立即上报 CONNECT status=0，并跳过外设主动
# 远端版本/特性查询。中心角色保持 SDK 原行为。生成文件仅位于 build 目录，不修改
# 共享 IDF_PATH；若上游源码锚点变化，配置阶段直接失败，防止静默应用错误补丁。

function(hid_bridge_generate_nimble_gap_compat source_path output_path)
    if(NOT EXISTS "${source_path}")
        message(FATAL_ERROR "NimBLE GAP source not found: ${source_path}")
    endif()

    file(READ "${source_path}" nimble_gap_source)

    set(expected_block [=[
    if (evt->role == BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE) {
        ble_gap_rd_rem_ver_tx(evt->connection_handle);
    } else {
        ble_gap_rd_rem_sup_feat_tx(evt->connection_handle);
    }
]=])

    set(replacement_block [=[
    if (evt->role == BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE) {
        /*
         * HID Bridge compatibility patch:
         * Notify the application as soon as the peripheral ACL exists so a
         * bonded peer can start encryption immediately. Do not force the
         * optional LL_PERIPHERAL_FEATURE_REQ sequence; some Windows Bluetooth
         * controllers stop responding to it immediately after radio restart.
         */
        conn->slave_conn = 1;
        ble_gap_event_connect_call(evt->connection_handle, 0);
    } else {
        ble_gap_rd_rem_sup_feat_tx(evt->connection_handle);
    }
]=])

    string(FIND "${nimble_gap_source}" "${expected_block}" anchor_index)
    if(anchor_index EQUAL -1)
        message(FATAL_ERROR
            "ESP-IDF NimBLE GAP layout changed; HID Bridge compatibility patch was not applied. "
            "Expected v6.0.2 peripheral remote-feature anchor is missing.")
    endif()

    string(REPLACE "${expected_block}" "${replacement_block}"
        patched_nimble_gap_source "${nimble_gap_source}")

    get_filename_component(output_directory "${output_path}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_directory}")
    file(WRITE "${output_path}" "${patched_nimble_gap_source}")
endfunction()
