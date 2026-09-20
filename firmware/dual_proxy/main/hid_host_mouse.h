#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef void (*dual_physical_mouse_callback_t)(
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan);
typedef void (*dual_physical_release_callback_t)(void);

esp_err_t dual_hid_host_start(
    dual_physical_mouse_callback_t report_callback,
    dual_physical_release_callback_t release_callback);
