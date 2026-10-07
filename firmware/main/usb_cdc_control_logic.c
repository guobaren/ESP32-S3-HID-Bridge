#include "usb_cdc_control_logic.h"

#include <string.h>

void dual_cdc_session_init(dual_cdc_session_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

static void session_activate(
    dual_cdc_session_state_t *state,
    const dual_frame_t *frame,
    uint32_t now_ms)
{
    state->active = true;
    state->sequence_initialized = true;
    state->expected_sequence = (uint16_t)(frame->sequence + 1U);
    state->last_activity_ms = now_ms;
}

dual_cdc_frame_action_t dual_cdc_session_accept(
    dual_cdc_session_state_t *state,
    const dual_frame_t *frame,
    uint32_t now_ms)
{
    if (state == NULL || frame == NULL) {
        return DUAL_CDC_ACTION_REJECT;
    }
    if (frame->type == DUAL_MESSAGE_DEVICE_PROBE) {
        return frame->payload_length == 8
            ? DUAL_CDC_ACTION_DEVICE_PROBE
            : DUAL_CDC_ACTION_REJECT;
    }
    if (frame->type == DUAL_MESSAGE_SESSION_START) {
        session_activate(state, frame, now_ms);
        return DUAL_CDC_ACTION_SOFTWARE_RELEASE;
    }
    if (!state->active) {
        if (frame->type == DUAL_MESSAGE_PING) {
            session_activate(state, frame, now_ms);
            return DUAL_CDC_ACTION_NONE;
        }
        return DUAL_CDC_ACTION_REJECT;
    }

    state->last_activity_ms = now_ms;
    state->expected_sequence = (uint16_t)(frame->sequence + 1U);
    state->sequence_initialized = true;
    switch (frame->type) {
    case DUAL_MESSAGE_PING:
        return DUAL_CDC_ACTION_NONE;
    case DUAL_MESSAGE_MOUSE_REPORT:
        if ((frame->payload_length != 7 && frame->payload_length != 8) ||
            (frame->payload_length == 8 && frame->payload[7] != 0 && frame->payload[7] != 5)) {
            return DUAL_CDC_ACTION_REJECT;
        }
        return DUAL_CDC_ACTION_MOUSE_REPORT;
    case DUAL_MESSAGE_RELEASE_ALL:
        return DUAL_CDC_ACTION_SOFTWARE_RELEASE;
    default:
        return DUAL_CDC_ACTION_REJECT;
    }
}

bool dual_cdc_session_lease_expired(
    const dual_cdc_session_state_t *state,
    uint32_t now_ms)
{
    return state != NULL && state->active &&
        (uint32_t)(now_ms - state->last_activity_ms) > DUAL_CDC_SOFTWARE_LEASE_MS;
}

void dual_cdc_session_expire(dual_cdc_session_state_t *state)
{
    if (state != NULL) {
        state->active = false;
        state->sequence_initialized = false;
    }
}
