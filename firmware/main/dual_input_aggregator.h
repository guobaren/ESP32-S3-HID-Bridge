#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t physical_buttons;
    uint8_t software_buttons;
    int64_t physical_x;
    int64_t physical_y;
    int64_t physical_wheel;
    int64_t physical_pan;
    int64_t software_x;
    int64_t software_y;
    int64_t software_wheel;
    int64_t software_pan;
    bool physical_online;
    uint8_t last_output_buttons;
} dual_input_state_t;

typedef struct {
    uint8_t buttons;
    int16_t x;
    int16_t y;
    int8_t wheel;
    int8_t pan;
} dual_mouse_report_t;

void dual_input_init(dual_input_state_t *state);
void dual_input_physical_report(
    dual_input_state_t *state,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan);
void dual_input_physical_release(dual_input_state_t *state);
void dual_input_software_report(
    dual_input_state_t *state,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan);
void dual_input_software_release(dual_input_state_t *state);
void dual_input_release_all(dual_input_state_t *state);
bool dual_input_peek_report(
    const dual_input_state_t *state,
    dual_mouse_report_t *report,
    bool force);
void dual_input_commit_report(dual_input_state_t *state, const dual_mouse_report_t *report);
bool dual_input_take_report(dual_input_state_t *state, dual_mouse_report_t *report, bool force);
