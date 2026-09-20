#pragma once

#include "bridge_protocol.h"
#include "esp_err.h"

typedef void (*dual_software_report_callback_t)(const dual_frame_t *frame);
typedef void (*dual_software_release_callback_t)(void);

esp_err_t dual_uart0_control_start(
    dual_software_report_callback_t report_callback,
    dual_software_release_callback_t release_callback);
