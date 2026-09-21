#include "hid_device_profile.h"

#include <string.h>

static void write_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static uint16_t read_u16_le(const uint8_t *source)
{
    return (uint16_t)source[0] | ((uint16_t)source[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *source)
{
    return (uint32_t)source[0] |
        ((uint32_t)source[1] << 8) |
        ((uint32_t)source[2] << 16) |
        ((uint32_t)source[3] << 24);
}

static bool valid_utf8(const uint8_t *data, size_t length)
{
    if (data == NULL && length != 0) {
        return false;
    }
    size_t index = 0;
    while (index < length) {
        const uint8_t first = data[index++];
        if (first == 0) {
            return false;
        }
        if (first <= 0x7FU) {
            continue;
        }
        size_t continuation_count;
        uint32_t codepoint;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1;
            codepoint = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2;
            codepoint = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (length - index < continuation_count) {
            return false;
        }
        for (size_t count = 0; count < continuation_count; ++count) {
            const uint8_t continuation = data[index++];
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6) | (continuation & 0x3FU);
        }
        if ((continuation_count == 2 && codepoint < 0x800U) ||
            (continuation_count == 3 && codepoint < 0x10000U) ||
            codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
    }
    return true;
}

static bool copy_bytes(uint8_t *destination, size_t capacity, uint16_t *stored_length,
                       const uint8_t *data, size_t length)
{
    if (destination == NULL || stored_length == NULL || length > capacity ||
        length > UINT16_MAX || (data == NULL && length != 0)) {
        return false;
    }
    if (length != 0) {
        memcpy(destination, data, length);
    }
    *stored_length = (uint16_t)length;
    return true;
}

static bool report_interface_is_unique(const hid_device_profile_t *profile,
                                       uint8_t interface_number)
{
    for (uint8_t index = 0; index < profile->report_descriptor_count; ++index) {
        if (profile->report_descriptors[index].interface_number == interface_number) {
            return false;
        }
    }
    return true;
}

void hid_device_profile_init(hid_device_profile_t *profile)
{
    if (profile != NULL) {
        memset(profile, 0, sizeof(*profile));
    }
}

bool hid_device_profile_set_device_descriptor(
    hid_device_profile_t *profile,
    const uint8_t *data,
    size_t length)
{
    return profile != NULL && copy_bytes(
        profile->device_descriptor.data,
        sizeof(profile->device_descriptor.data),
        &profile->device_descriptor.length,
        data,
        length);
}

bool hid_device_profile_set_config_descriptor(
    hid_device_profile_t *profile,
    const uint8_t *data,
    size_t length)
{
    return profile != NULL && copy_bytes(
        profile->config_descriptor.data,
        sizeof(profile->config_descriptor.data),
        &profile->config_descriptor.length,
        data,
        length);
}

bool hid_device_profile_set_synthetic_identity(
    hid_device_profile_t *profile,
    uint16_t vid,
    uint16_t pid)
{
    if (profile == NULL) {
        return false;
    }
    const uint8_t descriptor[18] = {
        18, 1, 0x00, 0x02, 0, 0, 0, 64,
        (uint8_t)vid, (uint8_t)(vid >> 8),
        (uint8_t)pid, (uint8_t)(pid >> 8),
        0x00, 0x01, 1, 2, 3, 1,
    };
    if (!hid_device_profile_set_device_descriptor(profile, descriptor, sizeof(descriptor))) {
        return false;
    }
    profile->flags |= HID_PROFILE_FLAG_PARTIAL |
        HID_PROFILE_FLAG_SYNTHETIC_DEVICE_DESCRIPTOR |
        HID_PROFILE_FLAG_SYNTHETIC_CONFIG_DESCRIPTOR;
    return true;
}

void hid_device_profile_set_flags(hid_device_profile_t *profile, uint8_t flags)
{
    if (profile != NULL) {
        profile->flags = flags;
    }
}

bool hid_device_profile_set_string(
    hid_profile_string_t *destination,
    const char *data,
    size_t length)
{
    if (destination == NULL || length > HID_PROFILE_MAX_STRING_BYTES ||
        length > UINT16_MAX || (data == NULL && length != 0) ||
        !valid_utf8((const uint8_t *)data, length)) {
        return false;
    }
    if (length != 0) {
        memcpy(destination->data, data, length);
    }
    destination->data[length] = '\0';
    destination->length = (uint16_t)length;
    return true;
}

bool hid_device_profile_add_report_descriptor(
    hid_device_profile_t *profile,
    uint8_t interface_number,
    uint8_t subclass,
    uint8_t protocol,
    const uint8_t *data,
    size_t length)
{
    if (profile == NULL || profile->report_descriptor_count >= HID_PROFILE_MAX_REPORT_DESCRIPTORS ||
        !report_interface_is_unique(profile, interface_number) ||
        length > HID_PROFILE_MAX_REPORT_DESCRIPTOR || length > UINT16_MAX ||
        (data == NULL && length != 0)) {
        return false;
    }
    hid_profile_report_descriptor_t *report =
        &profile->report_descriptors[profile->report_descriptor_count];
    report->interface_number = interface_number;
    report->subclass = subclass;
    report->protocol = protocol;
    if (!copy_bytes(report->data, sizeof(report->data), &report->length, data, length)) {
        return false;
    }
    ++profile->report_descriptor_count;
    return true;
}

static bool profile_is_valid(const hid_device_profile_t *profile)
{
    if (profile == NULL || profile->flags & (uint8_t)~HID_PROFILE_KNOWN_FLAGS ||
        profile->report_descriptor_count > HID_PROFILE_MAX_REPORT_DESCRIPTORS ||
        profile->device_descriptor.length > HID_PROFILE_MAX_DEVICE_DESCRIPTOR ||
        profile->config_descriptor.length > HID_PROFILE_MAX_CONFIG_DESCRIPTOR ||
        profile->manufacturer.length > HID_PROFILE_MAX_STRING_BYTES ||
        profile->product.length > HID_PROFILE_MAX_STRING_BYTES ||
        profile->serial.length > HID_PROFILE_MAX_STRING_BYTES ||
        !valid_utf8((const uint8_t *)profile->manufacturer.data, profile->manufacturer.length) ||
        !valid_utf8((const uint8_t *)profile->product.data, profile->product.length) ||
        !valid_utf8((const uint8_t *)profile->serial.data, profile->serial.length)) {
        return false;
    }
    for (uint8_t index = 0; index < profile->report_descriptor_count; ++index) {
        if (profile->report_descriptors[index].length > HID_PROFILE_MAX_REPORT_DESCRIPTOR) {
            return false;
        }
        for (uint8_t previous = 0; previous < index; ++previous) {
            if (profile->report_descriptors[previous].interface_number ==
                profile->report_descriptors[index].interface_number) {
                return false;
            }
        }
    }
    return true;
}

bool hid_device_profile_serialize(
    const hid_device_profile_t *profile,
    uint8_t *output,
    size_t capacity,
    size_t *length)
{
    if (output == NULL || length == NULL || !profile_is_valid(profile)) {
        return false;
    }
    size_t required = HID_PROFILE_SERIAL_HEADER_SIZE +
        profile->device_descriptor.length + profile->config_descriptor.length +
        profile->manufacturer.length + profile->product.length + profile->serial.length;
    if (profile->report_descriptor_count > 0 &&
        required > SIZE_MAX - (size_t)profile->report_descriptor_count *
            HID_PROFILE_REPORT_ENTRY_SIZE) {
        return false;
    }
    required += (size_t)profile->report_descriptor_count * HID_PROFILE_REPORT_ENTRY_SIZE;
    for (uint8_t index = 0; index < profile->report_descriptor_count; ++index) {
        if (required > SIZE_MAX - profile->report_descriptors[index].length) {
            return false;
        }
        required += profile->report_descriptors[index].length;
    }
    if (required > HID_PROFILE_MAX_BLOB || capacity < required) {
        return false;
    }

    memset(output, 0, HID_PROFILE_SERIAL_HEADER_SIZE);
    write_u32_le(&output[0], HID_PROFILE_MAGIC);
    write_u16_le(&output[4], HID_PROFILE_VERSION);
    write_u16_le(&output[6], HID_PROFILE_SERIAL_HEADER_SIZE);
    write_u16_le(&output[8], profile->device_descriptor.length);
    write_u16_le(&output[10], profile->config_descriptor.length);
    write_u16_le(&output[12], profile->manufacturer.length);
    write_u16_le(&output[14], profile->product.length);
    write_u16_le(&output[16], profile->serial.length);
    output[18] = profile->report_descriptor_count;
    output[19] = profile->flags;

    size_t cursor = HID_PROFILE_SERIAL_HEADER_SIZE;
    if (profile->device_descriptor.length != 0) {
        memcpy(&output[cursor], profile->device_descriptor.data,
               profile->device_descriptor.length);
        cursor += profile->device_descriptor.length;
    }
    if (profile->config_descriptor.length != 0) {
        memcpy(&output[cursor], profile->config_descriptor.data,
               profile->config_descriptor.length);
        cursor += profile->config_descriptor.length;
    }
    const hid_profile_string_t *strings[] = {
        &profile->manufacturer, &profile->product, &profile->serial,
    };
    for (size_t index = 0; index < sizeof(strings) / sizeof(strings[0]); ++index) {
        if (strings[index]->length != 0) {
            memcpy(&output[cursor], strings[index]->data, strings[index]->length);
            cursor += strings[index]->length;
        }
    }
    for (uint8_t index = 0; index < profile->report_descriptor_count; ++index) {
        const hid_profile_report_descriptor_t *report = &profile->report_descriptors[index];
        output[cursor] = report->interface_number;
        output[cursor + 1U] = report->subclass;
        output[cursor + 2U] = report->protocol;
        output[cursor + 3U] = 0;
        write_u16_le(&output[cursor + 4U], report->length);
        cursor += HID_PROFILE_REPORT_ENTRY_SIZE;
        if (report->length != 0) {
            memcpy(&output[cursor], report->data, report->length);
            cursor += report->length;
        }
    }
    *length = cursor;
    return true;
}

bool hid_device_profile_deserialize(
    hid_device_profile_t *profile,
    const uint8_t *blob,
    size_t length)
{
    if (profile == NULL || blob == NULL || length < HID_PROFILE_SERIAL_HEADER_SIZE ||
        length > HID_PROFILE_MAX_BLOB || read_u32_le(blob) != HID_PROFILE_MAGIC ||
        read_u16_le(&blob[4]) != HID_PROFILE_VERSION ||
        read_u16_le(&blob[6]) != HID_PROFILE_SERIAL_HEADER_SIZE ||
        (blob[19] & (uint8_t)~HID_PROFILE_KNOWN_FLAGS) != 0) {
        return false;
    }
    const uint16_t device_length = read_u16_le(&blob[8]);
    const uint16_t config_length = read_u16_le(&blob[10]);
    const uint16_t manufacturer_length = read_u16_le(&blob[12]);
    const uint16_t product_length = read_u16_le(&blob[14]);
    const uint16_t serial_length = read_u16_le(&blob[16]);
    const uint8_t report_count = blob[18];
    const uint8_t flags = blob[19];
    if (device_length > HID_PROFILE_MAX_DEVICE_DESCRIPTOR ||
        config_length > HID_PROFILE_MAX_CONFIG_DESCRIPTOR ||
        manufacturer_length > HID_PROFILE_MAX_STRING_BYTES ||
        product_length > HID_PROFILE_MAX_STRING_BYTES ||
        serial_length > HID_PROFILE_MAX_STRING_BYTES ||
        report_count > HID_PROFILE_MAX_REPORT_DESCRIPTORS) {
        return false;
    }

    hid_device_profile_init(profile);
    profile->flags = flags;
    size_t cursor = HID_PROFILE_SERIAL_HEADER_SIZE;
    if (device_length > length - cursor) {
        return false;
    }
    if (!hid_device_profile_set_device_descriptor(profile, &blob[cursor], device_length)) {
        return false;
    }
    cursor += device_length;
    if (config_length > length - cursor) {
        return false;
    }
    if (!hid_device_profile_set_config_descriptor(profile, &blob[cursor], config_length)) {
        return false;
    }
    cursor += config_length;
    const uint16_t string_lengths[] = {
        manufacturer_length, product_length, serial_length,
    };
    hid_profile_string_t *strings[] = {
        &profile->manufacturer, &profile->product, &profile->serial,
    };
    for (size_t index = 0; index < sizeof(strings) / sizeof(strings[0]); ++index) {
        if (string_lengths[index] > length - cursor ||
            !hid_device_profile_set_string(strings[index], (const char *)&blob[cursor],
                                           string_lengths[index])) {
            return false;
        }
        cursor += string_lengths[index];
    }
    for (uint8_t index = 0; index < report_count; ++index) {
        if (length - cursor < HID_PROFILE_REPORT_ENTRY_SIZE) {
            return false;
        }
        const uint8_t interface_number = blob[cursor];
        const uint8_t subclass = blob[cursor + 1U];
        const uint8_t protocol = blob[cursor + 2U];
        const uint16_t report_length = read_u16_le(&blob[cursor + 4U]);
        if (blob[cursor + 3U] != 0 || report_length > HID_PROFILE_MAX_REPORT_DESCRIPTOR) {
            return false;
        }
        cursor += HID_PROFILE_REPORT_ENTRY_SIZE;
        if (report_length > length - cursor ||
            !hid_device_profile_add_report_descriptor(
                profile, interface_number, subclass, protocol,
                &blob[cursor], report_length)) {
            return false;
        }
        cursor += report_length;
    }
    if (cursor != length || !profile_is_valid(profile)) {
        return false;
    }
    return true;
}

bool hid_profile_stream_can_send(
    bool peer_online,
    bool safety_pending,
    bool motion_pending,
    uint8_t motion_sent_since_profile,
    uint8_t fairness_limit)
{
    if (!peer_online || safety_pending) {
        return false;
    }
    return !motion_pending || motion_sent_since_profile >= fairness_limit;
}

bool hid_profile_stream_needs_restart(
    bool generation_valid,
    uint16_t streamed_generation,
    uint16_t peer_generation)
{
    return !generation_valid || streamed_generation != peer_generation;
}

uint32_t hid_profile_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    if (data == NULL && length != 0) {
        return 0;
    }
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static void receiver_reset_transfer(hid_profile_receiver_t *receiver, bool clear_profile)
{
    receiver->active = false;
    receiver->transfer_id = 0;
    receiver->total_length = 0;
    receiver->expected_crc32 = 0;
    receiver->received_length = 0;
    if (clear_profile) {
        receiver->profile_valid = false;
        memset(&receiver->profile, 0, sizeof(receiver->profile));
    }
}

void hid_profile_receiver_init(
    hid_profile_receiver_t *receiver,
    hid_profile_publish_callback_t publish_callback,
    void *publish_context)
{
    if (receiver == NULL) {
        return;
    }
    memset(receiver, 0, sizeof(*receiver));
    receiver->publish_callback = publish_callback;
    receiver->publish_context = publish_context;
}

bool hid_profile_receiver_begin(
    hid_profile_receiver_t *receiver,
    uint32_t transfer_id,
    uint32_t total_length,
    uint32_t crc32)
{
    if (receiver == NULL) {
        return false;
    }
    /* Keep the last published profile until a complete replacement commits. */
    receiver_reset_transfer(receiver, false);
    if (total_length == 0 || total_length > HID_PROFILE_MAX_BLOB) {
        return false;
    }
    receiver->transfer_id = transfer_id;
    receiver->total_length = total_length;
    receiver->expected_crc32 = crc32;
    receiver->active = true;
    return true;
}

bool hid_profile_receiver_chunk(
    hid_profile_receiver_t *receiver,
    uint32_t transfer_id,
    uint32_t offset,
    const uint8_t *data,
    size_t length)
{
    if (receiver == NULL || !receiver->active || transfer_id != receiver->transfer_id ||
        length == 0 || length > HID_PROFILE_MAX_CHUNK_DATA ||
        offset != receiver->received_length || offset > receiver->total_length ||
        length > receiver->total_length - offset || (data == NULL && length != 0)) {
        if (receiver != NULL) {
            receiver_reset_transfer(receiver, false);
        }
        return false;
    }
    memcpy(&receiver->blob[offset], data, length);
    receiver->received_length += (uint32_t)length;
    return true;
}

bool hid_profile_receiver_commit(
    hid_profile_receiver_t *receiver,
    uint32_t transfer_id,
    uint32_t total_length,
    uint32_t crc32)
{
    if (receiver == NULL || !receiver->active || transfer_id != receiver->transfer_id ||
        total_length != receiver->total_length || crc32 != receiver->expected_crc32 ||
        receiver->received_length != receiver->total_length ||
        hid_profile_crc32(receiver->blob, receiver->total_length) != receiver->expected_crc32) {
        if (receiver != NULL) {
            receiver_reset_transfer(receiver, false);
        }
        return false;
    }
    if (!hid_device_profile_deserialize(
            &receiver->staging_profile, receiver->blob, receiver->total_length)) {
        receiver_reset_transfer(receiver, false);
        return false;
    }
    receiver->profile = receiver->staging_profile;
    receiver->profile_valid = true;
    receiver->active = false;
    if (receiver->publish_callback != NULL) {
        receiver->publish_callback(&receiver->profile, receiver->publish_context);
    }
    return true;
}

bool hid_profile_receiver_accept_frame(
    hid_profile_receiver_t *receiver,
    const dual_frame_t *frame)
{
    if (receiver == NULL || frame == NULL) {
        return false;
    }
    if (frame->version != DUAL_PROXY_PROTOCOL_VERSION) {
        receiver_reset_transfer(receiver, false);
        return false;
    }
    if (frame->type == DUAL_MESSAGE_PROFILE_BEGIN) {
        if (frame->payload_length != 12U) {
            receiver_reset_transfer(receiver, false);
            return false;
        }
        return hid_profile_receiver_begin(
                receiver,
                read_u32_le(&frame->payload[0]),
                read_u32_le(&frame->payload[4]),
                read_u32_le(&frame->payload[8]));
    }
    if (frame->type == DUAL_MESSAGE_PROFILE_CHUNK) {
        if (frame->payload_length <= HID_PROFILE_FRAME_CHUNK_HEADER) {
            receiver_reset_transfer(receiver, false);
            return false;
        }
        return hid_profile_receiver_chunk(
                receiver,
                read_u32_le(&frame->payload[0]),
                read_u32_le(&frame->payload[4]),
                &frame->payload[HID_PROFILE_FRAME_CHUNK_HEADER],
                frame->payload_length - HID_PROFILE_FRAME_CHUNK_HEADER);
    }
    if (frame->type == DUAL_MESSAGE_PROFILE_COMMIT) {
        if (frame->payload_length != 12U) {
            receiver_reset_transfer(receiver, false);
            return false;
        }
        return hid_profile_receiver_commit(
                receiver,
                read_u32_le(&frame->payload[0]),
                read_u32_le(&frame->payload[4]),
                read_u32_le(&frame->payload[8]));
    }
    return false;
}

bool hid_profile_receiver_is_active(const hid_profile_receiver_t *receiver)
{
    return receiver != NULL && receiver->active;
}

bool hid_profile_receiver_has_profile(const hid_profile_receiver_t *receiver)
{
    return receiver != NULL && receiver->profile_valid;
}

const hid_device_profile_t *hid_profile_receiver_get_profile(
    const hid_profile_receiver_t *receiver)
{
    return hid_profile_receiver_has_profile(receiver) ? &receiver->profile : NULL;
}
