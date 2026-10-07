#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dual_proxy_runtime_config.h"

#include "bridge_protocol.h"
#include "esp_err.h"
#include "stats_snapshot.h"

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
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API
uint8_t dual_hid_host_makcu_physical_buttons(void);
#endif
/* 复制当前已序列化的物理设备 Profile；没有完整快照时返回 0。 */
size_t dual_hid_host_copy_profile_blob(uint32_t offset, uint8_t *output, size_t capacity,
                                       uint32_t *total_length);

/* 厂商控制事务是否仍在 400 ms 窗口内（G HUB 初始化/查询期间为真）。 */
bool dual_hid_host_vendor_busy(void);

/*
 * 诊断注入（2026-09-27）：把一段原始鼠标报告投进 RX 回调，使位移统计、入队与转发
 * 与物理报告走**完全相同**的路径。raw_report 格式同
 * hid_host_device_get_raw_input_report_data()（设备带 report ID 时首字节即 report ID）。
 */
bool dual_hid_host_inject_report(const uint8_t *raw_report, size_t length);

/*
 * 设备级 Vendor 控制传输自测（2026-09-27）：对物理设备发一笔厂商自定义 EP0 请求，
 * 用来验证通用控制通道（IN 方向的数据写入 out_data）。供 UART0 诊断命令调用。
 */
esp_err_t dual_hid_host_vendor_selftest(
    uint8_t bm_request_type,
    uint8_t b_request,
    uint16_t w_value,
    uint16_t w_index,
    uint16_t w_length,
    uint32_t timeout_ms,
    uint8_t *out_data,
    size_t out_capacity,
    size_t *out_length);
esp_err_t dual_hid_host_request_profile_refresh(void);
void dual_hid_host_collect_stats(dual_stats_snapshot_t *snapshot);
void dual_hid_host_handle_control_frame(const dual_frame_t *frame);
void dual_hid_host_clear_control_queue(void);
