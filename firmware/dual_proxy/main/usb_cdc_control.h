#pragma once

#include "bridge_protocol.h"
#include "esp_err.h"

typedef void (*dual_cdc_report_callback_t)(const dual_frame_t *frame);
typedef void (*dual_cdc_release_callback_t)(void);

esp_err_t dual_usb_cdc_control_start(
    dual_cdc_report_callback_t report_callback,
    dual_cdc_release_callback_t release_callback);
void dual_usb_cdc_control_on_detached(void);
