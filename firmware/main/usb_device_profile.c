#include "usb_device_profile.h"

usb_device_profile_t usb_device_profile_select(void)
{
    return USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD;
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
