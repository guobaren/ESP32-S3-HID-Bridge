/*
 * 直连控制传输通道（绕开 usb_host_hid 组件的单例 ctrl_xfer）。
 *
 * 动机（已实测的机制）：
 *   usb_host_hid 组件整个设备只用**一个** usb_transfer_t（hid_host.c:85 ctrl_xfer），
 *   且 usb_host_transfer_submit_control() 会以
 *   HOST_CHECK(!urb_obj->usb_host_inflight, ESP_ERR_NOT_FINISHED)（usb_host.c）
 *   拒绝复用"仍在飞"的对象；而该栈的 timeout_ms **未实现**
 *   （usb_types_stack.h:171 "currently not supported yet"），所以一笔控制传输一旦
 *   不被设备完成，就会永久占住对象、把整条厂商通道（HID++）锁死。
 *
 * 本模块的做法（对齐真实主机的行为）：
 *   1. 自建 USB 客户端 + 自建事件任务；
 *   2. **每个请求独立分配 usb_transfer_t**，完成后立即释放——一笔卡住不影响其它请求；
 *   3. 自定超时（默认 600 ms）：超时后用 usb_host_endpoint_flush(dev, 0) 退役该 URB
 *      （EP0 在 usb_host 的端点表里是可寻址的第 0 项），再释放，通道继续可用；
 *   4. 设备句柄按地址缓存，设备消失（DEV_GONE）时关闭并重新打开。
 *
 * 中断管道（移动/vendor 输入）仍由 usb_host_hid 组件持有，本模块只接管 EP0。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 启动直连通道（注册客户端 + 事件任务）。可重复调用，已启动则直接返回 ESP_OK。 */
esp_err_t dual_vendor_urb_start(void);

/*
 * 以独立 URB 发送一次 HID 类 SET_REPORT（承载 G HUB 的 HID++ 写请求）。
 * data/length 为**已按线序还原**的负载（含 report ID 首字节，若需要）。
 */
esp_err_t dual_vendor_urb_set_report(
    uint8_t device_address,
    uint8_t interface_number,
    uint8_t report_type,
    uint8_t report_id,
    const uint8_t *data,
    size_t length,
    uint32_t timeout_ms);

/* 计数（供统计行观测）：提交/成功/超时/退役/设备重开。 */
void dual_vendor_urb_stats(
    uint32_t *submitted,
    uint32_t *completed,
    uint32_t *timeouts,
    uint32_t *aborted,
    uint32_t *device_reopens,
    uint32_t *retries,
    int64_t *latency_max_us,
    uint32_t *latency_over_10ms,
    uint32_t *latency_over_100ms);
