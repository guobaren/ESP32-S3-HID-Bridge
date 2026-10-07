#include "mouse_motion_smoother.h"

#include <stddef.h>
#include <string.h>

static int64_t split_component(
    int64_t total,
    uint8_t index,
    uint8_t phase,
    uint8_t slot_count)
{
    const int64_t quotient = total / (int64_t)slot_count;
    const int64_t remainder = total % (int64_t)slot_count;
    const uint8_t phased_index =
        (uint8_t)((index + slot_count - phase) % slot_count);
    const uint64_t remainder_magnitude = (uint64_t)(remainder < 0 ? -remainder : remainder);
    const int64_t remainder_part = phased_index < remainder_magnitude
        ? (remainder < 0 ? -1 : 1)
        : 0;
    return quotient + remainder_part;
}

static void add_delta(mouse_motion_delta_t *target, mouse_motion_delta_t delta)
{
    target->x += delta.x;
    target->y += delta.y;
    target->wheel += delta.wheel;
    target->pan += delta.pan;
}

bool mouse_motion_smoother_valid_slot_count(uint8_t slot_count)
{
    return slot_count == 0U ||
        (slot_count <= MOUSE_MOTION_SMOOTHING_MAX_SLOTS &&
         slot_count % MOUSE_MOTION_SMOOTHING_SLOT_STEP == 0U);
}

void mouse_motion_smoother_enqueue(
    mouse_motion_smoother_t *state,
    mouse_motion_delta_t delta,
    uint8_t slot_count)
{
    if (state == NULL || !mouse_motion_smoother_valid_slot_count(slot_count)) {
        return;
    }

    if (slot_count == 0U) {
        mouse_motion_delta_t pending = mouse_motion_smoother_drain(state);
        add_delta(&state->slots[state->next_slot], pending);
        add_delta(&state->slots[state->next_slot], delta);
        return;
    }

    const uint8_t phase = (uint8_t)(state->distribution_phase % slot_count);
    for (uint8_t offset = 0; offset < slot_count; ++offset) {
        const uint8_t slot =
            (uint8_t)((state->next_slot + offset) % MOUSE_MOTION_SMOOTHING_MAX_SLOTS);
        mouse_motion_delta_t part = {
            .x = split_component(delta.x, offset, phase, slot_count),
            .y = split_component(delta.y, offset, phase, slot_count),
            .wheel = split_component(delta.wheel, offset, phase, slot_count),
            .pan = split_component(delta.pan, offset, phase, slot_count),
        };
        add_delta(&state->slots[slot], part);
    }
    state->distribution_phase = (uint8_t)((phase + 1U) % slot_count);
}

mouse_motion_delta_t mouse_motion_smoother_take_next(mouse_motion_smoother_t *state)
{
    if (state == NULL) {
        return (mouse_motion_delta_t){0};
    }
    mouse_motion_delta_t result = state->slots[state->next_slot];
    state->slots[state->next_slot] = (mouse_motion_delta_t){0};
    state->next_slot = (uint8_t)((state->next_slot + 1U) % MOUSE_MOTION_SMOOTHING_MAX_SLOTS);
    return result;
}

mouse_motion_delta_t mouse_motion_smoother_pending(const mouse_motion_smoother_t *state)
{
    mouse_motion_delta_t result = {0};
    if (state == NULL) {
        return result;
    }
    for (uint8_t index = 0; index < MOUSE_MOTION_SMOOTHING_MAX_SLOTS; ++index) {
        add_delta(&result, state->slots[index]);
    }
    return result;
}

mouse_motion_delta_t mouse_motion_smoother_drain(mouse_motion_smoother_t *state)
{
    mouse_motion_delta_t result = mouse_motion_smoother_pending(state);
    if (state != NULL) {
        memset(state->slots, 0, sizeof(state->slots));
        state->distribution_phase = 0;
    }
    return result;
}

bool mouse_motion_smoother_has_pending(const mouse_motion_smoother_t *state)
{
    mouse_motion_delta_t pending = mouse_motion_smoother_pending(state);
    return pending.x != 0 || pending.y != 0 || pending.wheel != 0 || pending.pan != 0;
}

void mouse_motion_smoother_reset(mouse_motion_smoother_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}
