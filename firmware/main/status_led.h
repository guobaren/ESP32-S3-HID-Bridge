#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** 初始化板载可寻址 RGB 状态指示灯。 */
esp_err_t status_led_init(void);

/** 更新 USB HID 是否已被目标设备枚举。 */
void status_led_set_usb_connected(bool connected);

/** 更新 BLE HID 是否已连接。 */
void status_led_set_ble_connected(bool connected);
