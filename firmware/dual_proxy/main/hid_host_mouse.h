#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_protocol.h"
#include "esp_err.h"

typedef void (*dual_physical_mouse_callback_t)(
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan);
typedef void (*dual_physical_release_callback_t)(bool device_gone);

esp_err_t dual_hid_host_start(
    dual_physical_mouse_callback_t report_callback,
    dual_physical_release_callback_t release_callback);
esp_err_t dual_hid_host_stop(void);
bool dual_hid_host_device_present(void);
bool dual_hid_host_mouse_present(void);
/* 复制当前已序列化的物理设备 Profile；没有完整快照时返回 0。 */
size_t dual_hid_host_copy_profile_blob(uint32_t offset, uint8_t *output, size_t capacity,
                                       uint32_t *total_length);

/* 厂商控制事务是否仍在 400 ms 窗口内（G HUB 初始化/查询期间为真）。 */
bool dual_hid_host_vendor_busy(void);
esp_err_t dual_hid_host_request_profile_refresh(void);
void dual_hid_host_handle_control_frame(const dual_frame_t *frame);
void dual_hid_host_clear_control_queue(void);
