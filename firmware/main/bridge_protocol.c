#include "bridge_protocol.h"

#include <stdbool.h>
#include <string.h>

static void parser_reset(bridge_parser_t *parser)
{
    parser->length = 0;
    parser->expected_length = 0;
}

static void parser_restart_from_byte(bridge_parser_t *parser, uint8_t value)
{
    parser_reset(parser);
    if (value == 0xA5) {
        parser->buffer[0] = value;
        parser->length = 1;
    }
}

static void dispatch_if_valid(bridge_parser_t *parser)
{
    const uint8_t *buffer = parser->buffer;
    const uint8_t payload_length = buffer[6];
    const size_t crc_offset = 7 + payload_length;
    const uint16_t expected_crc =
        (uint16_t)buffer[crc_offset] |
        ((uint16_t)buffer[crc_offset + 1] << 8);
    const uint16_t actual_crc = bridge_crc16_ccitt(&buffer[2], 5 + payload_length);

    if (buffer[2] != BRIDGE_PROTOCOL_VERSION || expected_crc != actual_crc) {
        return;
    }

    bridge_frame_t frame = {
        .version = buffer[2],
        .type = (bridge_message_type_t)buffer[3],
        .sequence = (uint16_t)buffer[4] | ((uint16_t)buffer[5] << 8),
        .payload_length = payload_length,
    };
    if (payload_length > 0) {
        memcpy(frame.payload, &buffer[7], payload_length);
    }

    parser->callback(&frame, parser->callback_context);
}

void bridge_parser_init(
    bridge_parser_t *parser,
    bridge_frame_callback_t callback,
    void *callback_context)
{
    memset(parser, 0, sizeof(*parser));
    parser->callback = callback;
    parser->callback_context = callback_context;
}

void bridge_parser_feed(bridge_parser_t *parser, const uint8_t *data, size_t length)
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
            const uint8_t payload_length = parser->buffer[6];
            if (payload_length > BRIDGE_MAX_PAYLOAD) {
                parser_restart_from_byte(parser, value);
                continue;
            }
            parser->expected_length = 9 + payload_length;
        }

        if (parser->expected_length > 0 && parser->length == parser->expected_length) {
            dispatch_if_valid(parser);
            parser_reset(parser);
        }
    }
}

uint16_t bridge_crc16_ccitt(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t index = 0; index < length; ++index) {
        crc ^= (uint16_t)data[index] << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000)
                ? (uint16_t)((crc << 1) ^ 0x1021)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t bridge_frame_serialize(
    const bridge_frame_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    if (frame == NULL || output == NULL || output_length == NULL ||
        frame->payload_length > BRIDGE_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = 9 + frame->payload_length;
    if (output_capacity < length) {
        return ESP_ERR_INVALID_SIZE;
    }

    output[0] = 0xA5;
    output[1] = 0x5A;
    output[2] = frame->version;
    output[3] = (uint8_t)frame->type;
    output[4] = (uint8_t)frame->sequence;
    output[5] = (uint8_t)(frame->sequence >> 8);
    output[6] = frame->payload_length;
    memcpy(output + 7, frame->payload, frame->payload_length);
    uint16_t crc = bridge_crc16_ccitt(output + 2, 5 + frame->payload_length);
    output[7 + frame->payload_length] = (uint8_t)crc;
    output[8 + frame->payload_length] = (uint8_t)(crc >> 8);
    *output_length = length;
    return ESP_OK;
}
