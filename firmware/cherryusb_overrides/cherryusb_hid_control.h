#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct usbh_hubport;

typedef struct {
    int raw_result;
    bool raw_result_valid;
    bool channel_idle;
    size_t payload_length;
} cherryusb_hid_control_trace_t;

/* 仅供 HID 初始化的 SET_IDLE / GET_REPORT_DESCRIPTOR 使用。 */
int cherryusb_hid_control_transfer_until(
    struct usbh_hubport *hport,
    uint8_t bm_request_type,
    uint8_t b_request,
    uint16_t w_value,
    uint16_t w_index,
    uint16_t w_length,
    uint8_t *buffer,
    int64_t deadline_us,
    cherryusb_hid_control_trace_t *trace);
