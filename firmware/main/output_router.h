#pragma once
#include "bridge_protocol.h"
#include "esp_err.h"

esp_err_t output_router_init(void);
esp_err_t output_router_submit(const bridge_frame_t *frame);
esp_err_t output_router_release_all(void);
