#pragma once
#include <stdbool.h>

#include "bridge_protocol.h"
#include "esp_err.h"
#include "output_mode_selector.h"
#include "usb_device_profile.h"

esp_err_t output_router_init(usb_device_profile_t usb_profile);
esp_err_t output_router_submit(const bridge_frame_t *frame);
esp_err_t output_router_release_all(void);
void output_router_set_connected(output_mode_t mode, bool connected);
