#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    USB_OUTPUT_LIVENESS_NO_CHANGE = 0,
    USB_OUTPUT_LIVENESS_BECAME_AVAILABLE,
    USB_OUTPUT_LIVENESS_BECAME_UNAVAILABLE,
} usb_output_liveness_event_t;

/* Windows 上开发板复位后，USB HID 重新枚举可能明显超过运行期掉线阈值。 */
#define USB_OUTPUT_STARTUP_ENUMERATION_GRACE_MS 3000U

typedef struct {
    bool available;
    bool has_been_ready;
    bool initial_unavailable_reported;
    uint32_t startup_elapsed_ms;
    uint32_t unavailable_elapsed_ms;
} usb_output_liveness_t;

/**
 * 根据 USB HID 端点是否可发送更新输出可用状态。
 *
 * 冷启动时给予 USB_OUTPUT_STARTUP_ENUMERATION_GRACE_MS 的枚举宽限期，期间
 * 即使端点尚不可发送也不报告离线；宽限期到期后若仍未 ready，报告一次
 * 离线以允许 BLE 后端启动。USB 一旦曾经 ready，后续连续不可发送则使用
 * unavailable_timeout_ms 快速判定掉线；恢复可发送时立即重新上线。
 */
usb_output_liveness_event_t usb_output_liveness_update(
    usb_output_liveness_t *state,
    bool ready,
    uint32_t elapsed_ms,
    uint32_t unavailable_timeout_ms);
