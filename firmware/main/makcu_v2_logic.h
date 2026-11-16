#pragma once
#include "makcu_v4_logic.h"

typedef struct {
    uint8_t state, opcode, length_low;
    uint16_t length, received;
    uint32_t last_byte_ms;
    uint8_t payload[64];
} makcu_v2_parser_t;

void makcu_v2_parser_init(makcu_v2_parser_t *parser);
void makcu_v2_parser_tick(makcu_v2_parser_t *parser, uint32_t now_ms);
void makcu_v2_parser_feed(makcu_v2_parser_t *parser, uint8_t value,
    uint32_t now_ms, makcu_v4_command_callback_t callback, void *context);
bool makcu_v2_encode_frame(uint8_t opcode, const uint8_t *payload,
    uint16_t length, uint8_t *output, size_t capacity, size_t *written);
