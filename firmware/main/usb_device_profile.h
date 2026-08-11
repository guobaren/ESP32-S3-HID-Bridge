#pragma once

#include <stdbool.h>

typedef enum {
    USB_DEVICE_PROFILE_CDC = 0,
    USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD = 1,
} usb_device_profile_t;

usb_device_profile_t usb_device_profile_select(bool uart_protocol_detected);
bool usb_device_profile_requires_restart(
    usb_device_profile_t selected_profile,
    bool uart_protocol_detected);
const char *usb_device_profile_name(usb_device_profile_t profile);
