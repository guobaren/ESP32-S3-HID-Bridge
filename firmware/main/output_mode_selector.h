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
 * 无活动模式时，首个连接成功的模式取得锁；活动模式断开后，如果另一
 * 模式仍然在线，则立即由另一模式接管，否则回到等待首个连接的状态。
 */
output_mode_t output_mode_selector_set_connected(
    output_mode_selector_t *selector,
    output_mode_t mode,
    bool connected);

