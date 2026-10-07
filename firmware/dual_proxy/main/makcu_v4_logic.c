#include "makcu_v4_logic.h"

#include <limits.h>
#include <string.h>

enum {
    BIN_SYNC_DE = 0,
    BIN_SYNC_AD,
    BIN_LEN_LOW,
    BIN_LEN_HIGH,
    BIN_OPCODE,
    BIN_PAYLOAD,
    BIN_DISCARD,
};

typedef struct {
    const char *name;
    uint8_t opcode;
    bool button;
    uint8_t button_index;
} ascii_command_name_t;

static const ascii_command_name_t s_ascii_names[] = {
    {"version", 0x04U, false, 0U}, {"device", 0x02U, false, 0U},
    {"echo", 0x00U, false, 0U}, {"move", 0x18U, false, 0U},
    {"move_now", 0x67U, false, 0U}, {"wheel", 0x19U, false, 0U},
    {"buttons", 0x10U, false, 0U}, {"left", 0x11U, true, 0U},
    {"right", 0x12U, true, 1U}, {"middle", 0x13U, true, 2U},
    {"side1", 0x14U, true, 3U}, {"side2", 0x15U, true, 4U},
    {"ms1", 0x14U, true, 3U}, {"ms2", 0x15U, true, 4U},
    {"move_mask", 0x16U, false, 0U}, {"wheel_mask", 0x17U, false, 0U},
    {"left_mask", 0x1AU, false, 0U}, {"right_mask", 0x1BU, false, 0U},
    {"middle_mask", 0x1CU, false, 0U}, {"side1_mask", 0x1DU, false, 0U},
    {"side2_mask", 0x1EU, false, 0U},
    {"interpolate", 0x1FU, false, 0U}, {"stream", 0x52U, false, 0U},
    {"phys_buttons", 0x55U, false, 0U},
    {"snapshot", 0x56U, false, 0U},
    {"inject_snapshot", 0x56U, false, 0U},
    {"lock_ml", 0x60U, false, 0U}, {"lock_mr", 0x60U, false, 0U},
    {"lock_mm", 0x60U, false, 0U}, {"lock_ms1", 0x60U, false, 0U},
    {"lock_ms2", 0x60U, false, 0U}, {"lock_mx", 0x60U, false, 0U},
    {"lock_mx+", 0x60U, false, 0U}, {"lock_mx-", 0x60U, false, 0U},
    {"lock_my", 0x60U, false, 0U}, {"lock_my+", 0x60U, false, 0U},
    {"lock_my-", 0x60U, false, 0U}, {"lock_mw", 0x60U, false, 0U},
    {"lock_mw+", 0x60U, false, 0U}, {"lock_mw-", 0x60U, false, 0U},
    {"click", 0x61U, false, 0U}, {"moveto", 0x62U, false, 0U},
    {"getpos", 0x63U, false, 0U}, {"screen", 0x64U, false, 0U},
    {"silent", 0x65U, false, 0U}, {"moving", 0x66U, false, 0U},
    {"baud", 0xA5U, false, 0U},
};

static void dispatch_error(
    makcu_v4_transport_t transport,
    makcu_v4_error_t error,
    uint8_t opcode,
    makcu_v4_command_callback_t callback,
    void *context)
{
    if (callback == NULL) {
        return;
    }
    const makcu_v4_command_t command = {
        .transport = transport,
        .error = error,
        .opcode = opcode,
    };
    callback(&command, context);
}

static uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static void trim_ascii_span(const char **text, size_t *length)
{
    while (*length != 0U && ((*text)[0] == ' ' || (*text)[0] == '\t')) {
        ++(*text);
        --(*length);
    }
    while (*length != 0U &&
           ((*text)[*length - 1U] == ' ' || (*text)[*length - 1U] == '\t')) {
        --(*length);
    }
}

static bool parse_i32(const char *text, size_t length, int32_t *result)
{
    if (text == NULL || length == 0U || result == NULL) {
        return false;
    }
    size_t index = 0U;
    bool negative = false;
    if (text[index] == '-' || text[index] == '+') {
        negative = text[index] == '-';
        if (++index == length) {
            return false;
        }
    }
    const uint32_t limit = negative ? 2147483648U : 2147483647U;
    uint32_t magnitude = 0U;
    for (; index < length; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        const uint32_t digit = (uint32_t)(text[index] - '0');
        if (magnitude > (limit - digit) / 10U) {
            return false;
        }
        magnitude = magnitude * 10U + digit;
    }
    if (negative) {
        *result = magnitude == 2147483648U
            ? INT32_MIN : -(int32_t)magnitude;
    } else {
        *result = (int32_t)magnitude;
    }
    return true;
}

static bool parse_arguments(
    const char *text,
    size_t length,
    makcu_v4_command_t *command)
{
    command->argument_count = 0U;
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
        trim_ascii_span(&token, &token_length);
        if (token_length == 0U ||
            command->argument_count >= MAKCU_V4_ARGUMENT_MAX ||
            !parse_i32(token, token_length,
                       &command->argument[command->argument_count])) {
            return false;
        }
        ++command->argument_count;
        if (end == length) {
            return true;
        }
        start = end + 1U;
    }
    return false;
}

static uint8_t lock_target_for_name(const char *name)
{
    static const char *const names[MAKCU_V4_LOCK_TARGETS] = {
        "lock_ml", "lock_mr", "lock_mm", "lock_ms1", "lock_ms2",
        "lock_mx", "lock_mx+", "lock_mx-", "lock_my", "lock_my+",
        "lock_my-", "lock_mw", "lock_mw+", "lock_mw-",
    };
    for (uint8_t index = 0U; index < MAKCU_V4_LOCK_TARGETS; ++index) {
        if (strcmp(name, names[index]) == 0) {
            return index;
        }
    }
    return UINT8_MAX;
}

static void parse_ascii_line(
    const char *line,
    size_t length,
    makcu_v4_command_callback_t callback,
    void *context)
{
    makcu_v4_command_t command = {
        .transport = MAKCU_V4_TRANSPORT_ASCII,
        .error = MAKCU_V4_ERROR_NONE,
    };
    const size_t source_length = length < MAKCU_V4_ASCII_LINE_MAX
        ? length : MAKCU_V4_ASCII_LINE_MAX;
    memcpy(command.source_line, line, source_length);
    command.source_line[source_length] = '\0';
    const char *text = line;
    trim_ascii_span(&text, &length);
    if (length < 5U || memcmp(text, "km.", 3U) != 0 ||
        text[length - 1U] != ')') {
        dispatch_error(MAKCU_V4_TRANSPORT_ASCII, MAKCU_V4_ERROR_SYNTAX,
                       0U, callback, context);
        return;
    }
    const size_t name_start = 3U;
    size_t open = name_start;
    while (open < length && text[open] != '(') {
        ++open;
    }
    if (open == length || open == name_start || open >= sizeof(command.name)) {
        dispatch_error(MAKCU_V4_TRANSPORT_ASCII, MAKCU_V4_ERROR_SYNTAX,
                       0U, callback, context);
        return;
    }
    const size_t name_length = open - name_start;
    memcpy(command.name, &text[name_start], name_length);
    command.name[name_length] = '\0';
    const char *args = &text[open + 1U];
    size_t args_length = length - open - 2U;
    trim_ascii_span(&args, &args_length);
    if (strcmp(command.name, "stream") == 0) {
        if (args_length == 5U && memcmp(args, "mouse", 5U) == 0) {
            command.argument[0] = 1;
            command.argument_count = 1U;
            command.query = true;
        } else if (args_length >= 7U && memcmp(args, "mouse,", 6U) == 0) {
            const char *enabled = &args[6];
            size_t enabled_length = args_length - 6U;
            trim_ascii_span(&enabled, &enabled_length);
            if (!parse_i32(enabled, enabled_length, &command.argument[1]) ||
                (command.argument[1] != 0 && command.argument[1] != 1)) {
                dispatch_error(MAKCU_V4_TRANSPORT_ASCII,
                               MAKCU_V4_ERROR_ARGUMENT, 0x52U,
                               callback, context);
                return;
            }
            command.argument[0] = 1;
            command.argument_count = 2U;
        } else {
            dispatch_error(MAKCU_V4_TRANSPORT_ASCII,
                           args_length >= 3U && memcmp(args, "key", 3U) == 0
                               ? MAKCU_V4_ERROR_UNSUPPORTED
                               : MAKCU_V4_ERROR_ARGUMENT,
                           0x52U, callback, context);
            return;
        }
    } else if (!parse_arguments(args, args_length, &command)) {
        dispatch_error(MAKCU_V4_TRANSPORT_ASCII, MAKCU_V4_ERROR_ARGUMENT,
                       0U, callback, context);
        return;
    }

    bool found = false;
    for (size_t index = 0U;
         index < sizeof(s_ascii_names) / sizeof(s_ascii_names[0]); ++index) {
        if (strcmp(command.name, s_ascii_names[index].name) == 0) {
            command.opcode = s_ascii_names[index].opcode;
            found = true;
            break;
        }
    }
    if (!found) {
        dispatch_error(MAKCU_V4_TRANSPORT_ASCII,
                       MAKCU_V4_ERROR_UNSUPPORTED, 0U, callback, context);
        return;
    }

    const uint8_t lock_target = lock_target_for_name(command.name);
    if (command.opcode == 0x60U) {
        if (lock_target == UINT8_MAX || command.argument_count > 1U ||
            (command.argument_count == 1U &&
             command.argument[0] != 0 && command.argument[0] != 1)) {
            dispatch_error(MAKCU_V4_TRANSPORT_ASCII,
                           MAKCU_V4_ERROR_ARGUMENT, command.opcode,
                           callback, context);
            return;
        }
        command.query = command.argument_count == 0U;
        command.argument[1] = command.argument[0];
        command.argument[0] = lock_target;
        command.argument_count = command.query ? 1U : 2U;
    } else if (command.opcode >= 0x11U && command.opcode <= 0x15U) {
        if (command.argument_count > 1U ||
            (command.argument_count == 1U &&
             (command.argument[0] < 0 || command.argument[0] > 1))) {
            dispatch_error(MAKCU_V4_TRANSPORT_ASCII,
                           MAKCU_V4_ERROR_ARGUMENT, command.opcode,
                           callback, context);
            return;
        }
        command.query = command.argument_count == 0U;
    } else if (command.opcode == 0x02U || command.opcode == 0x04U ||
               command.opcode == 0xA4U ||
               command.opcode == 0x55U || command.opcode == 0x56U ||
               command.opcode == 0x63U || command.opcode == 0x66U) {
        if (command.argument_count != 0U) {
            dispatch_error(MAKCU_V4_TRANSPORT_ASCII,
                           MAKCU_V4_ERROR_ARGUMENT, command.opcode,
                           callback, context);
            return;
        }
        command.query = true;
    } else if (command.opcode == 0x10U || command.opcode == 0x1FU ||
               command.opcode == 0x64U || command.opcode == 0xA5U) {
        command.query = command.argument_count == 0U;
        if (command.opcode == 0xA5U && command.query) {
            command.opcode = 0xA4U;
        }
    } else if (command.opcode == 0x52U) {
        command.query = command.argument_count == 1U;
    } else {
        command.query = false;
    }

    switch (command.opcode) {
        case 0x00U: /* echo */
            if (command.argument_count > 1U ||
                (command.argument_count == 1U &&
                 (command.argument[0] < 0 || command.argument[0] > 1))) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            } else {
                command.query = command.argument_count == 0U;
            }
            break;
        case 0x16U:
            if (command.argument_count != 4U ||
                command.argument[0] < 0 || command.argument[0] > 1 ||
                command.argument[1] < 0 || command.argument[1] > 1 ||
                command.argument[2] < 0 || command.argument[2] > 1 ||
                command.argument[3] < 0 || command.argument[3] > 1) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0x17U:
            if (command.argument_count != 2U || command.argument[0] < 0 ||
                command.argument[0] > 1 || command.argument[1] < 0 ||
                command.argument[1] > 1) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0x18U:
            if (command.argument_count < 2U || command.argument_count > 7U ||
                command.argument[0] < INT16_MIN || command.argument[0] > INT16_MAX ||
                command.argument[1] < INT16_MIN || command.argument[1] > INT16_MAX) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            } else {
                command.argument_count = 2U;
            }
            break;
        case 0x19U:
            if (command.argument_count != 1U || command.argument[0] < INT16_MIN ||
                command.argument[0] > INT16_MAX) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0x1AU: case 0x1BU: case 0x1CU: case 0x1DU: case 0x1EU:
            if (command.argument_count != 1U || command.argument[0] < 0 ||
                command.argument[0] > 1) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0x1FU:
            if (command.argument_count > 1U || (command.argument_count == 1U &&
                (command.argument[0] < 0 ||
                 (command.argument[0] > 100 && command.argument[0] != 255)))) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0x61U:
            if (command.argument_count < 1U || command.argument_count > 3U ||
                command.argument[0] < 1 || command.argument[0] > 5 ||
                (command.argument_count >= 2U &&
                 (command.argument[1] < 1 || command.argument[1] > 255)) ||
                (command.argument_count == 3U &&
                 (command.argument[2] < 0 || command.argument[2] > 5000))) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0x62U:
            if (command.argument_count != 2U || command.argument[0] < 0 ||
                command.argument[0] > 32767 || command.argument[1] < 0 ||
                command.argument[1] > 32767) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0x65U:
            if (command.argument_count != 2U ||
                command.argument[0] < 0 || command.argument[0] > 32767 ||
                command.argument[1] < 0 || command.argument[1] > 32767) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0x64U:
            if (command.argument_count != 0U && command.argument_count != 2U) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            } else if (command.argument_count == 2U &&
                       (command.argument[0] < 1 || command.argument[0] > 32767 ||
                        command.argument[1] < 1 || command.argument[1] > 32767)) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0x67U:
            if (command.argument_count != 2U || command.argument[0] < INT16_MIN ||
                command.argument[0] > INT16_MAX || command.argument[1] < INT16_MIN ||
                command.argument[1] > INT16_MAX) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0xA5U:
            if (command.argument_count > 1U || (command.argument_count == 1U &&
                command.argument[0] != 0 && command.argument[0] != 115200 &&
                command.argument[0] != 4000000)) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        case 0x10U:
            if (command.argument_count > 2U ||
                (command.argument_count == 1U &&
                 (command.argument[0] < 0 || command.argument[0] > 3)) ||
                (command.argument_count == 2U &&
                 (command.argument[0] < 0 || command.argument[0] > 3 ||
                  command.argument[1] < 0 || command.argument[1] > 1000))) {
                command.error = MAKCU_V4_ERROR_ARGUMENT;
            }
            break;
        default:
            break;
    }
    if (command.error != MAKCU_V4_ERROR_NONE) {
        dispatch_error(MAKCU_V4_TRANSPORT_ASCII, command.error,
                       command.opcode, callback, context);
        return;
    }

    if (callback != NULL) {
        callback(&command, context);
    }
}

void makcu_v4_ascii_parser_init(makcu_v4_ascii_parser_t *parser)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
    }
}

void makcu_v4_ascii_parser_feed(
    makcu_v4_ascii_parser_t *parser,
    const uint8_t *data,
    size_t length,
    makcu_v4_command_callback_t callback,
    void *context)
{
    if (parser == NULL || (data == NULL && length != 0U)) {
        return;
    }
    for (size_t index = 0U; index < length; ++index) {
        const uint8_t value = data[index];
        if (value == '\r' || value == '\n') {
            if (value == '\n' && parser->saw_cr) {
                parser->saw_cr = false;
                continue;
            }
            if (parser->overflow) {
                dispatch_error(MAKCU_V4_TRANSPORT_ASCII,
                               MAKCU_V4_ERROR_TOO_LONG, 0U,
                               callback, context);
            } else if (parser->length != 0U) {
                parse_ascii_line(parser->line, parser->length,
                                 callback, context);
            }
            parser->length = 0U;
            parser->overflow = false;
            parser->saw_cr = value == '\r';
            continue;
        }
        parser->saw_cr = false;
        if (value < 0x20U || value > 0x7EU) {
            parser->length = 0U;
            parser->overflow = false;
            continue;
        }
        if (parser->length < MAKCU_V4_ASCII_LINE_MAX) {
            parser->line[parser->length++] = (char)value;
            parser->line[parser->length] = '\0';
        } else {
            parser->overflow = true;
        }
    }
}

static bool binary_shape_supported(uint8_t opcode, const uint8_t *payload,
                                   uint16_t length, bool *query)
{
    *query = false;
    switch (opcode) {
        case 0x02U: case 0x04U: case 0x55U: case 0x56U:
        case 0x63U: case 0x66U:
            *query = true;
            return length == 0U;
        case 0x10U:
            *query = length == 0U;
            return length == 0U || (length == 1U && payload[0] <= 1U);
        case 0x11U: case 0x12U: case 0x13U: case 0x14U: case 0x15U:
            *query = length == 0U;
            return length == 0U || (length == 1U && payload[0] <= 1U);
        case 0x16U:
            return length == 4U && payload[0] <= 1U && payload[1] <= 1U &&
                   payload[2] <= 1U && payload[3] <= 1U;
        case 0x17U:
            return length == 2U && payload[0] <= 1U && payload[1] <= 1U;
        case 0x18U:
            return length == 4U;
        case 0x19U:
            return length == 2U;
        case 0x1AU: case 0x1BU: case 0x1CU: case 0x1DU: case 0x1EU:
            return length == 1U && payload[0] <= 1U;
        case 0x1FU:
            *query = length == 0U;
            return length == 0U || (length == 1U &&
                   (payload[0] <= 100U || payload[0] == 255U));
        case 0x52U:
            if (length == 1U) {
                *query = true;
                return payload[0] == 1U;
            }
            return length == 2U && payload[0] == 1U && payload[1] <= 1U;
        case 0x60U:
            *query = length == 1U;
            return (length == 1U && payload[0] < MAKCU_V4_LOCK_TARGETS) ||
                   (length == 2U && payload[0] < MAKCU_V4_LOCK_TARGETS &&
                    payload[1] <= 1U);
        case 0x61U:
            return (length == 1U && payload[0] >= 1U && payload[0] <= 5U) ||
                   (length == 2U && payload[0] >= 1U && payload[0] <= 5U &&
                    payload[1] != 0U) ||
                   (length == 4U && payload[0] >= 1U && payload[0] <= 5U &&
                    payload[1] != 0U &&
                    read_u16_le(&payload[2]) <= (uint16_t)5000U);
        case 0x62U: case 0x65U: case 0x67U:
            return length == 4U;
        case 0x64U:
            *query = length == 0U;
            return length == 0U || (length == 4U &&
                read_u16_le(payload) >= 1U && read_u16_le(payload) <= 32767U &&
                read_u16_le(&payload[2]) >= 1U &&
                read_u16_le(&payload[2]) <= 32767U);
        case 0xA4U:
            *query = true;
            return length == 0U;
        case 0xA5U:
            return length == 4U;
        default:
            return false;
    }
}

static void binary_dispatch(
    makcu_v4_binary_parser_t *parser,
    makcu_v4_command_callback_t callback,
    void *context)
{
    bool query = false;
    if (!binary_shape_supported(parser->opcode, parser->payload,
                                parser->payload_length, &query)) {
        dispatch_error(MAKCU_V4_TRANSPORT_BINARY,
                       (parser->opcode == 0x20U || parser->opcode == 0x21U ||
                        parser->opcode == 0x22U || parser->opcode == 0x23U ||
                        parser->opcode == 0x24U || parser->opcode == 0x25U ||
                        parser->opcode == 0x26U || parser->opcode == 0x27U ||
                        parser->opcode == 0x28U || parser->opcode == 0x29U ||
                        parser->opcode == 0x2AU || parser->opcode == 0x2BU ||
                        parser->opcode == 0x40U || parser->opcode == 0x41U ||
                        parser->opcode == 0x51U)
                           ? MAKCU_V4_ERROR_UNSUPPORTED
                           : MAKCU_V4_ERROR_ARGUMENT,
                       parser->opcode, callback, context);
        return;
    }
    if (callback == NULL) {
        return;
    }
    makcu_v4_command_t command = {
        .transport = MAKCU_V4_TRANSPORT_BINARY,
        .error = MAKCU_V4_ERROR_NONE,
        .opcode = parser->opcode,
        .payload_length = (uint8_t)parser->payload_length,
        .query = query,
    };
    if (parser->payload_length != 0U) {
        memcpy(command.payload, parser->payload, parser->payload_length);
    }
    callback(&command, context);
}

void makcu_v4_binary_parser_init(makcu_v4_binary_parser_t *parser)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
    }
}

void makcu_v4_binary_parser_feed(
    makcu_v4_binary_parser_t *parser,
    const uint8_t *data,
    size_t length,
    makcu_v4_command_callback_t callback,
    void *context)
{
    if (parser == NULL || (data == NULL && length != 0U)) {
        return;
    }
    for (size_t index = 0U; index < length; ++index) {
        const uint8_t value = data[index];
        switch (parser->state) {
            case BIN_SYNC_DE:
                if (value == 0xDEU) {
                    parser->state = BIN_SYNC_AD;
                }
                break;
            case BIN_SYNC_AD:
                parser->state = value == 0xADU ? BIN_LEN_LOW :
                    (value == 0xDEU ? BIN_SYNC_AD : BIN_SYNC_DE);
                break;
            case BIN_LEN_LOW:
                parser->length_low = value;
                parser->state = BIN_LEN_HIGH;
                break;
            case BIN_LEN_HIGH:
                parser->payload_length = (uint16_t)(parser->length_low |
                    ((uint16_t)value << 8U));
                parser->state = BIN_OPCODE;
                break;
            case BIN_OPCODE:
                parser->opcode = value;
                parser->payload_received = 0U;
                /*
                 * MAKCU's legacy AIO baud frames count CMD in LEN:
                 * A5 declares 5 for its fixed four-byte payload, while the
                 * legacy A4 query declares 1 and carries no payload. Accept
                 * these alongside the current LEN=4 / LEN=0 envelopes.
                 */
                if (parser->opcode == 0xA5U &&
                    parser->payload_length == 5U) {
                    parser->payload_length = 4U;
                } else if (parser->opcode == 0xA4U &&
                           parser->payload_length == 1U) {
                    parser->payload_length = 0U;
                }
                if (parser->payload_length == 0U) {
                    binary_dispatch(parser, callback, context);
                    parser->state = BIN_SYNC_DE;
                } else if (parser->payload_length > MAKCU_V4_MAX_PAYLOAD) {
                    dispatch_error(MAKCU_V4_TRANSPORT_BINARY,
                                   MAKCU_V4_ERROR_TOO_LONG, parser->opcode,
                                   callback, context);
                    parser->state = BIN_DISCARD;
                } else {
                    parser->state = BIN_PAYLOAD;
                }
                break;
            case BIN_PAYLOAD:
                parser->payload[parser->payload_received++] = value;
                if (parser->payload_received == parser->payload_length) {
                    binary_dispatch(parser, callback, context);
                    parser->state = BIN_SYNC_DE;
                }
                break;
            case BIN_DISCARD:
                if (--parser->payload_length == 0U) {
                    parser->state = BIN_SYNC_DE;
                }
                break;
            default:
                parser->state = BIN_SYNC_DE;
                break;
        }
    }
}

void makcu_v4_stream_parser_init(makcu_v4_stream_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
    makcu_v4_ascii_parser_init(&parser->ascii);
    makcu_v4_binary_parser_init(&parser->binary);
}

void makcu_v4_stream_parser_tick(
    makcu_v4_stream_parser_t *parser,
    uint32_t now_ms)
{
    const uint32_t timeout_ms = parser != NULL && parser->binary.state == BIN_DISCARD
        ? 8000U : 250U;
    if (parser == NULL || parser->mode == 0U ||
        (uint32_t)(now_ms - parser->last_byte_ms) < timeout_ms) {
        return;
    }
    /* A damaged or truncated binary frame must not capture later ASCII input. */
    if (parser->mode == 2U) {
        makcu_v4_binary_parser_init(&parser->binary);
    } else {
        makcu_v4_ascii_parser_init(&parser->ascii);
    }
    parser->mode = 0U;
}

void makcu_v4_stream_parser_feed(
    makcu_v4_stream_parser_t *parser,
    const uint8_t *data,
    size_t length,
    uint32_t now_ms,
    makcu_v4_command_callback_t callback,
    void *context)
{
    if (parser == NULL || (data == NULL && length != 0U)) {
        return;
    }
    for (size_t index = 0U; index < length; ++index) {
        const uint8_t value = data[index];
        if (parser->mode == 0U) {
            if (value == 0xDEU) {
                parser->mode = 2U;
            } else if (value >= 0x20U && value <= 0x7EU) {
                parser->mode = 1U;
            } else {
                continue;
            }
        } else if (parser->mode == 1U && value == 0xDEU) {
            makcu_v4_ascii_parser_init(&parser->ascii);
            parser->mode = 2U;
        }
        parser->last_byte_ms = now_ms;
        if (parser->mode == 2U) {
            makcu_v4_binary_parser_feed(&parser->binary, &value, 1U,
                                        callback, context);
            if (parser->binary.state == BIN_SYNC_DE) {
                parser->mode = 0U;
            }
        } else {
            makcu_v4_ascii_parser_feed(&parser->ascii, &value, 1U,
                                       callback, context);
            if (value == '\n' || value == '\r') {
                parser->mode = 0U;
            }
        }
    }
}

bool makcu_v4_encode_frame(
    uint8_t opcode,
    const uint8_t *payload,
    uint16_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    if (output_length == NULL || output == NULL ||
        (payload == NULL && payload_length != 0U) ||
        (size_t)payload_length + 5U > output_capacity) {
        return false;
    }
    output[0] = 0xDEU;
    output[1] = 0xADU;
    output[2] = (uint8_t)payload_length;
    output[3] = (uint8_t)(payload_length >> 8U);
    output[4] = opcode;
    if (payload_length != 0U) {
        memcpy(&output[5], payload, payload_length);
    }
    *output_length = (size_t)payload_length + 5U;
    return true;
}

bool makcu_v4_encode_error(
    uint8_t opcode,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    const uint8_t rejected = 0xFFU;
    return makcu_v4_encode_frame(opcode, &rejected, 1U, output,
                                 output_capacity, output_length);
}

void makcu_v4_state_init(makcu_v4_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->buttons_enabled = false;
    state->echo_enabled = false;
    state->screen_width = 1920U;
    state->screen_height = 1080U;
    state->pointer_x = state->screen_width / 2U;
    state->pointer_y = state->screen_height / 2U;
    state->interpolate_percent = 0U;
    state->last_injected_interval_ms = 1U;
}

void makcu_v4_state_touch(makcu_v4_state_t *state, uint32_t now_ms)
{
    if (state == NULL) {
        return;
    }
    state->lease_active = true;
    state->lease_deadline_ms = now_ms + MAKCU_V4_INPUT_LEASE_MS;
}

bool makcu_v4_state_lease_expired(
    const makcu_v4_state_t *state,
    uint32_t now_ms)
{
    if (state == NULL || !state->lease_active) {
        return false;
    }
    if (state->scheduled_click_deadline_ms != 0U &&
        (int32_t)(state->scheduled_click_deadline_ms - now_ms) > 0) {
        return false;
    }
    return (int32_t)(now_ms - state->lease_deadline_ms) >= 0;
}

void makcu_v4_state_release(makcu_v4_state_t *state)
{
    if (state != NULL) {
        state->injected_buttons = 0U;
        state->lease_active = false;
        state->scheduled_click_deadline_ms = 0U;
    }
}

bool makcu_v4_state_set_button(
    makcu_v4_state_t *state,
    uint8_t button,
    uint8_t value)
{
    if (state == NULL || button >= MAKCU_V4_MOUSE_BUTTONS || value > 1U) {
        return false;
    }
    const uint8_t bit = (uint8_t)(1U << button);
    if (value != 0U) {
        state->injected_buttons |= bit;
    } else {
        state->injected_buttons &= (uint8_t)~bit;
    }
    return true;
}

uint8_t makcu_v4_state_ascii_button(
    const makcu_v4_state_t *state,
    uint8_t button)
{
    if (state == NULL || button >= MAKCU_V4_MOUSE_BUTTONS) {
        return 0U;
    }
    const uint8_t bit = (uint8_t)(1U << button);
    return (uint8_t)(((state->physical_buttons & bit) != 0U ? 1U : 0U) |
                     ((state->injected_buttons & bit) != 0U ? 2U : 0U));
}

uint8_t makcu_v4_state_effective_physical_buttons(
    const makcu_v4_state_t *state)
{
    return state == NULL ? 0U :
        (uint8_t)(state->physical_buttons & (uint8_t)~state->button_mask);
}

bool makcu_v4_state_schedule_click(
    makcu_v4_state_t *state,
    uint32_t now_ms,
    uint16_t hold_ms)
{
    if (state == NULL || hold_ms > 5000U) {
        return false;
    }
    if (hold_ms == 0U) {
        hold_ms = 55U;
    }
    const uint32_t release_at = now_ms + hold_ms;
    state->scheduled_click_deadline_ms = release_at;
    state->lease_active = true;
    const uint32_t safe_release_deadline = release_at + MAKCU_V4_INPUT_LEASE_MS;
    if ((int32_t)(safe_release_deadline - state->lease_deadline_ms) > 0) {
        state->lease_deadline_ms = safe_release_deadline;
    }
    return true;
}

bool makcu_v4_normalize_interpolate(uint8_t requested, uint8_t *normalized)
{
    if (normalized == NULL) {
        return false;
    }
    if (requested == 255U) {
        *normalized = 255U;
        return true;
    }
    if (requested > 100U) {
        return false;
    }
    /* Integer round-half-up to the nearest 25%; boundaries are 13/38/63/88. */
    *normalized = (uint8_t)(((uint16_t)requested + 12U) / 25U * 25U);
    return true;
}

uint8_t makcu_v4_interpolate_slots(uint8_t normalized, uint8_t prior_interval_ms)
{
    if (normalized == 255U) {
        /* AUTO rounds the prior command interval to the nearest supported slot tier. */
        uint16_t slots = ((uint16_t)prior_interval_ms + 2U) / 5U * 5U;
        return (uint8_t)(slots > 20U ? 20U : slots);
    }
    if (normalized > 100U) {
        return 0U;
    }
    uint8_t canonical = 0U;
    (void)makcu_v4_normalize_interpolate(normalized, &canonical);
    return (uint8_t)(canonical / 5U);
}

static uint32_t abs_i32_u32(int32_t value)
{
    /* Widen before negation so INT32_MIN is well-defined. */
    const int64_t widened = value;
    return (uint32_t)(widened < 0 ? -widened : widened);
}

static uint32_t ceil_div_u32(uint32_t value, uint32_t divisor)
{
    return value / divisor + (value % divisor != 0U ? 1U : 0U);
}

uint32_t makcu_v4_motion_steps(int32_t x, int32_t y, int32_t wheel,
                               uint32_t minimum_steps)
{
    uint32_t steps = minimum_steps == 0U ? 1U : minimum_steps;
    const uint32_t x_steps = ceil_div_u32(abs_i32_u32(x), 32767U);
    const uint32_t y_steps = ceil_div_u32(abs_i32_u32(y), 32767U);
    const uint32_t wheel_steps = ceil_div_u32(abs_i32_u32(wheel), 127U);
    if (x_steps > steps) steps = x_steps;
    if (y_steps > steps) steps = y_steps;
    if (wheel_steps > steps) steps = wheel_steps;
    return steps;
}

int32_t makcu_v4_motion_step(int32_t remaining, uint32_t steps_left)
{
    if (steps_left <= 1U) return remaining;
    /* Runtime steps are capped at ceil(2^31 / 127) = 16909321 by wheel range. */
    return remaining / (int32_t)steps_left;
}

void makcu_v4_state_track_move(
    makcu_v4_state_t *state,
    int32_t x,
    int32_t y)
{
    if (state == NULL || state->screen_width == 0U ||
        state->screen_height == 0U) {
        return;
    }
    int64_t next_x = (int64_t)state->pointer_x + x;
    int64_t next_y = (int64_t)state->pointer_y + y;
    if (next_x < 0) {
        next_x = 0;
    } else if (next_x >= state->screen_width) {
        next_x = (int32_t)state->screen_width - 1;
    }
    if (next_y < 0) {
        next_y = 0;
    } else if (next_y >= state->screen_height) {
        next_y = (int32_t)state->screen_height - 1;
    }
    state->pointer_x = (uint16_t)next_x;
    state->pointer_y = (uint16_t)next_y;
}
