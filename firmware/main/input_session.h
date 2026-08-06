#pragma once
#include "bridge_protocol.h"
#include "esp_err.h"

typedef enum {
    BRIDGE_INPUT_NONE = 0,
    BRIDGE_INPUT_UART = 1,
    BRIDGE_INPUT_WIFI = 2,
    BRIDGE_INPUT_USB_CDC = 3,
} bridge_input_source_t;

esp_err_t input_session_init(void);
void input_session_handle(bridge_input_source_t source, const bridge_frame_t *frame);
void input_session_disconnected(bridge_input_source_t source);
void input_session_release_all(void);
