#pragma once

#include "esp_err.h"

esp_err_t usb_cdc_input_init(void);
void usb_cdc_input_on_detached(void);
