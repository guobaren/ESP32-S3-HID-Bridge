#pragma once
#include "bridge_protocol.h"
#include "mouse_motion_smoother.h"
typedef struct {
    mouse_motion_smoother_t smoother;
    mouse_motion_delta_t carry;
    uint8_t buttons;
    bool buttons_dirty;
    int64_t tick_us;
} m_udp_smoothing_t;
void m_udp_smoothing_reset(m_udp_smoothing_t *state);
void m_udp_smoothing_enqueue(m_udp_smoothing_t *state, const dual_frame_t *frame);
bool m_udp_smoothing_tick(m_udp_smoothing_t *state, int64_t now_us, dual_frame_t *output);
