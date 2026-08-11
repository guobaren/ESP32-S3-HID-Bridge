#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "output_mode_selector.h"
#include "usb_output_liveness.h"

static void test_event(int actual, int expected)
{
    assert(actual == expected);
}

static void test_ready_and_unavailable_boundaries(void)
{
    usb_output_liveness_t state = {0};
    const uint32_t timeout_ms = 100U;

    test_event(usb_output_liveness_update(&state, true, 0U, timeout_ms),
               USB_OUTPUT_LIVENESS_BECAME_AVAILABLE);
    test_event(usb_output_liveness_update(&state, true, 1U, timeout_ms),
               USB_OUTPUT_LIVENESS_NO_CHANGE);

    test_event(usb_output_liveness_update(&state, false, 99U, timeout_ms),
               USB_OUTPUT_LIVENESS_NO_CHANGE);
    test_event(usb_output_liveness_update(&state, true, 0U, timeout_ms),
               USB_OUTPUT_LIVENESS_NO_CHANGE);

    test_event(usb_output_liveness_update(&state, false, 99U, timeout_ms),
               USB_OUTPUT_LIVENESS_NO_CHANGE);
    test_event(usb_output_liveness_update(&state, false, 1U, timeout_ms),
               USB_OUTPUT_LIVENESS_BECAME_UNAVAILABLE);
    test_event(usb_output_liveness_update(&state, false, 1U, timeout_ms),
               USB_OUTPUT_LIVENESS_NO_CHANGE);
}

static void test_recovery_is_immediate(void)
{
    usb_output_liveness_t state = {0};
    const uint32_t timeout_ms = 100U;

    test_event(usb_output_liveness_update(&state, true, 0U, timeout_ms),
               USB_OUTPUT_LIVENESS_BECAME_AVAILABLE);
    test_event(usb_output_liveness_update(&state, false, timeout_ms, timeout_ms),
               USB_OUTPUT_LIVENESS_BECAME_UNAVAILABLE);
    test_event(usb_output_liveness_update(&state, true, 0U, timeout_ms),
               USB_OUTPUT_LIVENESS_BECAME_AVAILABLE);
    test_event(usb_output_liveness_update(&state, true, 1U, timeout_ms),
               USB_OUTPUT_LIVENESS_NO_CHANGE);
}

static void test_unavailable_elapsed_time_saturates(void)
{
    usb_output_liveness_t state = {0};
    const uint32_t timeout_ms = UINT32_MAX;

    test_event(usb_output_liveness_update(&state, true, 0U, timeout_ms),
               USB_OUTPUT_LIVENESS_BECAME_AVAILABLE);
    test_event(usb_output_liveness_update(&state, false, UINT32_MAX - 50U, timeout_ms),
               USB_OUTPUT_LIVENESS_NO_CHANGE);

    /* A wrapping accumulator would become 49 here and miss the timeout. */
    test_event(usb_output_liveness_update(&state, false, 100U, timeout_ms),
               USB_OUTPUT_LIVENESS_BECAME_UNAVAILABLE);
    test_event(usb_output_liveness_update(&state, false, UINT32_MAX, timeout_ms),
               USB_OUTPUT_LIVENESS_NO_CHANGE);
}

static void test_unavailable_usb_switches_to_connected_ble(void)
{
    usb_output_liveness_t liveness = {0};
    output_mode_selector_t selector = {0};
    const uint32_t timeout_ms = 100U;

    test_event(usb_output_liveness_update(&liveness, true, 0U, timeout_ms),
               USB_OUTPUT_LIVENESS_BECAME_AVAILABLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_USB);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_BLE, true) == OUTPUT_MODE_USB);

    test_event(usb_output_liveness_update(&liveness, false, 50U, timeout_ms),
               USB_OUTPUT_LIVENESS_NO_CHANGE);
    test_event(usb_output_liveness_update(&liveness, false, 50U, timeout_ms),
               USB_OUTPUT_LIVENESS_BECAME_UNAVAILABLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, false) == OUTPUT_MODE_BLE);

    test_event(usb_output_liveness_update(&liveness, true, 0U, timeout_ms),
               USB_OUTPUT_LIVENESS_BECAME_AVAILABLE);
    assert(output_mode_selector_set_connected(&selector, OUTPUT_MODE_USB, true) == OUTPUT_MODE_BLE);
}

int main(void)
{
    test_ready_and_unavailable_boundaries();
    test_recovery_is_immediate();
    test_unavailable_elapsed_time_saturates();
    test_unavailable_usb_switches_to_connected_ble();
    puts("USB 输出存活测试通过：覆盖 99/100 ms 边界、恢复、累计时间饱和及断线后切换 BLE。");
    return 0;
}
