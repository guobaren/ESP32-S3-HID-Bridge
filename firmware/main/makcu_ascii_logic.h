#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAKCU_ASCII_COMMAND_MAX 96U
#define MAKCU_ASCII_MOUSE_BUTTON_COUNT 5U
#define MAKCU_ASCII_INPUT_LEASE_MS 1500U

typedef enum {
    MAKCU_ASCII_COMMAND_ERROR = 0,
    MAKCU_ASCII_COMMAND_MOVE,
    MAKCU_ASCII_COMMAND_WHEEL,
    MAKCU_ASCII_COMMAND_BUTTON,
    MAKCU_ASCII_COMMAND_ECHO,
    MAKCU_ASCII_COMMAND_VERSION,
    MAKCU_ASCII_COMMAND_DEVICE,
    MAKCU_ASCII_COMMAND_RELEASE,
} makcu_ascii_command_kind_t;

typedef enum {
    MAKCU_ASCII_ERROR_NONE = 0,
    MAKCU_ASCII_ERROR_SYNTAX,
    MAKCU_ASCII_ERROR_ARGUMENT,
    MAKCU_ASCII_ERROR_UNSUPPORTED,
    MAKCU_ASCII_ERROR_TOO_LONG,
} makcu_ascii_error_t;

typedef struct {
    makcu_ascii_command_kind_t kind;
    makcu_ascii_error_t error;
    uint8_t button_index;
    uint8_t argument_count;
    int32_t argument[2];
    bool query;
    char body[MAKCU_ASCII_COMMAND_MAX + 1U];
} makcu_ascii_command_t;

typedef struct {
    char body[MAKCU_ASCII_COMMAND_MAX];
    size_t length;
    bool collecting;
    bool overflow;
} makcu_ascii_parser_t;

typedef void (*makcu_ascii_command_callback_t)(
    const makcu_ascii_command_t *command,
    void *context);

typedef struct {
    uint8_t injected_buttons;
    uint32_t last_activity_ms;
    bool lease_active;
    bool echo_enabled;
} makcu_ascii_session_t;

void makcu_ascii_parser_init(makcu_ascii_parser_t *parser);
void makcu_ascii_parser_feed(
    makcu_ascii_parser_t *parser,
    const uint8_t *data,
    size_t length,
    makcu_ascii_command_callback_t callback,
    void *context);

void makcu_ascii_session_init(makcu_ascii_session_t *session);
void makcu_ascii_session_touch(makcu_ascii_session_t *session, uint32_t now_ms);
bool makcu_ascii_session_lease_expired(
    const makcu_ascii_session_t *session,
    uint32_t now_ms);
void makcu_ascii_session_expire(makcu_ascii_session_t *session);
bool makcu_ascii_session_set_button(
    makcu_ascii_session_t *session,
    uint8_t button_index,
    int32_t state,
    bool *send_report);
uint8_t makcu_ascii_button_state(
    uint8_t button_index,
    uint8_t physical_buttons,
    uint8_t injected_buttons);
