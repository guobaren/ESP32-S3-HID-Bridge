#include <assert.h>
#include <stdio.h>

#include "output_mode_selector.h"

static void test_usb_stays_active_until_disconnected(void)
{
    output_mode_selector_t selector = {0};
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, false) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, false) == OUTPUT_MODE_USB);
}

static void test_ble_stays_active_until_disconnected(void)
{
    output_mode_selector_t selector = {0};
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, false) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, false) == OUTPUT_MODE_BLE);
}

static void test_inactive_disconnect_does_not_switch(void)
{
    output_mode_selector_t selector = {0};
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, false) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, false) == OUTPUT_MODE_NONE);
}

int main(void)
{
    test_usb_stays_active_until_disconnected();
    test_ble_stays_active_until_disconnected();
    test_inactive_disconnect_does_not_switch();
    puts("输出模式选择测试通过：先连接者保持活动，断线后才切换到另一条在线链路。");
    return 0;
}
