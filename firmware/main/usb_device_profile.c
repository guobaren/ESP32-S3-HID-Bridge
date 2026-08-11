#include "usb_device_profile.h"

usb_device_profile_t usb_device_profile_select(bool uart_protocol_detected)
{
    return uart_protocol_detected
        ? USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD
        : USB_DEVICE_PROFILE_CDC;
}

bool usb_device_profile_requires_restart(
    usb_device_profile_t selected_profile,
    bool uart_protocol_detected)
{
    return selected_profile == USB_DEVICE_PROFILE_CDC && uart_protocol_detected;
}

const char *usb_device_profile_name(usb_device_profile_t profile)
{
    switch (profile) {
    case USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD:
        return "键盘 + 相对触摸板 HID";
    case USB_DEVICE_PROFILE_CDC:
    default:
        return "CDC 串口";
    }
}
