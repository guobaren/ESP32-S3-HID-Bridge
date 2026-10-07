#ifndef UART1_VENDOR_QUEUE_LOGIC_H
#define UART1_VENDOR_QUEUE_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#include "bridge_protocol.h"

/*
 * Raw mouse reports also carry button edges and follow the physical-device
 * lifetime. A PC vendor-session BEGIN must invalidate only control traffic.
 */
static inline bool uart1_uses_vendor_input_generation(uint8_t type)
{
    return type == DUAL_MESSAGE_RAW_HID_INPUT;
}

static inline bool uart1_uses_vendor_control_generation(uint8_t type)
{
    return type == DUAL_MESSAGE_HID_SET_REPORT ||
        type == DUAL_MESSAGE_HID_GET_REPORT_REQUEST ||
        type == DUAL_MESSAGE_HID_GET_REPORT_RESPONSE ||
        type == DUAL_MESSAGE_VENDOR_CONTROL_REQUEST ||
        type == DUAL_MESSAGE_VENDOR_CONTROL_RESPONSE;
}

static inline bool uart1_is_retryable_vendor_session_barrier(uint8_t type)
{
    return type == DUAL_MESSAGE_VENDOR_SESSION_BEGIN ||
        type == DUAL_MESSAGE_VENDOR_SESSION_ACK;
}

#endif
