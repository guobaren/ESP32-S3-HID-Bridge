#pragma once

typedef enum {
    USB_DEVICE_PROFILE_CDC = 0,
    USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD = 1,
} usb_device_profile_t;

usb_device_profile_t usb_device_profile_select(void);
const char *usb_device_profile_name(usb_device_profile_t profile);
