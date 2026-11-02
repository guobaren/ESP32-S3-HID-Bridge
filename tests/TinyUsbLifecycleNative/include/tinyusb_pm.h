#pragma once

#include <stdbool.h>
#include "tinyusb.h"

typedef struct {
    bool lock_enable;
} tinyusb_pm_config_t;

esp_err_t tinyusb_pm_init(const tinyusb_pm_config_t *config);
esp_err_t tinyusb_pm_deinit(void);
esp_err_t tinyusb_pm_lock_get(bool *acquired);
esp_err_t tinyusb_pm_remote_wake(void);
void tinyusb_pm_on_event(tinyusb_event_t *event);
