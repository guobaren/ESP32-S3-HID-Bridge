#include "m_udp_smoothing.h"
#include <limits.h>
#include <string.h>
void m_udp_smoothing_reset(m_udp_smoothing_t *state)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
}
static int64_t legacy_clip(int64_t value, int64_t low, int64_t high)
{
    return value < low ? low : (value > high ? high : value);
}

void m_udp_smoothing_enqueue(m_udp_smoothing_t *state, const dual_frame_t *frame)
{
    if (state == NULL || frame == NULL || (frame->payload_length != 7U && frame->payload_length != 8U)) return;
    if (frame->payload_length == 8U && !mouse_motion_smoother_valid_slot_count(frame->payload[7])) return;
    const uint8_t *p = frame->payload;
    state->buttons_dirty |= state->buttons != p[0];
    state->buttons = p[0];
    mouse_motion_delta_t delta = {
        .x = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8)),
        .y = (int16_t)((uint16_t)p[3] | ((uint16_t)p[4] << 8)),
        .wheel = (int8_t)p[5], .pan = (int8_t)p[6],
    };
    mouse_motion_smoother_enqueue(&state->smoother, delta,
        frame->payload_length == 8U ? p[7] : 0U);
}

bool m_udp_smoothing_tick(m_udp_smoothing_t *state, int64_t now, dual_frame_t *output)
{
    if (state == NULL || output == NULL) return false;
    if (now - state->tick_us < 1000) return false;
    state->tick_us = now;
    mouse_motion_delta_t d = mouse_motion_smoother_take_next(&state->smoother);
    d.x += state->carry.x; d.y += state->carry.y;
    d.wheel += state->carry.wheel; d.pan += state->carry.pan;
    mouse_motion_delta_t out = {
        legacy_clip(d.x, INT16_MIN, INT16_MAX),
        legacy_clip(d.y, INT16_MIN, INT16_MAX),
        legacy_clip(d.wheel, INT8_MIN, INT8_MAX),
        legacy_clip(d.pan, INT8_MIN, INT8_MAX),
    };
    state->carry = (mouse_motion_delta_t){
        d.x-out.x, d.y-out.y, d.wheel-out.wheel, d.pan-out.pan};
    if (!state->buttons_dirty && out.x == 0 && out.y == 0 &&
        out.wheel == 0 && out.pan == 0) return false;
    dual_frame_t frame = {.version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_MOUSE_REPORT, .payload_length = 8U};
    frame.payload[0] = state->buttons;
    frame.payload[1] = (uint8_t)out.x;
    frame.payload[2] = (uint8_t)((uint16_t)out.x >> 8);
    frame.payload[3] = (uint8_t)out.y;
    frame.payload[4] = (uint8_t)((uint16_t)out.y >> 8);
    frame.payload[5] = (uint8_t)out.wheel;
    frame.payload[6] = (uint8_t)out.pan;
    frame.payload[7] = 0U; /* P output uses zero smoothing slots. */
    state->buttons_dirty = false;
    *output = frame;
    return true;
}