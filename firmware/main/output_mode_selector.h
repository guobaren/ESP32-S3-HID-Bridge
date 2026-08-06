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
 * BLE HID 在完成加密并真正可发送后优先于 USB HID；BLE 断开时若 USB
 * 仍在线则回退到 USB，否则回到无活动输出状态。
 */
output_mode_t output_mode_selector_set_connected(
    output_mode_selector_t *selector,
    output_mode_t mode,
    bool connected);

