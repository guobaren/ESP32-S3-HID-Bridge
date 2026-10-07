#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MOUSE_MOTION_SMOOTHING_MAX_SLOTS 20U
#define MOUSE_MOTION_SMOOTHING_SLOT_STEP 5U

typedef struct {
    int64_t x;
    int64_t y;
    int64_t wheel;
    int64_t pan;
} mouse_motion_delta_t;

typedef struct {
    mouse_motion_delta_t slots[MOUSE_MOTION_SMOOTHING_MAX_SLOTS];
    uint8_t next_slot;
    uint8_t distribution_phase;
} mouse_motion_smoother_t;

bool mouse_motion_smoother_valid_slot_count(uint8_t slot_count);

void mouse_motion_smoother_enqueue(
    mouse_motion_smoother_t *state,
    mouse_motion_delta_t delta,
    uint8_t slot_count);

mouse_motion_delta_t mouse_motion_smoother_take_next(mouse_motion_smoother_t *state);

mouse_motion_delta_t mouse_motion_smoother_pending(const mouse_motion_smoother_t *state);

mouse_motion_delta_t mouse_motion_smoother_drain(mouse_motion_smoother_t *state);

bool mouse_motion_smoother_has_pending(const mouse_motion_smoother_t *state);

void mouse_motion_smoother_reset(mouse_motion_smoother_t *state);
