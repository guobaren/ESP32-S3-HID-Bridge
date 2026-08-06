#include <assert.h>
#include <stdio.h>

#include "output_mode_selector.h"

static void test_ble_takes_over_usb(void)
{
    output_mode_selector_t selector = {0};
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, false) == OUTPUT_MODE_USB);
}

static void test_usb_does_not_preempt_ble(void)
{
    output_mode_selector_t selector = {0};
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, false) == OUTPUT_MODE_BLE);
}

static void test_all_disconnected(void)
{
    output_mode_selector_t selector = {0};
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, false) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, false) == OUTPUT_MODE_NONE);
}

int main(void)
{
    test_ble_takes_over_usb();
    test_usb_does_not_preempt_ble();
    test_all_disconnected();
    puts("输出模式选择测试通过：BLE 就绪后优先接管，BLE 断开后安全回退 USB。");
    return 0;
}
