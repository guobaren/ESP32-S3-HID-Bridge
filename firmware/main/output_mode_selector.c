#include "output_mode_selector.h"

#include <stddef.h>

static bool *connection_flag(output_mode_selector_t *selector, output_mode_t mode)
{
    if (mode == OUTPUT_MODE_USB) {
        return &selector->usb_connected;
    }
    if (mode == OUTPUT_MODE_BLE) {
        return &selector->ble_connected;
    }
    return NULL;
}

output_mode_t output_mode_selector_set_connected(
    output_mode_selector_t *selector,
    output_mode_t mode,
    bool connected)
{
    if (selector == NULL) {
        return OUTPUT_MODE_NONE;
    }

    bool *flag = connection_flag(selector, mode);
    if (flag == NULL) {
        return selector->active_mode;
    }
    *flag = connected;

    /*
     * BLE 只有在加密完成、HID 真正可发送后才会被标记为 connected。
     * 因此 BLE 就绪时优先使用 BLE；这样原生 USB 即使仍接着供电或已枚举，
     * 也不会阻止用户切换到 BLE。BLE 断开后再安全回退到 USB。
     */
    if (selector->ble_connected) {
        selector->active_mode = OUTPUT_MODE_BLE;
    } else if (selector->usb_connected) {
        selector->active_mode = OUTPUT_MODE_USB;
    } else {
        selector->active_mode = OUTPUT_MODE_NONE;
    }

    return selector->active_mode;
}
