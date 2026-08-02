#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BRIDGE_PROTOCOL_VERSION 2
#define BRIDGE_MAX_PAYLOAD 64

typedef enum {
    BRIDGE_MESSAGE_KEYBOARD_REPORT = 0x01,
    BRIDGE_MESSAGE_MOUSE_REPORT = 0x02,
    BRIDGE_MESSAGE_RELEASE_ALL = 0x03,
    BRIDGE_MESSAGE_PING = 0x04,
    BRIDGE_MESSAGE_SESSION_START = 0x05,
    BRIDGE_MESSAGE_DEVICE_PROBE = 0x06,
    BRIDGE_MESSAGE_DEVICE_HELLO = 0x07,
} bridge_message_type_t;

typedef struct {
    uint8_t version;
    bridge_message_type_t type;
    uint16_t sequence;
    uint8_t payload_length;
    uint8_t payload[BRIDGE_MAX_PAYLOAD];
} bridge_frame_t;

typedef void (*bridge_frame_callback_t)(const bridge_frame_t *frame, void *context);

typedef struct {
    uint8_t buffer[9 + BRIDGE_MAX_PAYLOAD];
    size_t length;
    size_t expected_length;
    bridge_frame_callback_t callback;
    void *callback_context;
} bridge_parser_t;

void bridge_parser_init(
    bridge_parser_t *parser,
    bridge_frame_callback_t callback,
    void *callback_context);

void bridge_parser_feed(bridge_parser_t *parser, const uint8_t *data, size_t length);

uint16_t bridge_crc16_ccitt(const uint8_t *data, size_t length);

esp_err_t bridge_frame_serialize(
    const bridge_frame_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);
