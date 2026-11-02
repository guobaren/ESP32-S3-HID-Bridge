#pragma once

#include "esp_err.h"
#include "tinyusb.h"

esp_err_t tinyusb_usb_wakeup_init(tinyusb_port_t port);
esp_err_t tinyusb_usb_wakeup_deinit(void);
