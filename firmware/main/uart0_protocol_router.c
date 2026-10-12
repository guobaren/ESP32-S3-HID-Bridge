#include "uart0_protocol_router.h"
#include <string.h>

void uart0_protocol_router_init(uart0_protocol_router_t *s,
    dual_frame_callback_t legacy, makcu_v4_command_callback_t v4, void *context)
{
    memset(s, 0, sizeof(*s));
    dual_parser_init(&s->legacy, legacy, context);
    makcu_v4_stream_parser_init(&s->v4);
    s->v4_callback = v4;
    s->context = context;
}

void uart0_protocol_router_tick(uart0_protocol_router_t *s, uint32_t now_ms)
{
    if (s->length != 0U && (uint32_t)(now_ms - s->last_byte_ms) >= 250U) {
        s->length = s->expected = 0U;
    }
    makcu_v4_stream_parser_tick(&s->v4, now_ms);
}

void uart0_protocol_router_feed(uart0_protocol_router_t *s,
    const uint8_t *data, size_t length, uint32_t now_ms)
{
    uart0_protocol_router_tick(s, now_ms);
    for (size_t i = 0; i < length; ++i) {
        const uint8_t value = data[i];
        if (s->length == 1U && value != 0x5AU) {
            s->length = 0U;
        }
        if (s->length != 0U || (s->v4.mode == 0U && value == 0xA5U)) {
            s->a5[s->length++] = value;
            s->last_byte_ms = now_ms;
            if (s->length == 7U) s->expected = (uint16_t)(9U + s->a5[6]);
            if (s->expected != 0U && s->length == s->expected) {
                const size_t payload = s->a5[6];
                const uint16_t crc = (uint16_t)s->a5[7U + payload] |
                    ((uint16_t)s->a5[8U + payload] << 8);
                if (payload <= DUAL_PROXY_MAX_PAYLOAD &&
                    s->a5[2] == DUAL_PROXY_PROTOCOL_VERSION &&
                    crc == dual_crc16_ccitt(&s->a5[2], 5U + payload)) {
                    dual_parser_feed(&s->legacy, s->a5, s->length);
                }
                s->length = s->expected = 0U;
            }
        } else {
            makcu_v4_stream_parser_feed(&s->v4, &value, 1U, now_ms,
                s->v4_callback, s->context);
        }
    }
}

uart0_a5_route_action_t uart0_protocol_router_classify_a5_frame(
    const dual_frame_t *frame, bool makcu_mode_active,
    bool reject_current_buffer_tail)
{
    if (frame == NULL) {
        return UART0_A5_ROUTE_IGNORE;
    }
    if (reject_current_buffer_tail) {
        return UART0_A5_ROUTE_REJECT_TRANSITION_TAIL;
    }

    switch (frame->type) {
        case DUAL_MESSAGE_DEVICE_PROBE:
            return frame->payload_length == 8U
                ? UART0_A5_ROUTE_NEUTRAL : UART0_A5_ROUTE_IGNORE;
        case DUAL_MESSAGE_STATS_SNAPSHOT_REQUEST:
            if (frame->payload_length != 0U) {
                return UART0_A5_ROUTE_IGNORE;
            }
            /* 统计分页仍走仅 A5 模式可用的输出门控。 */
            return makcu_mode_active
                ? UART0_A5_ROUTE_REJECT_MAKCU_ACTIVE
                : UART0_A5_ROUTE_NEUTRAL;
        case DUAL_MESSAGE_MAKCU_MODE_REQUEST:
            /* 无效操作码由处理器返回 INVALID，且不会切换输入所有权。 */
            return UART0_A5_ROUTE_NEUTRAL;
        case DUAL_MESSAGE_BUTTON_STATE:
            if (frame->payload_length == DUAL_BUTTON_STATE_PAYLOAD_LENGTH &&
                frame->payload[0] <= 1U &&
                (!makcu_mode_active || frame->payload[0] == 0U)) {
                return UART0_A5_ROUTE_NEUTRAL;
            }
            return makcu_mode_active
                ? UART0_A5_ROUTE_REJECT_MAKCU_ACTIVE : UART0_A5_ROUTE_IGNORE;

        case DUAL_MESSAGE_KEYBOARD_REPORT:
        case DUAL_MESSAGE_MOUSE_REPORT:
        case DUAL_MESSAGE_RELEASE_ALL:
        case DUAL_MESSAGE_PING:
        case DUAL_MESSAGE_SESSION_START:
        case DUAL_MESSAGE_DIAG_PROFILE_READ:
        case DUAL_MESSAGE_DIAG_PROFILE_BEGIN:
        case DUAL_MESSAGE_DIAG_PROFILE_CHUNK:
        case DUAL_MESSAGE_DIAG_PROFILE_COMMIT:
        case DUAL_MESSAGE_DIAG_PROFILE_MODE:
        case DUAL_MESSAGE_DIAG_STREAM_CONTROL:
        case DUAL_MESSAGE_DIAG_INJECT_REQUEST:
        case DUAL_MESSAGE_DIAG_VENDOR_TEST_REQUEST:
        case DUAL_MESSAGE_DIAG_REPORT_INJECT_REQUEST:
        case DUAL_MESSAGE_DIAG_PROFILE_REFRESH_REQUEST:
            return makcu_mode_active
                ? UART0_A5_ROUTE_REJECT_MAKCU_ACTIVE
                : UART0_A5_ROUTE_SELECT_A5_OWNER;

        default:
            return UART0_A5_ROUTE_IGNORE;
    }
}

bool uart0_protocol_router_v4_command_allowed(
    bool makcu_mode_active, bool reject_current_buffer_tail)
{
    return makcu_mode_active && !reject_current_buffer_tail;
}
