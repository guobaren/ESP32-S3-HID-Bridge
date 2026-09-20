#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bridge_protocol.h"

#define DUAL_CDC_SOFTWARE_LEASE_MS 1500U

typedef struct {
    bool active;
    bool sequence_initialized;
    uint16_t expected_sequence;
    uint32_t last_activity_ms;
} dual_cdc_session_state_t;

typedef enum {
    DUAL_CDC_ACTION_NONE = 0,
    DUAL_CDC_ACTION_DEVICE_PROBE,
    DUAL_CDC_ACTION_MOUSE_REPORT,
    DUAL_CDC_ACTION_SOFTWARE_RELEASE,
    DUAL_CDC_ACTION_REJECT,
} dual_cdc_frame_action_t;

void dual_cdc_session_init(dual_cdc_session_state_t *state);
dual_cdc_frame_action_t dual_cdc_session_accept(
    dual_cdc_session_state_t *state,
    const dual_frame_t *frame,
    uint32_t now_ms);
bool dual_cdc_session_lease_expired(
    const dual_cdc_session_state_t *state,
    uint32_t now_ms);
void dual_cdc_session_expire(dual_cdc_session_state_t *state);
