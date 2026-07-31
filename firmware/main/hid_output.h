#pragma once

#include "bridge_protocol.h"
#include "esp_err.h"

esp_err_t hid_output_init(void);
esp_err_t hid_output_submit(const bridge_frame_t *frame);
