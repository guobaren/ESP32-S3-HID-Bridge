#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAKCU_V4_MAX_PAYLOAD 64U
#define MAKCU_V4_ASCII_LINE_MAX 127U
#define MAKCU_V4_ARGUMENT_MAX 7U
#define MAKCU_V4_MOUSE_BUTTONS 5U
#define MAKCU_V4_LOCK_TARGETS 14U
#define MAKCU_V4_INPUT_LEASE_MS 1500U

typedef enum {
    MAKCU_V4_TRANSPORT_ASCII = 0,
    MAKCU_V4_TRANSPORT_BINARY = 1,
} makcu_v4_transport_t;

typedef enum {
    MAKCU_V4_ERROR_NONE = 0,
    MAKCU_V4_ERROR_SYNTAX,
    MAKCU_V4_ERROR_ARGUMENT,
    MAKCU_V4_ERROR_UNSUPPORTED,
    MAKCU_V4_ERROR_TOO_LONG,
    MAKCU_V4_ERROR_FRAME,
} makcu_v4_error_t;

typedef struct {
    makcu_v4_transport_t transport;
    makcu_v4_error_t error;
    uint8_t opcode;
    uint8_t payload_length;
    uint8_t payload[MAKCU_V4_MAX_PAYLOAD];
    int32_t argument[MAKCU_V4_ARGUMENT_MAX];
    uint8_t argument_count;
    bool query;
    char name[24];
    char source_line[MAKCU_V4_ASCII_LINE_MAX + 1U];
} makcu_v4_command_t;

typedef void (*makcu_v4_command_callback_t)(
    const makcu_v4_command_t *command,
    void *context);

typedef struct {
    char line[MAKCU_V4_ASCII_LINE_MAX + 1U];
    size_t length;
    bool overflow;
    bool saw_cr;
} makcu_v4_ascii_parser_t;

typedef struct {
    uint8_t state;
    uint8_t length_low;
    uint16_t payload_length;
    uint8_t opcode;
    uint8_t payload[MAKCU_V4_MAX_PAYLOAD];
    uint16_t payload_received;
} makcu_v4_binary_parser_t;

typedef struct {
    makcu_v4_ascii_parser_t ascii;
    makcu_v4_binary_parser_t binary;
    uint8_t mode; /* 0 idle, 1 ASCII line, 2 binary frame */
    uint32_t last_byte_ms;
} makcu_v4_stream_parser_t;

typedef struct {
    uint8_t physical_buttons;
    uint8_t injected_buttons;
    uint8_t button_mask;
    uint8_t move_mask;   /* bit 0 left, 1 right, 2 down, 3 up */
    uint8_t wheel_mask;  /* bit 0 down, bit 1 up */
    uint16_t lock_mask;  /* official targets 0..13 */
    uint16_t screen_width;
    uint16_t screen_height;
    uint16_t pointer_x;
    uint16_t pointer_y;
    uint8_t interpolate_percent;
    uint8_t last_injected_interval_ms;
    uint8_t ascii_buttons_mode;
    uint16_t ascii_buttons_period_ms;
    bool buttons_enabled;
    bool mouse_stream_enabled;
    bool echo_enabled;
    bool lease_active;
    uint32_t lease_deadline_ms;
    uint32_t scheduled_click_deadline_ms;
} makcu_v4_state_t;

void makcu_v4_ascii_parser_init(makcu_v4_ascii_parser_t *parser);
void makcu_v4_ascii_parser_feed(
    makcu_v4_ascii_parser_t *parser,
    const uint8_t *data,
    size_t length,
    makcu_v4_command_callback_t callback,
    void *context);
void makcu_v4_binary_parser_init(makcu_v4_binary_parser_t *parser);
void makcu_v4_binary_parser_feed(
    makcu_v4_binary_parser_t *parser,
    const uint8_t *data,
    size_t length,
    makcu_v4_command_callback_t callback,
    void *context);
void makcu_v4_stream_parser_init(makcu_v4_stream_parser_t *parser);
void makcu_v4_stream_parser_feed(
    makcu_v4_stream_parser_t *parser,
    const uint8_t *data,
    size_t length,
    uint32_t now_ms,
    makcu_v4_command_callback_t callback,
    void *context);
void makcu_v4_stream_parser_tick(
    makcu_v4_stream_parser_t *parser,
    uint32_t now_ms);

bool makcu_v4_encode_frame(
    uint8_t opcode,
    const uint8_t *payload,
    uint16_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);
bool makcu_v4_encode_error(
    uint8_t opcode,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

void makcu_v4_state_init(makcu_v4_state_t *state);
void makcu_v4_state_touch(makcu_v4_state_t *state, uint32_t now_ms);
bool makcu_v4_state_lease_expired(
    const makcu_v4_state_t *state,
    uint32_t now_ms);
void makcu_v4_state_release(makcu_v4_state_t *state);
bool makcu_v4_state_set_button(
    makcu_v4_state_t *state,
    uint8_t button,
    uint8_t value);
uint8_t makcu_v4_state_ascii_button(
    const makcu_v4_state_t *state,
    uint8_t button);
uint8_t makcu_v4_state_effective_physical_buttons(
    const makcu_v4_state_t *state);
bool makcu_v4_state_schedule_click(
    makcu_v4_state_t *state,
    uint32_t now_ms,
    uint16_t hold_ms);
bool makcu_v4_normalize_interpolate(uint8_t requested, uint8_t *normalized);
uint8_t makcu_v4_interpolate_slots(uint8_t normalized, uint8_t prior_interval_ms);
uint32_t makcu_v4_motion_steps(int32_t x, int32_t y, int32_t wheel,
                               uint32_t minimum_steps);
int32_t makcu_v4_motion_step(int32_t remaining, uint32_t steps_left);
void makcu_v4_state_track_move(
    makcu_v4_state_t *state,
    int32_t x,
    int32_t y);
