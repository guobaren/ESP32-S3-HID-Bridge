#pragma once
#include <stdbool.h>

#include "bridge_protocol.h"
#include "esp_err.h"

esp_err_t ble_output_init(void);
esp_err_t ble_output_submit(const bridge_frame_t *frame);

/* 由输出路由控制 BLE 是否允许广播/建立新的连接。 */
void ble_output_set_transport_allowed(bool allowed);
void ble_output_start_advertising_if_allowed(void);
