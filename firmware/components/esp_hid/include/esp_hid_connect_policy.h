#pragma once

#include <stdbool.h>

/*
 * ESP-IDF v6.0.2 的外设路径会在 HCI Read Remote Supported Features 完成后
 * 才分发 BLE_GAP_EVENT_CONNECT，并把该 HCI Complete 事件的 status 原样传出。
 * 因此这里必须使用 HCI 状态语义，不能套用数值相同的 BLE_HS_* Host 错误码。
 * 0x1a 表示远端不支持该可选特性，ACL 仍可继续；0x08/0x13 分别是连接监督
 * 超时和远端主动断开，不能标为 HID 已连接。
 */
enum {
    ESP_HID_NIMBLE_STATUS_SUCCESS = 0,
    ESP_HID_HCI_STATUS_CONNECTION_SUPERVISION_TIMEOUT = 8,
    ESP_HID_HCI_STATUS_REMOTE_USER_TERMINATED = 19,
    ESP_HID_HCI_STATUS_UNSUPPORTED_REMOTE_FEATURE = 26,
    /* Legacy 高占空比定向广播每轮最多约 1.28 秒；24 轮覆盖约 30 秒。 */
    ESP_HID_DIRECTED_RECONNECT_BURSTS = 24,
};

static inline bool esp_hid_nimble_connect_event_is_usable(
    int status,
    bool connection_handle_is_active)
{
    if (!connection_handle_is_active) {
        return false;
    }

    return status == ESP_HID_NIMBLE_STATUS_SUCCESS ||
           status == ESP_HID_HCI_STATUS_UNSUPPORTED_REMOTE_FEATURE;
}

static inline bool esp_hid_nimble_should_use_directed_reconnect(
    bool has_bonded_peer,
    unsigned int bursts_remaining)
{
    return has_bonded_peer && bursts_remaining > 0;
}

/*
 * 定向广播已经促成一次连接尝试后，若远端特性交换仍以监督超时或远端终止
 * 结束，继续重新装填定向广播预算会让 Windows 每约 10 秒重复一次失败连接。
 * 这两类失败应立即回落普通快速广播，等待中心端以完整广播流程重新发现。
 */
static inline bool esp_hid_nimble_rejected_connect_should_fall_back_to_undirected(
    int status)
{
    return status == ESP_HID_HCI_STATUS_CONNECTION_SUPERVISION_TIMEOUT ||
           status == ESP_HID_HCI_STATUS_REMOTE_USER_TERMINATED;
}
