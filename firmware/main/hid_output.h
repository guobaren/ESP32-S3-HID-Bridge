#pragma once

#include "bridge_protocol.h"
#include "esp_err.h"
#include "usb_device_profile.h"

esp_err_t hid_output_init(usb_device_profile_t profile);
esp_err_t hid_output_submit(const bridge_frame_t *frame);
