#include "bridge_protocol.h"

#include <stdbool.h>
#include <string.h>

static void parser_reset(dual_parser_t *parser)
{
    parser->length = 0;
    parser->expected_length = 0;
}

static void parser_restart_from_byte(dual_parser_t *parser, uint8_t value)
{
    parser_reset(parser);
    if (value == 0xA5) {
        parser->buffer[0] = value;
        parser->length = 1;
    }
}

static bool frame_is_valid(const uint8_t *buffer, size_t length)
{
    if (length < 9 || buffer[0] != 0xA5 || buffer[1] != 0x5A ||
        buffer[2] != DUAL_PROXY_PROTOCOL_VERSION || buffer[6] > DUAL_PROXY_MAX_PAYLOAD) {
        return false;
    }
    const size_t payload_length = buffer[6];
    const size_t crc_offset = 7 + payload_length;
    if (length != crc_offset + 2) {
        return false;
    }
    const uint16_t expected = (uint16_t)buffer[crc_offset] |
        ((uint16_t)buffer[crc_offset + 1] << 8);
    return expected == dual_crc16_ccitt(&buffer[2], 5 + payload_length);
}

void dual_parser_init(dual_parser_t *parser, dual_frame_callback_t callback, void *context)
{
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
    parser->callback = callback;
    parser->callback_context = context;
}

void dual_parser_feed(dual_parser_t *parser, const uint8_t *data, size_t length)
{
    if (parser == NULL || data == NULL || parser->callback == NULL) {
        return;
    }
    for (size_t index = 0; index < length; ++index) {
        const uint8_t value = data[index];
        if (parser->length == 0) {
            if (value == 0xA5) {
                parser->buffer[parser->length++] = value;
            }
            continue;
        }
        if (parser->length == 1) {
            if (value == 0x5A) {
                parser->buffer[parser->length++] = value;
            } else {
                parser_restart_from_byte(parser, value);
            }
            continue;
        }
        if (parser->length >= sizeof(parser->buffer)) {
            parser_restart_from_byte(parser, value);
            continue;
        }
        parser->buffer[parser->length++] = value;
        if (parser->length == 7) {
            if (parser->buffer[2] != DUAL_PROXY_PROTOCOL_VERSION ||
                parser->buffer[6] > DUAL_PROXY_MAX_PAYLOAD) {
                parser_restart_from_byte(parser, value);
                continue;
            }
            parser->expected_length = 9 + parser->buffer[6];
        }
        if (parser->expected_length > 0 && parser->length == parser->expected_length) {
            if (frame_is_valid(parser->buffer, parser->length)) {
                dual_frame_t frame = {
                    .version = parser->buffer[2],
                    .type = parser->buffer[3],
                    .sequence = (uint16_t)parser->buffer[4] |
                        ((uint16_t)parser->buffer[5] << 8),
                    .payload_length = parser->buffer[6],
                };
                if (frame.payload_length > 0) {
                    memcpy(frame.payload, &parser->buffer[7], frame.payload_length);
                }
                parser->callback(&frame, parser->callback_context);
            }
            parser_reset(parser);
        }
    }
}

uint16_t dual_crc16_ccitt(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t index = 0; index < length; ++index) {
        crc ^= (uint16_t)data[index] << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U)
                ? (uint16_t)((crc << 1) ^ 0x1021U)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t dual_frame_serialize(const dual_frame_t *frame, uint8_t *output, size_t capacity, size_t *length)
{
    if (frame == NULL || output == NULL || length == NULL ||
        frame->payload_length > DUAL_PROXY_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t frame_length = 9 + frame->payload_length;
    if (capacity < frame_length) {
        return ESP_ERR_INVALID_SIZE;
    }
    output[0] = 0xA5;
    output[1] = 0x5A;
    output[2] = frame->version;
    output[3] = frame->type;
    output[4] = (uint8_t)frame->sequence;
    output[5] = (uint8_t)(frame->sequence >> 8);
    output[6] = frame->payload_length;
    memcpy(&output[7], frame->payload, frame->payload_length);
    const uint16_t crc = dual_crc16_ccitt(&output[2], 5 + frame->payload_length);
    output[7 + frame->payload_length] = (uint8_t)crc;
    output[8 + frame->payload_length] = (uint8_t)(crc >> 8);
    *length = frame_length;
    return ESP_OK;
}

static void write_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static uint16_t read_u16_le(const uint8_t *input)
{
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8);
}

static void write_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static uint32_t read_u32_le(const uint8_t *input)
{
    return (uint32_t)input[0] |
        ((uint32_t)input[1] << 8) |
        ((uint32_t)input[2] << 16) |
        ((uint32_t)input[3] << 24);
}

static bool valid_report_type(uint8_t report_type)
{
    return report_type >= DUAL_HID_REPORT_TYPE_INPUT &&
        report_type <= DUAL_HID_REPORT_TYPE_FEATURE;
}

static bool encode_report_payload(
    uint8_t header_length,
    uint8_t *payload,
    size_t capacity,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload_length)
{
    if (payload == NULL || payload_length == NULL ||
        capacity < (size_t)header_length + data_length ||
        data_length > DUAL_HID_CONTROL_MAX_DATA ||
        (data == NULL && data_length != 0) ||
        header_length + data_length > DUAL_PROXY_MAX_PAYLOAD) {
        return false;
    }
    if (data_length != 0) {
        memcpy(&payload[header_length], data, data_length);
    }
    *payload_length = (uint8_t)(header_length + data_length);
    return true;
}

bool dual_hid_raw_input_encode(
    uint8_t interface_number,
    uint8_t report_id,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length)
{
    if (payload == NULL || payload_length == NULL ||
        data_length > DUAL_HID_RAW_INPUT_MAX_DATA ||
        capacity < DUAL_HID_RAW_INPUT_HEADER_LENGTH + data_length ||
        (data == NULL && data_length != 0)) {
        return false;
    }
    payload[0] = interface_number;
    payload[1] = report_id;
    payload[2] = (uint8_t)data_length;
    if (data_length != 0) {
        memcpy(&payload[DUAL_HID_RAW_INPUT_HEADER_LENGTH], data, data_length);
    }
    *payload_length = (uint8_t)(DUAL_HID_RAW_INPUT_HEADER_LENGTH + data_length);
    return true;
}

bool dual_hid_raw_input_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *interface_number,
    uint8_t *report_id,
    const uint8_t **data,
    size_t *data_length)
{
    if (payload == NULL || payload_length < DUAL_HID_RAW_INPUT_HEADER_LENGTH ||
        payload[2] > DUAL_HID_RAW_INPUT_MAX_DATA ||
        payload_length != DUAL_HID_RAW_INPUT_HEADER_LENGTH + payload[2] ||
        interface_number == NULL || report_id == NULL || data == NULL ||
        data_length == NULL) {
        return false;
    }
    *interface_number = payload[0];
    *report_id = payload[1];
    *data = &payload[DUAL_HID_RAW_INPUT_HEADER_LENGTH];
    *data_length = payload[2];
    return true;
}

bool dual_hid_set_report_encode(
    uint16_t transaction_id,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length)
{
    if (payload == NULL || !valid_report_type(report_type) ||
        capacity < DUAL_HID_SET_REPORT_HEADER_LENGTH + data_length ||
        !encode_report_payload(DUAL_HID_SET_REPORT_HEADER_LENGTH, payload, capacity,
                               data, data_length, payload_length)) {
        return false;
    }
    write_u16_le(payload, transaction_id);
    payload[2] = interface_number;
    payload[3] = report_id;
    payload[4] = report_type;
    payload[5] = (uint8_t)data_length;
    return true;
}

bool dual_hid_set_report_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *interface_number,
    uint8_t *report_id,
    uint8_t *report_type,
    const uint8_t **data,
    size_t *data_length)
{
    if (payload == NULL || payload_length < DUAL_HID_SET_REPORT_HEADER_LENGTH ||
        payload[5] > DUAL_HID_CONTROL_MAX_DATA ||
        payload_length != DUAL_HID_SET_REPORT_HEADER_LENGTH + payload[5] ||
        !valid_report_type(payload[4]) || transaction_id == NULL ||
        interface_number == NULL || report_id == NULL || report_type == NULL ||
        data == NULL || data_length == NULL) {
        return false;
    }
    *transaction_id = read_u16_le(payload);
    *interface_number = payload[2];
    *report_id = payload[3];
    *report_type = payload[4];
    *data = &payload[DUAL_HID_SET_REPORT_HEADER_LENGTH];
    *data_length = payload[5];
    return true;
}

/*
 * 设备级 Vendor 控制请求/响应编解码（2026-09-27）。
 * 与 SET_REPORT 那套的区别：请求里带完整的 bmRequestType/bRequest/wValue/wIndex，
 * 这样 M 侧可以原样构造任意 EP0 控制传输，而不只是 HID 类 SET_REPORT。
 */
bool dual_vendor_control_request_encode(
    uint16_t transaction_id,
    uint8_t bm_request_type,
    uint8_t b_request,
    uint16_t w_value,
    uint16_t w_index,
    uint16_t w_length,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length)
{
    if (payload == NULL || payload_length == NULL ||
        data_length > DUAL_VENDOR_CONTROL_MAX_DATA ||
        w_length > DUAL_VENDOR_CONTROL_MAX_DATA ||
        capacity < DUAL_VENDOR_CONTROL_REQUEST_HEADER_LENGTH + data_length ||
        (data == NULL && data_length != 0U) ||
        /* OUT 必须自带与 wLength 等长的数据；IN 不携带数据。 */
        (((bm_request_type & 0x80U) == 0U) ? (data_length != (size_t)w_length)
                                           : (data_length != 0U))) {
        return false;
    }
    write_u16_le(payload, transaction_id);
    payload[2] = bm_request_type;
    payload[3] = b_request;
    write_u16_le(&payload[4], w_value);
    write_u16_le(&payload[6], w_index);
    write_u16_le(&payload[8], (uint16_t)data_length);
    write_u16_le(&payload[10], w_length);
    if (data_length != 0U) {
        memcpy(&payload[DUAL_VENDOR_CONTROL_REQUEST_HEADER_LENGTH], data, data_length);
    }
    *payload_length = (uint8_t)(DUAL_VENDOR_CONTROL_REQUEST_HEADER_LENGTH + data_length);
    return true;
}

bool dual_vendor_control_request_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *bm_request_type,
    uint8_t *b_request,
    uint16_t *w_value,
    uint16_t *w_index,
    uint16_t *w_length,
    const uint8_t **data,
    size_t *data_length)
{
    if (payload == NULL || transaction_id == NULL || bm_request_type == NULL ||
        b_request == NULL || w_value == NULL || w_index == NULL ||
        w_length == NULL || data == NULL || data_length == NULL ||
        payload_length < DUAL_VENDOR_CONTROL_REQUEST_HEADER_LENGTH) {
        return false;
    }
    const uint16_t carried = read_u16_le(&payload[8]);
    const uint16_t requested = read_u16_le(&payload[10]);
    if (carried > DUAL_VENDOR_CONTROL_MAX_DATA ||
        requested > DUAL_VENDOR_CONTROL_MAX_DATA ||
        payload_length != DUAL_VENDOR_CONTROL_REQUEST_HEADER_LENGTH + carried ||
        (((payload[2] & 0x80U) == 0U) ? (carried != requested) : (carried != 0U))) {
        return false;
    }
    *transaction_id = read_u16_le(payload);
    *bm_request_type = payload[2];
    *b_request = payload[3];
    *w_value = read_u16_le(&payload[4]);
    *w_index = read_u16_le(&payload[6]);
    *w_length = requested;
    *data = &payload[DUAL_VENDOR_CONTROL_REQUEST_HEADER_LENGTH];
    *data_length = carried;
    return true;
}

bool dual_vendor_control_response_encode(
    uint16_t transaction_id,
    uint8_t status,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length)
{
    if (payload == NULL || payload_length == NULL ||
        data_length > DUAL_VENDOR_CONTROL_MAX_DATA ||
        capacity < DUAL_VENDOR_CONTROL_RESPONSE_HEADER_LENGTH + data_length ||
        (data == NULL && data_length != 0U)) {
        return false;
    }
    write_u16_le(payload, transaction_id);
    payload[2] = status;
    write_u16_le(&payload[3], (uint16_t)data_length);
    if (data_length != 0U) {
        memcpy(&payload[DUAL_VENDOR_CONTROL_RESPONSE_HEADER_LENGTH], data, data_length);
    }
    *payload_length = (uint8_t)(DUAL_VENDOR_CONTROL_RESPONSE_HEADER_LENGTH + data_length);
    return true;
}

bool dual_vendor_control_response_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *status,
    const uint8_t **data,
    size_t *data_length)
{
    if (payload == NULL || transaction_id == NULL || status == NULL ||
        data == NULL || data_length == NULL ||
        payload_length < DUAL_VENDOR_CONTROL_RESPONSE_HEADER_LENGTH) {
        return false;
    }
    const uint16_t declared = read_u16_le(&payload[3]);
    if (declared > DUAL_VENDOR_CONTROL_MAX_DATA ||
        payload_length != DUAL_VENDOR_CONTROL_RESPONSE_HEADER_LENGTH + declared) {
        return false;
    }
    *transaction_id = read_u16_le(payload);
    *status = payload[2];
    *data = &payload[DUAL_VENDOR_CONTROL_RESPONSE_HEADER_LENGTH];
    *data_length = declared;
    return true;
}

bool dual_hid_get_request_encode(
    uint16_t transaction_id,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    uint8_t requested_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length)
{
    if (payload == NULL || payload_length == NULL ||
        capacity < DUAL_HID_GET_REPORT_REQUEST_LENGTH ||
        !valid_report_type(report_type) || requested_length == 0U ||
        requested_length > DUAL_HID_CONTROL_MAX_DATA) {
        return false;
    }
    write_u16_le(payload, transaction_id);
    payload[2] = interface_number;
    payload[3] = report_id;
    payload[4] = report_type;
    payload[5] = requested_length;
    *payload_length = DUAL_HID_GET_REPORT_REQUEST_LENGTH;
    return true;
}

bool dual_hid_get_request_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *interface_number,
    uint8_t *report_id,
    uint8_t *report_type,
    uint8_t *requested_length)
{
    if (payload == NULL || payload_length != DUAL_HID_GET_REPORT_REQUEST_LENGTH ||
        !valid_report_type(payload[4]) || payload[5] == 0U ||
        payload[5] > DUAL_HID_CONTROL_MAX_DATA || transaction_id == NULL ||
        interface_number == NULL || report_id == NULL || report_type == NULL ||
        requested_length == NULL) {
        return false;
    }
    *transaction_id = read_u16_le(payload);
    *interface_number = payload[2];
    *report_id = payload[3];
    *report_type = payload[4];
    *requested_length = payload[5];
    return true;
}

bool dual_hid_get_response_encode(
    uint16_t transaction_id,
    uint8_t status,
    uint8_t interface_number,
    uint8_t report_id,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length)
{
    if (payload == NULL || payload_length == NULL ||
        status > DUAL_HID_REPORT_STATUS_UNSUPPORTED ||
        capacity < DUAL_HID_GET_REPORT_RESPONSE_HEADER_LENGTH + data_length ||
        !encode_report_payload(DUAL_HID_GET_REPORT_RESPONSE_HEADER_LENGTH, payload,
                               capacity, data, data_length, payload_length)) {
        return false;
    }
    if (status != DUAL_HID_REPORT_STATUS_OK && data_length != 0U) {
        return false;
    }
    write_u16_le(payload, transaction_id);
    payload[2] = status;
    payload[3] = interface_number;
    payload[4] = report_id;
    payload[5] = (uint8_t)data_length;
    return true;
}

bool dual_hid_get_response_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *status,
    uint8_t *interface_number,
    uint8_t *report_id,
    const uint8_t **data,
    size_t *data_length)
{
    if (payload == NULL || payload_length < DUAL_HID_GET_REPORT_RESPONSE_HEADER_LENGTH ||
        payload[2] > DUAL_HID_REPORT_STATUS_UNSUPPORTED ||
        payload[5] > DUAL_HID_CONTROL_MAX_DATA ||
        payload_length != DUAL_HID_GET_REPORT_RESPONSE_HEADER_LENGTH + payload[5] ||
        (payload[2] != DUAL_HID_REPORT_STATUS_OK && payload[5] != 0U) ||
        transaction_id == NULL || status == NULL || interface_number == NULL ||
        report_id == NULL || data == NULL || data_length == NULL) {
        return false;
    }
    *transaction_id = read_u16_le(payload);
    *status = payload[2];
    *interface_number = payload[3];
    *report_id = payload[4];
    *data = &payload[DUAL_HID_GET_REPORT_RESPONSE_HEADER_LENGTH];
    *data_length = payload[5];
    return true;
}

bool dual_log_read_request_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint32_t *offset,
    uint8_t *max_bytes)
{
    if (payload == NULL || offset == NULL || max_bytes == NULL ||
        payload_length != DUAL_LOG_READ_REQUEST_LENGTH || payload[4] == 0U) {
        return false;
    }
    *offset = read_u32_le(payload);
    *max_bytes = payload[4];
    return true;
}

bool dual_log_read_response_encode(
    uint32_t offset,
    uint32_t total_bytes,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length)
{
    if (payload == NULL || payload_length == NULL ||
        data_length > DUAL_PROXY_MAX_PAYLOAD - DUAL_LOG_READ_RESPONSE_HEADER_LENGTH ||
        (data == NULL && data_length != 0U) ||
        capacity < DUAL_LOG_READ_RESPONSE_HEADER_LENGTH + data_length) {
        return false;
    }
    write_u32_le(&payload[0], offset);
    write_u32_le(&payload[4], total_bytes);
    if (data_length > 0U) {
        memcpy(&payload[DUAL_LOG_READ_RESPONSE_HEADER_LENGTH], data, data_length);
    }
    *payload_length = (uint8_t)(DUAL_LOG_READ_RESPONSE_HEADER_LENGTH + data_length);
    return true;
}

bool dual_log_read_response_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint32_t *offset,
    uint32_t *total_bytes,
    const uint8_t **data,
    size_t *data_length)
{
    if (payload == NULL || offset == NULL || total_bytes == NULL || data == NULL ||
        data_length == NULL || payload_length < DUAL_LOG_READ_RESPONSE_HEADER_LENGTH) {
        return false;
    }
    *offset = read_u32_le(&payload[0]);
    *total_bytes = read_u32_le(&payload[4]);
    *data = &payload[DUAL_LOG_READ_RESPONSE_HEADER_LENGTH];
    *data_length = payload_length - DUAL_LOG_READ_RESPONSE_HEADER_LENGTH;
    return true;
}
