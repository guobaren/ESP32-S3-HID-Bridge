#pragma once

#include "tinyusb.h"

esp_err_t tinyusb_task_check_config(const tinyusb_task_config_t *config);
esp_err_t tinyusb_task_start(tinyusb_port_t port,
                             const tinyusb_task_config_t *config,
                             const tinyusb_desc_config_t *descriptor);
esp_err_t tinyusb_task_stop(void);
