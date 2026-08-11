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
    output_mode_t active_mode;
} output_mode_selector_t;

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

