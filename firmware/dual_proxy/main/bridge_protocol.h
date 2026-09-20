#pragma once

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
} dual_message_type_t;

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
