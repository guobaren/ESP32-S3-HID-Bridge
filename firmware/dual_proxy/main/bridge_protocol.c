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
