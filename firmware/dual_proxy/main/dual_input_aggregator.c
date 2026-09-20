#include "dual_input_aggregator.h"

#include <limits.h>
#include <string.h>

static int64_t add_saturated(int64_t left, int64_t right)
{
    if (right > 0 && left > INT64_MAX - right) {
        return INT64_MAX;
    }
    if (right < 0 && left < INT64_MIN - right) {
        return INT64_MIN;
    }
    return left + right;
}

static int16_t clamp_i16(int64_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static int8_t clamp_i8(int64_t value)
{
    if (value > INT8_MAX) {
        return INT8_MAX;
    }
    if (value < INT8_MIN) {
        return INT8_MIN;
    }
    return (int8_t)value;
}

static void add_axis(int64_t *axis, int64_t delta)
{
    *axis = add_saturated(*axis, delta);
}

static uint64_t magnitude(int64_t value)
{
    return value >= 0 ? (uint64_t)value : (uint64_t)(-(value + 1)) + 1U;
}

static void cancel_opposite(int64_t *left, int64_t *right)
{
    if ((*left > 0 && *right < 0) || (*left < 0 && *right > 0)) {
        const uint64_t left_abs = magnitude(*left);
        const uint64_t right_abs = magnitude(*right);
        if (left_abs <= right_abs) {
            *right += *left;
            *left = 0;
        } else {
            *left += *right;
            *right = 0;
        }
    }
}

static int64_t take_from_axis(int64_t *first, int64_t *second, int64_t requested)
{
    int64_t remaining = requested;
    if (remaining > 0) {
        if (*first > 0) {
            const int64_t amount = *first < remaining ? *first : remaining;
            *first -= amount;
            remaining -= amount;
        }
        if (remaining > 0 && *second > 0) {
            const int64_t amount = *second < remaining ? *second : remaining;
            *second -= amount;
            remaining -= amount;
        }
    } else if (remaining < 0) {
        if (*first < 0) {
            const int64_t amount = *first > remaining ? *first : remaining;
            *first -= amount;
            remaining -= amount;
        }
        if (remaining < 0 && *second < 0) {
            const int64_t amount = *second > remaining ? *second : remaining;
            *second -= amount;
            remaining -= amount;
        }
    }
    return requested - remaining;
}

void dual_input_init(dual_input_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void dual_input_physical_report(
    dual_input_state_t *state,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan)
{
    if (state == NULL) {
        return;
    }
    state->physical_online = true;
    state->physical_buttons = buttons & 0x1FU;
    add_axis(&state->physical_x, x);
    add_axis(&state->physical_y, y);
    add_axis(&state->physical_wheel, wheel);
    add_axis(&state->physical_pan, pan);
}

void dual_input_physical_release(dual_input_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->physical_online = false;
    state->physical_buttons = 0;
    state->physical_x = 0;
    state->physical_y = 0;
    state->physical_wheel = 0;
    state->physical_pan = 0;
}

void dual_input_software_report(
    dual_input_state_t *state,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan)
{
    if (state == NULL) {
        return;
    }
    state->software_buttons = buttons & 0x1FU;
    add_axis(&state->software_x, x);
    add_axis(&state->software_y, y);
    add_axis(&state->software_wheel, wheel);
    add_axis(&state->software_pan, pan);
}

void dual_input_software_release(dual_input_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->software_buttons = 0;
    state->software_x = 0;
    state->software_y = 0;
    state->software_wheel = 0;
    state->software_pan = 0;
}

void dual_input_release_all(dual_input_state_t *state)
{
    if (state == NULL) {
        return;
    }
    const bool physical_online = state->physical_online;
    dual_input_init(state);
    state->physical_online = physical_online;
}

bool dual_input_peek_report(
    const dual_input_state_t *state,
    dual_mouse_report_t *report,
    bool force)
{
    if (state == NULL || report == NULL) {
        return false;
    }

    int64_t physical_x = state->physical_x;
    int64_t physical_y = state->physical_y;
    int64_t physical_wheel = state->physical_wheel;
    int64_t physical_pan = state->physical_pan;
    int64_t software_x = state->software_x;
    int64_t software_y = state->software_y;
    int64_t software_wheel = state->software_wheel;
    int64_t software_pan = state->software_pan;
    cancel_opposite(&physical_x, &software_x);
    cancel_opposite(&physical_y, &software_y);
    cancel_opposite(&physical_wheel, &software_wheel);
    cancel_opposite(&physical_pan, &software_pan);

    const uint8_t buttons = (uint8_t)((state->physical_buttons | state->software_buttons) & 0x1FU);
    const int64_t total_x = add_saturated(physical_x, software_x);
    const int64_t total_y = add_saturated(physical_y, software_y);
    const int64_t total_wheel = add_saturated(physical_wheel, software_wheel);
    const int64_t total_pan = add_saturated(physical_pan, software_pan);
    if (!force && buttons == state->last_output_buttons &&
        total_x == 0 && total_y == 0 && total_wheel == 0 && total_pan == 0) {
        return false;
    }

    report->buttons = buttons;
    report->x = clamp_i16(total_x);
    report->y = clamp_i16(total_y);
    report->wheel = clamp_i8(total_wheel);
    report->pan = clamp_i8(total_pan);
    return true;
}

void dual_input_commit_report(dual_input_state_t *state, const dual_mouse_report_t *report)
{
    if (state == NULL || report == NULL) {
        return;
    }
    cancel_opposite(&state->physical_x, &state->software_x);
    cancel_opposite(&state->physical_y, &state->software_y);
    cancel_opposite(&state->physical_wheel, &state->software_wheel);
    cancel_opposite(&state->physical_pan, &state->software_pan);
    take_from_axis(&state->physical_x, &state->software_x, report->x);
    take_from_axis(&state->physical_y, &state->software_y, report->y);
    take_from_axis(&state->physical_wheel, &state->software_wheel, report->wheel);
    take_from_axis(&state->physical_pan, &state->software_pan, report->pan);
    state->last_output_buttons = report->buttons;
}

bool dual_input_take_report(dual_input_state_t *state, dual_mouse_report_t *report, bool force)
{
    if (!dual_input_peek_report(state, report, force)) {
        return false;
    }
    dual_input_commit_report(state, report);
    return true;
}
