#include "makcu_v2_logic.h"
#include <string.h>

void makcu_v2_parser_init(makcu_v2_parser_t *p) { memset(p, 0, sizeof(*p)); }
void makcu_v2_parser_tick(makcu_v2_parser_t *p, uint32_t now_ms)
{
    if (p->state && (uint32_t)(now_ms - p->last_byte_ms) >= 250U)
        makcu_v2_parser_init(p);
}

bool makcu_v2_encode_frame(uint8_t opcode, const uint8_t *payload,
    uint16_t length, uint8_t *output, size_t capacity, size_t *written)
{
    if (!output || !written || capacity < (size_t)length + 4U ||
        (length && !payload)) return false;
    output[0] = 0x50U; output[1] = opcode;
    output[2] = (uint8_t)length; output[3] = (uint8_t)(length >> 8U);
    if (length) memcpy(output + 4U, payload, length);
    *written = (size_t)length + 4U;
    return true;
}

typedef struct {
    uint8_t opcode;
    bool silent_release;
    makcu_v4_command_callback_t callback;
    void *context;
} conversion_t;
static void converted(const makcu_v4_command_t *source, void *context)
{
    conversion_t *c = context;
    makcu_v4_command_t command = *source;
    command.v2_frame = true;
    command.v2_opcode = c->opcode;
    if (c->silent_release) command.payload[0] = 2U;
    c->callback(&command, c->context);
}

/* 只映射定义明确且已有执行能力的鼠标命令，其他命令显式返回 ERR。 */
static void dispatch(makcu_v2_parser_t *p,
    makcu_v4_command_callback_t callback, void *context)
{
    uint8_t opcode = 0U, data[64] = {0};
    uint16_t length = p->length;
    memcpy(data, p->payload, length);
    bool valid = true;
    switch (p->opcode) {
        case 0x08: opcode = 0x11; break;
        case 0x11: opcode = 0x12; break;
        case 0x0A: opcode = 0x13; break;
        case 0x12: opcode = 0x14; break;
        case 0x13: opcode = 0x15; break;
        case 0x04: /* V2 的点击延时是 u8；V4 使用 u16。 */
            opcode = 0x61;
            valid = length == 3U;
            data[3] = 0U; length = 4U;
            break;
        case 0x0D:
            opcode = 0x18;
            valid = length == 4U || length == 5U || length == 7U;
            for (uint16_t i = 4U; i < length; ++i) valid &= data[i] == 0U;
            length = 4U;
            break;
        case 0x0E:
            opcode = 0x62;
            valid = (length == 4U || length == 5U || length == 9U || length == 13U) &&
                (data[1] & 0x80U) == 0U && (data[3] & 0x80U) == 0U;
            for (uint16_t i = 4U; i < length; ++i) valid &= data[i] == 0U;
            length = 4U;
            break;
        case 0x18:
            opcode = 0x19;
            valid = length == 1U;
            data[1] = (data[0] & 0x80U) ? 0xFFU : 0U;
            length = 2U;
            break;
        case 0x05: opcode = 0x63; valid = length == 0U; break;
        case 0xBD: opcode = 0x64; break;
        case 0xB1: opcode = length == 0U ? 0xA4 : 0xA5; break;
        case 0xB3: opcode = 0x02; valid = length == 0U; break;
        case 0xBF: opcode = 0x04; valid = length == 0U; break;
        default: valid = false; break;
    }
    if (!valid) {
        makcu_v4_command_t command = {.transport = MAKCU_V4_TRANSPORT_BINARY,
            .error = MAKCU_V4_ERROR_UNSUPPORTED, .v2_frame = true, .v2_opcode = p->opcode};
        callback(&command, context);
        return;
    }
    uint8_t frame[69]; size_t written = 0U;
    makcu_v4_binary_parser_t parser;
    makcu_v4_binary_parser_init(&parser);
    const bool button = opcode >= 0x11U && opcode <= 0x15U;
    const bool silent_release = button && length == 1U && data[0] == 2U;
    if (silent_release) data[0] = 0U;
    conversion_t conversion = {p->opcode, silent_release, callback, context};
    if (makcu_v4_encode_frame(opcode, data, length, frame, sizeof(frame), &written))
        makcu_v4_binary_parser_feed(&parser, frame, written, converted, &conversion);
}

void makcu_v2_parser_feed(makcu_v2_parser_t *p, uint8_t value,
    uint32_t now_ms, makcu_v4_command_callback_t callback, void *context)
{
    makcu_v2_parser_tick(p, now_ms);
    p->last_byte_ms = now_ms;
    switch (p->state) {
        case 0: if (value == 0x50U) p->state = 1U; return;
        case 1: p->opcode = value; p->state = 2U; return;
        case 2: p->length_low = value; p->state = 3U; return;
        case 3:
            p->length = (uint16_t)(p->length_low | ((uint16_t)value << 8U));
            p->received = 0U; p->state = 4U;
            break;
        default:
            if (p->received < sizeof(p->payload)) p->payload[p->received] = value;
            ++p->received;
            break;
    }
    if (p->state == 4U && p->received == p->length) {
        if (p->length <= sizeof(p->payload)) dispatch(p, callback, context);
        else {
            makcu_v4_command_t command = {.transport = MAKCU_V4_TRANSPORT_BINARY,
                .error = MAKCU_V4_ERROR_TOO_LONG, .v2_frame = true, .v2_opcode = p->opcode};
            callback(&command, context);
        }
        makcu_v2_parser_init(p);
    }
}
