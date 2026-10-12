#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hid_device_profile.h"
#include "freertos/FreeRTOS.h"

esp_err_t dual_hid_host_cherryusb_start(void);
esp_err_t dual_hid_host_cherryusb_stop(void);
esp_err_t dual_hid_host_cherryusb_refresh_profile(void);
esp_err_t dual_hid_host_cherryusb_control(
    bool get_report,
    bool vendor_control,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    uint8_t bm_request_type,
    uint8_t b_request,
    uint16_t w_value,
    uint16_t w_index,
    uint16_t w_length,
    uint32_t timeout_ms,
    uint8_t *data,
    size_t data_capacity,
    size_t *data_length);

esp_err_t dual_hid_host_backend_interface_open(
    uint8_t interface_number,
    uint8_t subclass,
    uint8_t protocol,
    const uint8_t *report_descriptor,
    size_t report_descriptor_length,
    uint32_t generation);
void dual_hid_host_backend_interface_close(
    uint8_t interface_number,
    uint32_t generation,
    bool device_gone);
bool dual_hid_host_backend_enqueue_report_from_isr(
    uint8_t interface_number,
    uint32_t generation,
    const uint8_t *report,
    size_t report_length,
    BaseType_t *higher_priority_task_woken);
bool dual_hid_host_backend_enqueue_report(
    uint8_t interface_number,
    uint32_t generation,
    const uint8_t *report,
    size_t report_length,
    bool injected);
void dual_hid_host_backend_request_safety_release(void);
void dual_hid_host_backend_request_safety_release_from_isr(
    BaseType_t *higher_priority_task_woken);
esp_err_t dual_hid_host_backend_publish_profile(
    uint8_t device_address,
    const hid_device_profile_t *profile,
    uint8_t expected_hid_interfaces,
    uint8_t collected_hid_interfaces);
void dual_hid_host_backend_service_disconnect(void);
