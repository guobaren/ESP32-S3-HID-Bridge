#pragma once
#include "bridge_protocol.h"
#include "esp_err.h"

esp_err_t wifi_target_output_init(void);
esp_err_t wifi_target_output_submit(const bridge_frame_t *frame);
