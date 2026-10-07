#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dual_status_led_logic.h"
#include "esp_err.h"

esp_err_t dual_status_led_init(void);
void dual_status_led_set_role(dual_status_led_role_t role);
void dual_status_led_set_pc_mounted(bool mounted);
void dual_status_led_set_host_mouse_ready(bool ready);
void dual_status_led_set_peer_connected(bool connected);
void dual_status_led_set_peer_usb_ready(bool ready);
void dual_status_led_set_flow_error(bool failed);
void dual_status_led_notify_software_success(uint32_t now_ms);
