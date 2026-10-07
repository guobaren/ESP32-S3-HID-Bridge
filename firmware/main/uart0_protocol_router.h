#pragma once
#include "bridge_protocol.h"
#include "makcu_v4_logic.h"

/* A5 候选整包隔离，防止其载荷被当成 V4 命令。 */
typedef struct {
    uint8_t a5[264];
    uint16_t length;
    uint16_t expected;
    uint32_t last_byte_ms;
    dual_parser_t legacy;
    makcu_v4_stream_parser_t v4;
    makcu_v4_command_callback_t v4_callback;
    void *context;
} uart0_protocol_router_t;

void uart0_protocol_router_init(uart0_protocol_router_t *state,
    dual_frame_callback_t legacy, makcu_v4_command_callback_t v4, void *context);
void uart0_protocol_router_tick(uart0_protocol_router_t *state, uint32_t now_ms);
void uart0_protocol_router_feed(uart0_protocol_router_t *state,
    const uint8_t *data, size_t length, uint32_t now_ms);
