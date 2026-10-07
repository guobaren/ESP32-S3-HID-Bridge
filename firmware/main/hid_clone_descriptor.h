#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_device_profile.h"

#define HID_CLONE_MAX_HID_INTERFACES 4U
#define HID_CLONE_MAX_ENDPOINT_NUMBER 6U
#define HID_CLONE_CDC_DESCRIPTOR_LENGTH 66U
#define HID_CLONE_MAX_CONFIG_LENGTH \
    (HID_PROFILE_MAX_CONFIG_DESCRIPTOR + HID_CLONE_CDC_DESCRIPTOR_LENGTH)

typedef struct {
    uint8_t device_descriptor[18];
    uint8_t configuration_descriptor[HID_CLONE_MAX_CONFIG_LENGTH];
    uint16_t configuration_length;
    uint8_t hid_count;
    uint8_t hid_interface_numbers[HID_CLONE_MAX_HID_INTERFACES];
    uint8_t profile_report_indices[HID_CLONE_MAX_HID_INTERFACES];
    uint8_t cdc_control_interface;
    uint8_t cdc_data_interface;
    uint8_t cdc_notification_endpoint;
    uint8_t cdc_data_out_endpoint;
    uint8_t cdc_data_in_endpoint;
} hid_clone_descriptor_set_t;

bool hid_clone_descriptor_build(
    const hid_device_profile_t *profile,
    hid_clone_descriptor_set_t *output);

/* 诊断及严格兼容模式：完全保留物理设备的配置拓扑，不追加 CDC。 */
bool hid_clone_descriptor_build_exact(
    const hid_device_profile_t *profile,
    hid_clone_descriptor_set_t *output);
