#include "hid_clone_descriptor.h"

#include <string.h>

#define USB_DESC_DEVICE 0x01U
#define USB_DESC_CONFIGURATION 0x02U
#define USB_DESC_INTERFACE 0x04U
#define USB_DESC_ENDPOINT 0x05U
#define USB_CLASS_HID 0x03U
#define USB_ENDPOINT_DIR_IN 0x80U

static uint16_t read_u16_le(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static void write_u16_le(uint8_t *value, uint16_t data)
{
    value[0] = (uint8_t)data;
    value[1] = (uint8_t)(data >> 8);
}

static int find_report_index(const hid_device_profile_t *profile, uint8_t interface_number)
{
    for (uint8_t index = 0; index < profile->report_descriptor_count; ++index) {
        if (profile->report_descriptors[index].interface_number == interface_number) {
            return index;
        }
    }
    return -1;
}

static uint8_t allocate_endpoint(uint8_t used_mask)
{
    for (uint8_t number = 1; number <= HID_CLONE_MAX_ENDPOINT_NUMBER; ++number) {
        if ((used_mask & (uint8_t)(1U << number)) == 0) {
            return number;
        }
    }
    return 0;
}

static void append_cdc_descriptor(
    uint8_t *destination,
    uint8_t control_interface,
    uint8_t data_interface,
    uint8_t notification_endpoint,
    uint8_t data_out_endpoint,
    uint8_t data_in_endpoint)
{
    const uint8_t descriptor[HID_CLONE_CDC_DESCRIPTOR_LENGTH] = {
        8, 0x0B, control_interface, 2, 0x02, 0x02, 0x01, 0,
        9, USB_DESC_INTERFACE, control_interface, 0, 1, 0x02, 0x02, 0x01, 0,
        5, 0x24, 0x00, 0x10, 0x01,
        5, 0x24, 0x01, 0x00, data_interface,
        4, 0x24, 0x02, 0x02,
        5, 0x24, 0x06, control_interface, data_interface,
        7, USB_DESC_ENDPOINT, notification_endpoint, 0x03, 8, 0, 16,
        9, USB_DESC_INTERFACE, data_interface, 0, 2, 0x0A, 0x00, 0x00, 0,
        7, USB_DESC_ENDPOINT, data_out_endpoint, 0x02, 64, 0, 0,
        7, USB_DESC_ENDPOINT, data_in_endpoint, 0x02, 64, 0, 0,
    };
    memcpy(destination, descriptor, sizeof(descriptor));
}

static bool hid_clone_descriptor_build_internal(
    const hid_device_profile_t *profile,
    hid_clone_descriptor_set_t *output,
    bool append_cdc)
{
    if (profile == NULL || output == NULL ||
        profile->device_descriptor.length != 18U ||
        profile->config_descriptor.length < 9U ||
        profile->config_descriptor.length > HID_PROFILE_MAX_CONFIG_DESCRIPTOR ||
        profile->config_descriptor.length + HID_CLONE_CDC_DESCRIPTOR_LENGTH >
            HID_CLONE_MAX_CONFIG_LENGTH) {
        return false;
    }
    const uint8_t *device = profile->device_descriptor.data;
    const uint8_t *config = profile->config_descriptor.data;
    const uint16_t config_length = profile->config_descriptor.length;
    if (device[0] != 18U || device[1] != USB_DESC_DEVICE ||
        config[0] != 9U || config[1] != USB_DESC_CONFIGURATION ||
        read_u16_le(&config[2]) != config_length || config[4] > 253U) {
        return false;
    }

    hid_clone_descriptor_set_t built;
    memset(&built, 0, sizeof(built));
    memcpy(built.device_descriptor, device, 18U);
    memcpy(built.configuration_descriptor, config, config_length);

    uint8_t used_in = 0;
    uint8_t used_out = 0;
    uint8_t max_interface = 0;
    size_t cursor = 0;
    while (cursor < config_length) {
        if (config_length - cursor < 2U) {
            return false;
        }
        const uint8_t length = config[cursor];
        const uint8_t type = config[cursor + 1U];
        if (length < 2U || length > config_length - cursor) {
            return false;
        }
        if (type == USB_DESC_INTERFACE) {
            if (length < 9U) {
                return false;
            }
            const uint8_t interface_number = config[cursor + 2U];
            if (interface_number > max_interface) {
                max_interface = interface_number;
            }
            if (config[cursor + 5U] == USB_CLASS_HID) {
                if (built.hid_count >= HID_CLONE_MAX_HID_INTERFACES) {
                    return false;
                }
                const int report_index = find_report_index(profile, interface_number);
                if (report_index < 0) {
                    return false;
                }
                built.hid_interface_numbers[built.hid_count] = interface_number;
                built.profile_report_indices[built.hid_count] = (uint8_t)report_index;
                ++built.hid_count;
            }
        } else if (type == USB_DESC_ENDPOINT) {
            if (length < 7U) {
                return false;
            }
            const uint8_t address = config[cursor + 2U];
            const uint8_t number = address & 0x0FU;
            if (number == 0 || number > HID_CLONE_MAX_ENDPOINT_NUMBER) {
                return false;
            }
            uint8_t *mask = (address & USB_ENDPOINT_DIR_IN) != 0 ? &used_in : &used_out;
            if ((*mask & (uint8_t)(1U << number)) != 0) {
                return false;
            }
            *mask |= (uint8_t)(1U << number);
        }
        cursor += length;
    }
    if (cursor != config_length || built.hid_count == 0 ||
        built.hid_count != profile->report_descriptor_count) {
        return false;
    }

    if (!append_cdc) {
        built.configuration_length = config_length;
        *output = built;
        return true;
    }

    const uint8_t notification_number = allocate_endpoint(used_in);
    if (notification_number == 0) {
        return false;
    }
    used_in |= (uint8_t)(1U << notification_number);
    const uint8_t data_in_number = allocate_endpoint(used_in);
    const uint8_t data_out_number = allocate_endpoint(used_out);
    if (data_in_number == 0 || data_out_number == 0) {
        return false;
    }
    built.cdc_control_interface = (uint8_t)(max_interface + 1U);
    built.cdc_data_interface = (uint8_t)(max_interface + 2U);
    built.cdc_notification_endpoint = (uint8_t)(USB_ENDPOINT_DIR_IN | notification_number);
    built.cdc_data_in_endpoint = (uint8_t)(USB_ENDPOINT_DIR_IN | data_in_number);
    built.cdc_data_out_endpoint = data_out_number;
    append_cdc_descriptor(
        &built.configuration_descriptor[config_length],
        built.cdc_control_interface, built.cdc_data_interface,
        built.cdc_notification_endpoint, built.cdc_data_out_endpoint,
        built.cdc_data_in_endpoint);
    built.configuration_length =
        (uint16_t)(config_length + HID_CLONE_CDC_DESCRIPTOR_LENGTH);
    write_u16_le(&built.configuration_descriptor[2], built.configuration_length);
    built.configuration_descriptor[4] = (uint8_t)(config[4] + 2U);
    *output = built;
    return true;
}

bool hid_clone_descriptor_build(
    const hid_device_profile_t *profile,
    hid_clone_descriptor_set_t *output)
{
    return hid_clone_descriptor_build_internal(profile, output, true);
}

bool hid_clone_descriptor_build_exact(
    const hid_device_profile_t *profile,
    hid_clone_descriptor_set_t *output)
{
    return hid_clone_descriptor_build_internal(profile, output, false);
}
