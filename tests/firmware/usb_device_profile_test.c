#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "usb_device_profile.h"

int main(void)
{
    usb_device_profile_t without_uart = usb_device_profile_select(false);
    usb_device_profile_t with_uart = usb_device_profile_select(true);

    assert(without_uart == USB_DEVICE_PROFILE_CDC);
    assert(with_uart == USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD);
    assert(usb_device_profile_requires_restart(without_uart, true));
    assert(!usb_device_profile_requires_restart(without_uart, false));
    assert(!usb_device_profile_requires_restart(with_uart, true));
    assert(strstr(usb_device_profile_name(without_uart), "CDC") != NULL);
    assert(strstr(usb_device_profile_name(with_uart), "HID") != NULL);

    puts("USB 设备配置选择测试通过：启动期选择正确，CDC 模式晚到 UART 握手会请求重启切换 HID。");
    return 0;
}
