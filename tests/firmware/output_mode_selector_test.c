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

static void test_usb_hid_gates_ble_until_usb_is_unavailable(void)
{
    output_mode_selector_t selector = {0};
    output_mode_selector_set_usb_monitoring(&selector, true);

    /* HID 存活尚未判定时，不能先启动 BLE。 */
    assert(!output_mode_selector_should_allow_ble(&selector));
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_USB);
    assert(!output_mode_selector_should_allow_ble(&selector));

    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, false) == OUTPUT_MODE_NONE);
    assert(output_mode_selector_should_allow_ble(&selector));
}

static void test_ble_activity_is_sticky_when_usb_recovers(void)
{
    output_mode_selector_t selector = {0};
    output_mode_selector_set_usb_monitoring(&selector, true);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, false) == OUTPUT_MODE_NONE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_should_allow_ble(&selector));

    /* BLE 已活动后 USB 恢复不能抢占，BLE 仍允许保持到自然断开。 */
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_BLE);
    assert(output_mode_selector_should_allow_ble(&selector));

    /* BLE 断开后回到 USB，并重新禁止 BLE 广播。 */
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, false) == OUTPUT_MODE_USB);
    assert(!output_mode_selector_should_allow_ble(&selector));
}

static void test_cdc_profile_does_not_wait_for_usb_hid(void)
{
    output_mode_selector_t selector = {0};
    output_mode_selector_set_usb_monitoring(&selector, false);
    assert(output_mode_selector_should_allow_ble(&selector));
}

int main(void)
{
    test_usb_stays_active_until_disconnected();
    test_ble_stays_active_until_disconnected();
    test_inactive_disconnect_does_not_switch();
    test_usb_hid_gates_ble_until_usb_is_unavailable();
    test_ble_activity_is_sticky_when_usb_recovers();
    test_cdc_profile_does_not_wait_for_usb_hid();
    puts("输出模式选择测试通过：USB HID 闸门、BLE 活动锁定和断线回退策略均符合预期。");
    return 0;
}
