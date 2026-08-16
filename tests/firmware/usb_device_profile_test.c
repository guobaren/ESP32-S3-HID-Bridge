#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "usb_device_profile.h"

int main(void)
{
    usb_device_profile_t selected_profile = usb_device_profile_select();

    assert(selected_profile == USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD);
    assert(strcmp(
               usb_device_profile_name(selected_profile),
               "键盘 + 相对触摸板 HID") == 0);

    puts("USB 设备配置选择测试通过：无需 UART 参数，始终选择键盘 + 相对触摸板 HID，名称正确。");
    return 0;
}
