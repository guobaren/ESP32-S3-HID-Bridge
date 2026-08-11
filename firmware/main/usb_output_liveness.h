#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    USB_OUTPUT_LIVENESS_NO_CHANGE = 0,
    USB_OUTPUT_LIVENESS_BECAME_AVAILABLE,
    USB_OUTPUT_LIVENESS_BECAME_UNAVAILABLE,
} usb_output_liveness_event_t;

typedef struct {
    bool available;
    uint32_t unavailable_elapsed_ms;
} usb_output_liveness_t;

/**
 * 根据 USB HID 端点是否可发送更新输出可用状态。
 *
 * 首次可发送时立即上线；连续不可发送达到超时后才下线，避免端点短暂忙碌
 * 导致误切换。恢复可发送时立即重新上线。
 */
usb_output_liveness_event_t usb_output_liveness_update(
    usb_output_liveness_t *state,
    bool ready,
    uint32_t elapsed_ms,
    uint32_t unavailable_timeout_ms);
