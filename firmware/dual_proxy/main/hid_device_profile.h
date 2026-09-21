#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_protocol.h"

/*
 * This is a bounded, transport-neutral profile model.  It deliberately does
 * not contain a default mouse, VID/PID, or Logitech-specific layout.
 */
#define HID_PROFILE_MAGIC 0x50444948U /* little-endian bytes: HIDP */
#define HID_PROFILE_VERSION 2U
#define HID_PROFILE_MAX_BLOB 4096U
#define HID_PROFILE_SERIAL_HEADER_SIZE 20U
#define HID_PROFILE_REPORT_ENTRY_SIZE 6U
#define HID_PROFILE_MAX_DEVICE_DESCRIPTOR 64U
#define HID_PROFILE_MAX_CONFIG_DESCRIPTOR 2048U
#define HID_PROFILE_MAX_STRING_BYTES 128U
#define HID_PROFILE_MAX_REPORT_DESCRIPTORS 8U
#define HID_PROFILE_MAX_REPORT_DESCRIPTOR 512U
#define HID_PROFILE_FRAME_CHUNK_HEADER 8U
#define HID_PROFILE_MAX_CHUNK_DATA \
    (DUAL_PROXY_MAX_PAYLOAD - HID_PROFILE_FRAME_CHUNK_HEADER)

/* The Host HID API exposes identity strings and VID/PID, but not the raw
 * device/configuration descriptor through the public handle.  These flags
 * make that limitation explicit to a future clone/re-enumeration consumer. */
#define HID_PROFILE_FLAG_PARTIAL 0x01U
#define HID_PROFILE_FLAG_SYNTHETIC_DEVICE_DESCRIPTOR 0x02U
#define HID_PROFILE_FLAG_SYNTHETIC_CONFIG_DESCRIPTOR 0x04U
#define HID_PROFILE_KNOWN_FLAGS (HID_PROFILE_FLAG_PARTIAL | \
                                 HID_PROFILE_FLAG_SYNTHETIC_DEVICE_DESCRIPTOR | \
                                 HID_PROFILE_FLAG_SYNTHETIC_CONFIG_DESCRIPTOR)

typedef struct {
    uint16_t length;
    uint8_t data[HID_PROFILE_MAX_DEVICE_DESCRIPTOR];
} hid_profile_device_descriptor_t;

typedef struct {
    uint16_t length;
    uint8_t data[HID_PROFILE_MAX_CONFIG_DESCRIPTOR];
} hid_profile_config_descriptor_t;

typedef struct {
    uint16_t length;
    char data[HID_PROFILE_MAX_STRING_BYTES + 1U];
} hid_profile_string_t;

typedef struct {
    uint8_t interface_number;
    uint8_t subclass;
    uint8_t protocol;
    uint16_t length;
    uint8_t data[HID_PROFILE_MAX_REPORT_DESCRIPTOR];
} hid_profile_report_descriptor_t;

typedef struct {
    hid_profile_device_descriptor_t device_descriptor;
    hid_profile_config_descriptor_t config_descriptor;
    hid_profile_string_t manufacturer;
    hid_profile_string_t product;
    hid_profile_string_t serial;
    uint8_t flags;
    uint8_t report_descriptor_count;
    hid_profile_report_descriptor_t report_descriptors[HID_PROFILE_MAX_REPORT_DESCRIPTORS];
} hid_device_profile_t;

typedef void (*hid_profile_publish_callback_t)(
    const hid_device_profile_t *profile,
    void *context);

typedef struct {
    uint8_t blob[HID_PROFILE_MAX_BLOB];
    uint32_t transfer_id;
    uint32_t total_length;
    uint32_t expected_crc32;
    uint32_t received_length;
    bool active;
    bool profile_valid;
    /* Decode staging lives in static receiver storage, not the 4 KB UART task stack. */
    hid_device_profile_t staging_profile;
    hid_device_profile_t profile;
    hid_profile_publish_callback_t publish_callback;
    void *publish_context;
} hid_profile_receiver_t;

void hid_device_profile_init(hid_device_profile_t *profile);
bool hid_device_profile_set_device_descriptor(
    hid_device_profile_t *profile,
    const uint8_t *data,
    size_t length);
bool hid_device_profile_set_config_descriptor(
    hid_device_profile_t *profile,
    const uint8_t *data,
    size_t length);
bool hid_device_profile_set_synthetic_identity(
    hid_device_profile_t *profile,
    uint16_t vid,
    uint16_t pid);
void hid_device_profile_set_flags(hid_device_profile_t *profile, uint8_t flags);
bool hid_device_profile_set_string(
    hid_profile_string_t *destination,
    const char *data,
    size_t length);
bool hid_device_profile_add_report_descriptor(
    hid_device_profile_t *profile,
    uint8_t interface_number,
    uint8_t subclass,
    uint8_t protocol,
    const uint8_t *data,
    size_t length);

/* Pure scheduling predicate shared by UART1 streaming and host tests. */
bool hid_profile_stream_can_send(
    bool peer_online,
    bool safety_pending,
    bool motion_pending,
    uint8_t motion_sent_since_profile,
    uint8_t fairness_limit);
bool hid_profile_stream_needs_restart(
    bool generation_valid,
    uint16_t streamed_generation,
    uint16_t peer_generation);

bool hid_device_profile_serialize(
    const hid_device_profile_t *profile,
    uint8_t *output,
    size_t capacity,
    size_t *length);
bool hid_device_profile_deserialize(
    hid_device_profile_t *profile,
    const uint8_t *blob,
    size_t length);

uint32_t hid_profile_crc32(const uint8_t *data, size_t length);

void hid_profile_receiver_init(
    hid_profile_receiver_t *receiver,
    hid_profile_publish_callback_t publish_callback,
    void *publish_context);
bool hid_profile_receiver_begin(
    hid_profile_receiver_t *receiver,
    uint32_t transfer_id,
    uint32_t total_length,
    uint32_t crc32);
bool hid_profile_receiver_chunk(
    hid_profile_receiver_t *receiver,
    uint32_t transfer_id,
    uint32_t offset,
    const uint8_t *data,
    size_t length);
bool hid_profile_receiver_commit(
    hid_profile_receiver_t *receiver,
    uint32_t transfer_id,
    uint32_t total_length,
    uint32_t crc32);
bool hid_profile_receiver_accept_frame(
    hid_profile_receiver_t *receiver,
    const dual_frame_t *frame);
bool hid_profile_receiver_is_active(const hid_profile_receiver_t *receiver);
bool hid_profile_receiver_has_profile(const hid_profile_receiver_t *receiver);
const hid_device_profile_t *hid_profile_receiver_get_profile(
    const hid_profile_receiver_t *receiver);
