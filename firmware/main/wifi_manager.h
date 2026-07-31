#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t wifi_manager_init(void);
bool wifi_manager_wait_connected(TickType_t timeout);
bool wifi_manager_is_configured(void);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_provisioning(void);
esp_err_t wifi_manager_start_provisioning(void);
esp_err_t wifi_manager_apply_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_get_ip(char *buffer, size_t buffer_size);
