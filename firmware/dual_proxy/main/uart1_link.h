#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bridge_protocol.h"
#include "esp_err.h"

#define DUAL_ROLE_PC_DEVICE 1
#define DUAL_ROLE_MOUSE_HOST 2
#define DUAL_USB_STATE_WAITING 0
#define DUAL_USB_STATE_MOUNTED 1
#define DUAL_USB_STATE_HID_CONNECTED 2
#define DUAL_USB_STATE_DISCONNECTED 3
#define DUAL_USB_STATE_ERROR 4

typedef void (*dual_link_frame_callback_t)(const dual_frame_t *frame);
typedef void (*dual_link_fault_callback_t)(void);

esp_err_t dual_uart1_start(
    uint8_t role,
    const uint8_t node_id[6],
    dual_link_frame_callback_t frame_callback,
    dual_link_fault_callback_t fault_callback);
void dual_uart1_set_usb_state(uint8_t usb_state);
bool dual_uart1_peer_online(void);
uint16_t dual_uart1_generation(void);
esp_err_t dual_uart1_send_mouse(
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan);
esp_err_t dual_uart1_send_release(uint8_t reason);
