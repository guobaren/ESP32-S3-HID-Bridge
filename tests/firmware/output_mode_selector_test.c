#include <assert.h>
#include <stdio.h>

#include "output_mode_selector.h"

static void test_usb_first(void)
{
    output_mode_selector_t selector = {0};
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, false) == OUTPUT_MODE_USB);
}

static void test_ble_first(void)
{
    output_mode_selector_t selector = {0};
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_BLE);
}

static void test_active_disconnect_handover(void)
{
    output_mode_selector_t selector = {0};
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, false) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, false) == OUTPUT_MODE_NONE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_USB);
}

int main(void)
{
    test_usb_first();
    test_ble_first();
    test_active_disconnect_handover();
    puts("输出模式选择测试通过：首连接锁定、后连接忽略、活动连接断开后安全接管。");
    return 0;
}
