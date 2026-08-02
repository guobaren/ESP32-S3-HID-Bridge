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

    if (connected && selector->active_mode == OUTPUT_MODE_NONE) {
        selector->active_mode = mode;
    } else if (!connected && selector->active_mode == mode) {
        if (mode != OUTPUT_MODE_USB && selector->usb_connected) {
            selector->active_mode = OUTPUT_MODE_USB;
        } else if (mode != OUTPUT_MODE_BLE && selector->ble_connected) {
            selector->active_mode = OUTPUT_MODE_BLE;
        } else {
            selector->active_mode = OUTPUT_MODE_NONE;
        }
    }

    return selector->active_mode;
}
