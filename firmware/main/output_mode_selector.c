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

    /* 当前活动链路仍在线时保持锁定，后来连接的链路不得主动抢占。 */
    if ((selector->active_mode == OUTPUT_MODE_USB && selector->usb_connected) ||
        (selector->active_mode == OUTPUT_MODE_BLE && selector->ble_connected)) {
        return selector->active_mode;
    }

    /*
     * 没有活动链路时，由本次新连接的链路先取得输出；当前链路断开时，
     * 则切换到仍在线的另一条链路。正常事件序列下不会同时满足两个回退项。
     */
    if (connected) {
        selector->active_mode = mode;
    } else if (selector->usb_connected) {
        selector->active_mode = OUTPUT_MODE_USB;
    } else if (selector->ble_connected) {
        selector->active_mode = OUTPUT_MODE_BLE;
    } else {
        selector->active_mode = OUTPUT_MODE_NONE;
    }

    return selector->active_mode;
}
