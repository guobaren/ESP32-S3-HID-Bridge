#pragma once

#include "esp_err.h"

typedef struct {
    int gpio_num;
    int port;
} tinyusb_vbus_monitor_config_t;

esp_err_t tinyusb_vbus_monitor_init(const tinyusb_vbus_monitor_config_t *config);
void tinyusb_vbus_monitor_deinit(void);
