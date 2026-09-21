#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "bridge_protocol.h"
#include "hid_device_profile.h"

esp_err_t dual_pc_hid_install_device(void);
esp_err_t dual_pc_hid_start_sender(void);
esp_err_t dual_pc_hid_stop_sender(void);
esp_err_t dual_pc_hid_schedule_reconfigure(const hid_device_profile_t *profile);
void dual_pc_hid_enable_reconfigure(void);
void dual_pc_hid_software_report(uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan);
void dual_pc_hid_software_release(void);
void dual_pc_hid_physical_report(uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan);
void dual_pc_hid_physical_release(void);
void dual_pc_hid_release_all(void);
bool dual_pc_hid_ready(void);
void dual_pc_hid_handle_vendor_frame(const dual_frame_t *frame);
void dual_pc_hid_vendor_link_fault(void);
