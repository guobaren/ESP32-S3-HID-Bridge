#pragma once
#include "bridge_protocol.h"
#include "makcu_v4_logic.h"

typedef enum {
    UART0_A5_ROUTE_IGNORE = 0,
    UART0_A5_ROUTE_NEUTRAL,
    UART0_A5_ROUTE_SELECT_A5_OWNER,
    UART0_A5_ROUTE_REJECT_MAKCU_ACTIVE,
    UART0_A5_ROUTE_REJECT_TRANSITION_TAIL,
} uart0_a5_route_action_t;

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
/* 完整 A5 帧解码后的显式请求白名单与所有权策略。 */
uart0_a5_route_action_t uart0_protocol_router_classify_a5_frame(
    const dual_frame_t *frame, bool makcu_mode_active,
    bool reject_current_buffer_tail);
bool uart0_protocol_router_v4_command_allowed(
    bool makcu_mode_active, bool reject_current_buffer_tail);
