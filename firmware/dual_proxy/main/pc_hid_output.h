#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t dual_pc_hid_install_device(void);
esp_err_t dual_pc_hid_start_sender(void);
void dual_pc_hid_software_report(uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan);
void dual_pc_hid_software_release(void);
void dual_pc_hid_physical_report(uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan);
void dual_pc_hid_physical_release(void);
void dual_pc_hid_release_all(void);
bool dual_pc_hid_ready(void);
