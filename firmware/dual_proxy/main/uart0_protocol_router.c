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
