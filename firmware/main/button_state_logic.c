#include "button_state_logic.h"

#include <string.h>

void dual_button_state_init(dual_button_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void dual_button_state_set_subscribed(dual_button_state_t *state, bool subscribed)
{
    if (state == NULL || state->subscribed == subscribed) {
        return;
    }
    state->subscribed = subscribed;
    ++state->generation;
    state->dirty = subscribed;
}

void dual_button_state_update_physical(dual_button_state_t *state, uint8_t buttons)
{
    if (state != NULL) {
        (void)dual_button_state_update_physical_for_epoch(
            state, state->observation_epoch, buttons);
    }
}

bool dual_button_state_update_physical_for_epoch(
    dual_button_state_t *state, uint32_t observation_epoch, uint8_t buttons)
{
    if (state == NULL || state->observation_epoch != observation_epoch) {
        return false;
    }
    buttons &= DUAL_BUTTON_STATE_MASK;
    if (state->valid && state->buttons == buttons) {
        return true;
    }
    state->buttons = buttons;
    state->valid = true;
    ++state->generation;
    if (state->subscribed) {
        state->dirty = true;
    }
    return true;
}

void dual_button_state_invalidate(dual_button_state_t *state)
{
    if (state == NULL) {
        return;
    }
    ++state->observation_epoch;
    state->buttons = 0U;
    state->valid = false;
    ++state->generation;
    if (state->subscribed) {
        state->dirty = true;
    }
}

bool dual_button_state_invalidate_for_epoch(
    dual_button_state_t *state, uint32_t observation_epoch)
{
    if (state == NULL || state->observation_epoch != observation_epoch) {
        return false;
    }
    dual_button_state_invalidate(state);
    return true;
}

uint32_t dual_button_state_observation_epoch(const dual_button_state_t *state)
{
    return state != NULL ? state->observation_epoch : 0U;
}

bool dual_button_state_due(const dual_button_state_t *state, int64_t now_us)
{
    if (state == NULL || !state->subscribed) {
        return false;
    }
    return state->dirty || !state->has_successful_send ||
        now_us - state->last_success_us >= DUAL_BUTTON_STATE_HEARTBEAT_US;
}

uint8_t dual_button_state_payload(const dual_button_state_t *state)
{
    if (state == NULL) {
        return 0U;
    }
    return (uint8_t)((state->buttons & DUAL_BUTTON_STATE_MASK) |
        (state->valid ? DUAL_BUTTON_STATE_VALID : 0U));
}

uint32_t dual_button_state_generation(const dual_button_state_t *state)
{
    return state != NULL ? state->generation : 0U;
}

void dual_button_state_record_send(
    dual_button_state_t *state,
    uint32_t sent_generation,
    bool success,
    int64_t now_us)
{
    if (state == NULL || !success) {
        return;
    }
    state->has_successful_send = true;
    state->last_success_us = now_us;
    if (state->generation == sent_generation) {
        state->dirty = false;
    }
}
