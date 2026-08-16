#pragma once

#include <stdbool.h>

typedef enum {
    OUTPUT_MODE_NONE = 0,
    OUTPUT_MODE_USB,
    OUTPUT_MODE_BLE,
} output_mode_t;

typedef struct {
    bool usb_connected;
    bool ble_connected;
    bool usb_monitoring;
    bool usb_state_known;
    output_mode_t active_mode;
} output_mode_selector_t;

/**
 * 标记当前 profile 是否包含需要监测存活状态的 USB HID 输出。
 *
 * CDC-only profile 没有 USB HID 输出，BLE 可以直接作为输出后端；HID-only
 * profile 在首次检测结果出来前保持 BLE 禁止，避免 USB 尚未判定时先启动广播。
 */
void output_mode_selector_set_usb_monitoring(
    output_mode_selector_t *selector,
    bool enabled);

/**
 * 更新输出连接状态并返回当前活动模式。
 *
 * 首个连接并成为活动输出的 USB HID 或 BLE HID 会保持活动，后来连接的
 * 链路不会抢占；只有当前活动链路断开后才切换到仍在线的另一条链路。
 */
output_mode_t output_mode_selector_set_connected(
    output_mode_selector_t *selector,
    output_mode_t mode,
    bool connected);

/**
 * 判断 BLE 是否允许继续广播或建立新的连接。
 *
 * 已经成为活动输出的 BLE 连接保持允许；USB HID 已活动、或 USB HID 尚未
 * 完成首次存活判定时，禁止 BLE 广播和新的连接尝试。
 */
bool output_mode_selector_should_allow_ble(const output_mode_selector_t *selector);

