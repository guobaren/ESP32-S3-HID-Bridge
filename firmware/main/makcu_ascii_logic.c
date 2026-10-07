#include "makcu_ascii_logic.h"

#include <string.h>

typedef struct {
    const char *name;
    uint8_t index;
} button_name_t;

static const button_name_t s_button_names[] = {
    {"left", 0U},
    {"right", 1U},
    {"middle", 2U},
    {"side1", 3U},
    {"side2", 4U},
};

static bool is_space(char value)
{
    return value == ' ' || value == '\t';
}

static void trim_span(const char **text, size_t *length)
{
    while (*length > 0U && is_space((*text)[0])) {
        ++(*text);
        --(*length);
    }
    while (*length > 0U && is_space((*text)[*length - 1U])) {
        --(*length);
    }
}

static bool span_equals(const char *text, size_t length, const char *expected)
{
    const size_t expected_length = strlen(expected);
    return length == expected_length && memcmp(text, expected, length) == 0;
}

static bool parse_integer(const char *text, size_t length, int32_t *value)
{
    if (text == NULL || length == 0U || value == NULL) {
        return false;
    }
    bool negative = false;
    size_t index = 0U;
    if (text[index] == '-' || text[index] == '+') {
        negative = text[index] == '-';
        if (++index == length) {
            return false;
        }
    }

    uint32_t magnitude = 0U;
    const uint32_t limit = negative ? 2147483648U : 2147483647U;
    for (; index < length; ++index) {
        const char digit = text[index];
        if (digit < '0' || digit > '9') {
            return false;
        }
        const uint32_t next = (uint32_t)(digit - '0');
        if (magnitude > (limit - next) / 10U) {
            return false;
        }
        magnitude = magnitude * 10U + next;
    }

    if (negative) {
        *value = magnitude == 2147483648U
            ? INT32_MIN : -(int32_t)magnitude;
    } else {
        *value = (int32_t)magnitude;
    }
    return true;
}

static bool parse_arguments(
    const char *text,
    size_t length,
    int32_t arguments[2],
    uint8_t *argument_count)
{
    *argument_count = 0U;
    if (length == 0U) {
        return true;
    }

    size_t start = 0U;
    while (start < length) {
        size_t end = start;
        while (end < length && text[end] != ',') {
            ++end;
        }
        const char *token = &text[start];
        size_t token_length = end - start;
        trim_span(&token, &token_length);

        if (token_length == 0U) {
            /* The reference ASCII parser accepts the trailing comma in .move(1,1,). */
            return end == length && *argument_count > 0U;
        }
        if (*argument_count >= 2U ||
            !parse_integer(token, token_length, &arguments[*argument_count])) {
            return false;
        }
        ++*argument_count;
        if (end == length) {
            return true;
        }
        start = end + 1U;
        if (start == length) {
            /* Trailing comma is accepted as an optional empty final argument. */
            return *argument_count > 0U;
        }
    }
    return true;
}

static makcu_ascii_command_t make_error(
    makcu_ascii_error_t error,
    const char *body,
    size_t length)
{
    makcu_ascii_command_t command = {0};
    command.kind = MAKCU_ASCII_COMMAND_ERROR;
    command.error = error;
    const size_t copy_length = length < MAKCU_ASCII_COMMAND_MAX
        ? length : MAKCU_ASCII_COMMAND_MAX;
    if (body != NULL && copy_length > 0U) {
        memcpy(command.body, body, copy_length);
    }
    command.body[copy_length] = '\0';
    return command;
}

static makcu_ascii_command_t parse_command_body(const char *body, size_t length)
{
    makcu_ascii_command_t command = {0};
    const size_t copy_length = length < MAKCU_ASCII_COMMAND_MAX
        ? length : MAKCU_ASCII_COMMAND_MAX;
    memcpy(command.body, body, copy_length);
    command.body[copy_length] = '\0';

    size_t open = 0U;
    while (open < length && body[open] != '(') {
        ++open;
    }
    if (open == length || length == 0U || body[length - 1U] != ')') {
        return make_error(MAKCU_ASCII_ERROR_SYNTAX, body, length);
    }
    for (size_t index = open + 1U; index + 1U < length; ++index) {
        if (body[index] == '(' || body[index] == ')') {
            return make_error(MAKCU_ASCII_ERROR_SYNTAX, body, length);
        }
    }

    const char *name = body;
    size_t name_length = open;
    trim_span(&name, &name_length);
    int32_t arguments[2] = {0, 0};
    uint8_t argument_count = 0U;
    const size_t argument_length = length - open - 2U;
    if (!parse_arguments(&body[open + 1U], argument_length,
                         arguments, &argument_count)) {
        return make_error(MAKCU_ASCII_ERROR_ARGUMENT, body, length);
    }

    if (span_equals(name, name_length, "move")) {
        if (argument_count != 2U || arguments[0] < INT16_MIN ||
            arguments[0] > INT16_MAX || arguments[1] < INT16_MIN ||
            arguments[1] > INT16_MAX) {
            return make_error(MAKCU_ASCII_ERROR_ARGUMENT, body, length);
        }
        command.kind = MAKCU_ASCII_COMMAND_MOVE;
        command.argument_count = argument_count;
        command.argument[0] = arguments[0];
        command.argument[1] = arguments[1];
        return command;
    }
    if (span_equals(name, name_length, "wheel")) {
        if (argument_count != 1U) {
            return make_error(MAKCU_ASCII_ERROR_ARGUMENT, body, length);
        }
        command.kind = MAKCU_ASCII_COMMAND_WHEEL;
        command.argument_count = argument_count;
        command.argument[0] = arguments[0];
        return command;
    }
    if (span_equals(name, name_length, "echo")) {
        if (argument_count > 1U ||
            (argument_count == 1U && arguments[0] != 0 && arguments[0] != 1)) {
            return make_error(MAKCU_ASCII_ERROR_ARGUMENT, body, length);
        }
        command.kind = MAKCU_ASCII_COMMAND_ECHO;
        command.argument_count = argument_count;
        command.query = argument_count == 0U;
        if (argument_count > 0U) {
            command.argument[0] = arguments[0];
        }
        return command;
    }
    if (span_equals(name, name_length, "version") ||
        span_equals(name, name_length, "device")) {
        if (argument_count != 0U) {
            return make_error(MAKCU_ASCII_ERROR_ARGUMENT, body, length);
        }
        command.kind = span_equals(name, name_length, "version")
            ? MAKCU_ASCII_COMMAND_VERSION : MAKCU_ASCII_COMMAND_DEVICE;
        command.query = true;
        return command;
    }
    if (span_equals(name, name_length, "release")) {
        if (argument_count != 0U) {
            return make_error(MAKCU_ASCII_ERROR_ARGUMENT, body, length);
        }
        command.kind = MAKCU_ASCII_COMMAND_RELEASE;
        return command;
    }
    for (size_t index = 0U;
         index < sizeof(s_button_names) / sizeof(s_button_names[0]); ++index) {
        if (!span_equals(name, name_length, s_button_names[index].name)) {
            continue;
        }
        if (argument_count > 1U ||
            (argument_count == 1U && arguments[0] < 0) ||
            (argument_count == 1U && arguments[0] > 2)) {
            return make_error(MAKCU_ASCII_ERROR_ARGUMENT, body, length);
        }
        if (argument_count == 1U && arguments[0] == 2) {
            return make_error(MAKCU_ASCII_ERROR_UNSUPPORTED, body, length);
        }
        command.kind = MAKCU_ASCII_COMMAND_BUTTON;
        command.button_index = s_button_names[index].index;
        command.argument_count = argument_count;
        command.query = argument_count == 0U;
        if (argument_count > 0U) {
            command.argument[0] = arguments[0];
        }
        return command;
    }
    return make_error(MAKCU_ASCII_ERROR_UNSUPPORTED, body, length);
}

void makcu_ascii_parser_init(makcu_ascii_parser_t *parser)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
    }
}

static void dispatch_body(
    makcu_ascii_parser_t *parser,
    makcu_ascii_command_callback_t callback,
    void *context)
{
    if (callback == NULL) {
        return;
    }
    const makcu_ascii_command_t command = parser->overflow
        ? make_error(MAKCU_ASCII_ERROR_TOO_LONG, NULL, 0U)
        : parse_command_body(parser->body, parser->length);
    callback(&command, context);
}

void makcu_ascii_parser_feed(
    makcu_ascii_parser_t *parser,
    const uint8_t *data,
    size_t length,
    makcu_ascii_command_callback_t callback,
    void *context)
{
    if (parser == NULL || (data == NULL && length != 0U)) {
        return;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char value = (char)data[index];
        if (!parser->collecting) {
            if (value == '.') {
                parser->collecting = true;
                parser->overflow = false;
                parser->length = 0U;
            }
            continue;
        }
        if (value == '.') {
            /* Resynchronize at a fresh command prefix after a damaged fragment. */
            parser->overflow = false;
            parser->length = 0U;
            continue;
        }
        if (value == ')') {
            if (parser->length < sizeof(parser->body)) {
                parser->body[parser->length++] = value;
            } else {
                parser->overflow = true;
            }
            dispatch_body(parser, callback, context);
            parser->collecting = false;
            parser->overflow = false;
            parser->length = 0U;
            continue;
        }
        if (parser->length < sizeof(parser->body)) {
            parser->body[parser->length++] = value;
        } else {
            parser->overflow = true;
        }
    }
}

void makcu_ascii_session_init(makcu_ascii_session_t *session)
{
    if (session != NULL) {
        memset(session, 0, sizeof(*session));
        session->echo_enabled = true;
    }
}

void makcu_ascii_session_touch(makcu_ascii_session_t *session, uint32_t now_ms)
{
    if (session == NULL) {
        return;
    }
    session->last_activity_ms = now_ms;
    session->lease_active = true;
}

bool makcu_ascii_session_lease_expired(
    const makcu_ascii_session_t *session,
    uint32_t now_ms)
{
    return session != NULL && session->lease_active &&
        (uint32_t)(now_ms - session->last_activity_ms) >=
            MAKCU_ASCII_INPUT_LEASE_MS;
}

void makcu_ascii_session_expire(makcu_ascii_session_t *session)
{
    if (session != NULL) {
        session->injected_buttons = 0U;
        session->lease_active = false;
    }
}

bool makcu_ascii_session_set_button(
    makcu_ascii_session_t *session,
    uint8_t button_index,
    int32_t state,
    bool *send_report)
{
    if (session == NULL || send_report == NULL ||
        button_index >= MAKCU_ASCII_MOUSE_BUTTON_COUNT ||
        state < 0 || state > 1) {
        return false;
    }
    const uint8_t bit = (uint8_t)(1U << button_index);
    if (state == 1) {
        session->injected_buttons |= bit;
    } else {
        session->injected_buttons &= (uint8_t)~bit;
    }
    *send_report = true;
    return true;
}

uint8_t makcu_ascii_button_state(
    uint8_t button_index,
    uint8_t physical_buttons,
    uint8_t injected_buttons)
{
    if (button_index >= MAKCU_ASCII_MOUSE_BUTTON_COUNT) {
        return 0U;
    }
    const uint8_t bit = (uint8_t)(1U << button_index);
    return (uint8_t)(((physical_buttons & bit) != 0U ? 1U : 0U) |
                     ((injected_buttons & bit) != 0U ? 2U : 0U));
}
