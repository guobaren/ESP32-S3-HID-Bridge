#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define DUAL_PROXY_PROTOCOL_VERSION 2
#define DUAL_PROXY_MAX_PAYLOAD 64

typedef enum {
    DUAL_MESSAGE_KEYBOARD_REPORT = 0x01,
    DUAL_MESSAGE_MOUSE_REPORT = 0x02,
    DUAL_MESSAGE_RELEASE_ALL = 0x03,
    DUAL_MESSAGE_PING = 0x04,
    DUAL_MESSAGE_SESSION_START = 0x05,
    DUAL_MESSAGE_DEVICE_PROBE = 0x06,
    DUAL_MESSAGE_DEVICE_HELLO = 0x07,
    DUAL_MESSAGE_LINK_HELLO = 0x20,
    DUAL_MESSAGE_PHYSICAL_MOUSE = 0x21,
    DUAL_MESSAGE_PHYSICAL_RELEASE = 0x22,
    DUAL_MESSAGE_LINK_PING = 0x23,
    DUAL_MESSAGE_PROFILE_BEGIN = 0x24,
    DUAL_MESSAGE_PROFILE_CHUNK = 0x25,
    DUAL_MESSAGE_PROFILE_COMMIT = 0x26,
    DUAL_MESSAGE_RAW_HID_INPUT = 0x27,
    DUAL_MESSAGE_HID_SET_REPORT = 0x28,
    DUAL_MESSAGE_HID_GET_REPORT_REQUEST = 0x29,
    DUAL_MESSAGE_HID_GET_REPORT_RESPONSE = 0x2A,
} dual_message_type_t;

#define DUAL_HID_RAW_INPUT_HEADER_LENGTH 3U
#define DUAL_HID_SET_REPORT_HEADER_LENGTH 6U
#define DUAL_HID_GET_REPORT_REQUEST_LENGTH 6U
#define DUAL_HID_GET_REPORT_RESPONSE_HEADER_LENGTH 6U
#define DUAL_HID_RAW_INPUT_MAX_DATA \
    (DUAL_PROXY_MAX_PAYLOAD - DUAL_HID_RAW_INPUT_HEADER_LENGTH)
#define DUAL_HID_CONTROL_MAX_DATA \
    (DUAL_PROXY_MAX_PAYLOAD - DUAL_HID_SET_REPORT_HEADER_LENGTH)

typedef enum {
    DUAL_HID_REPORT_TYPE_INPUT = 1,
    DUAL_HID_REPORT_TYPE_OUTPUT = 2,
    DUAL_HID_REPORT_TYPE_FEATURE = 3,
} dual_hid_report_type_t;

typedef enum {
    DUAL_HID_REPORT_STATUS_OK = 0,
    DUAL_HID_REPORT_STATUS_INVALID = 1,
    DUAL_HID_REPORT_STATUS_TIMEOUT = 2,
    DUAL_HID_REPORT_STATUS_UNSUPPORTED = 3,
} dual_hid_report_status_t;

typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t sequence;
    uint8_t payload_length;
    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD];
} dual_frame_t;

typedef void (*dual_frame_callback_t)(const dual_frame_t *frame, void *context);

typedef struct {
    uint8_t buffer[9 + DUAL_PROXY_MAX_PAYLOAD];
    size_t length;
    size_t expected_length;
    dual_frame_callback_t callback;
    void *callback_context;
} dual_parser_t;

void dual_parser_init(dual_parser_t *parser, dual_frame_callback_t callback, void *context);
void dual_parser_feed(dual_parser_t *parser, const uint8_t *data, size_t length);
uint16_t dual_crc16_ccitt(const uint8_t *data, size_t length);
esp_err_t dual_frame_serialize(const dual_frame_t *frame, uint8_t *output, size_t capacity, size_t *length);

bool dual_hid_raw_input_encode(
    uint8_t interface_number,
    uint8_t report_id,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length);
bool dual_hid_raw_input_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *interface_number,
    uint8_t *report_id,
    const uint8_t **data,
    size_t *data_length);

bool dual_hid_set_report_encode(
    uint16_t transaction_id,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length);
bool dual_hid_set_report_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *interface_number,
    uint8_t *report_id,
    uint8_t *report_type,
    const uint8_t **data,
    size_t *data_length);

bool dual_hid_get_request_encode(
    uint16_t transaction_id,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    uint8_t requested_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length);
bool dual_hid_get_request_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *interface_number,
    uint8_t *report_id,
    uint8_t *report_type,
    uint8_t *requested_length);

bool dual_hid_get_response_encode(
    uint16_t transaction_id,
    uint8_t status,
    uint8_t interface_number,
    uint8_t report_id,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length);
bool dual_hid_get_response_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *status,
    uint8_t *interface_number,
    uint8_t *report_id,
    const uint8_t **data,
    size_t *data_length);
