#include "uart0_control.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "uart0_protocol_router.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "bridge_protocol.h"
#include "dual_proxy_runtime_config.h"
#include "makcu_ascii_logic.h"
#include "makcu_v4_logic.h"
#include "diag_stream.h"
#include "dual_proxy_app.h"
#include "hid_device_profile.h"
#include "hid_host_mouse.h"
#include "mouse_motion_smoother.h"
#include "m_udp_smoothing.h"
#include "pc_hid_output.h"
#include "stats_snapshot.h"
#include "uart1_link.h"
#include "uart0_output.h"

#define CONTROL_UART UART_NUM_0
#define CONTROL_BAUD_P 921600
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
#define CONTROL_BAUD_M 115200
#else
#define CONTROL_BAUD_M 921600
#endif
#define CONTROL_RX_BUFFER_SIZE 4096
#define CONTROL_TX_BUFFER_SIZE 4096
#define CONTROL_LEASE_MS 1500
#define CONTROL_TASK_STACK_SIZE 6144
#define CONTROL_POLL_MS 1U

#if DUAL_PROXY_ENABLE_MAKCU_V4_API && DUAL_PROXY_ENABLE_MAKCU_ASCII_API
#error "V4 MAK_API and legacy ASCII mode are exclusive on UART0"
#endif

static const char *TAG = "dual_uart0";
static dual_software_report_callback_t s_report_callback;
static dual_software_release_callback_t s_release_callback;
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API
static dual_software_physical_buttons_callback_t s_physical_buttons_callback;
#endif
static uint8_t s_role;
static bool s_control_started;
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API
static volatile bool s_makcu_ascii_requested;
static makcu_ascii_session_t s_makcu_session;
static uint16_t s_makcu_sequence;
#endif

#if DUAL_PROXY_ENABLE_MAKCU_V4_API
#define MAKCU_V4_EVENT_CAPACITY 64U
#define MAKCU_V4_JOB_CAPACITY 8U

typedef struct {
    uint8_t button;
    uint8_t value;
    uint8_t physical_mask;
    bool binary;
    bool ascii;
} makcu_v4_button_event_t;

typedef enum {
    MAKCU_V4_JOB_MOVE = 1,
    MAKCU_V4_JOB_CLICK,
    MAKCU_V4_JOB_SILENT,
} makcu_v4_job_kind_t;

typedef struct {
    makcu_v4_job_kind_t kind;
    int32_t remaining_x;
    int32_t remaining_y;
    int32_t remaining_wheel;
    uint32_t steps_left;
    uint32_t due_ms;
    uint8_t buttons;
    uint8_t button;
    uint8_t clicks_left;
    uint8_t click_phase;
    uint8_t restore_buttons;
    uint8_t smoothing_slots;
    uint16_t hold_ms;
    bool started;
} makcu_v4_job_t;

static volatile bool s_makcu_v4_requested;
static makcu_v4_state_t s_makcu_v4_state;
static uint16_t s_makcu_v4_sequence;
static uint32_t s_makcu_v4_baud = CONTROL_BAUD_M;
static uint32_t s_makcu_v4_last_move_ms;
static bool s_makcu_v4_last_move_valid;
static portMUX_TYPE s_makcu_v4_input_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_makcu_v4_physical_buttons;
static uint8_t s_makcu_v4_button_mask;
static uint8_t s_makcu_v4_move_mask;
static uint8_t s_makcu_v4_wheel_mask;
static bool s_makcu_v4_binary_stream;
static uint8_t s_makcu_v4_ascii_buttons_mode;
static uint16_t s_makcu_v4_ascii_buttons_period_ms;
static bool s_makcu_v4_event_overflow;
static bool s_makcu_v4_overflow_binary;
static bool s_makcu_v4_cancel_requested;
static makcu_v4_button_event_t s_makcu_v4_events[MAKCU_V4_EVENT_CAPACITY];
static uint8_t s_makcu_v4_event_read;
static uint8_t s_makcu_v4_event_write;
static uint8_t s_makcu_v4_event_count;
static makcu_v4_job_t s_makcu_v4_jobs[MAKCU_V4_JOB_CAPACITY];
static uint8_t s_makcu_v4_job_read;
static uint8_t s_makcu_v4_job_write;
static uint8_t s_makcu_v4_job_count;
static uint8_t s_makcu_v4_transient_buttons;
static uint16_t s_makcu_v4_pointer_x = 960U;
static uint16_t s_makcu_v4_pointer_y = 540U;
static uint16_t s_makcu_v4_screen_width = 1920U;
static uint16_t s_makcu_v4_screen_height = 1080U;
#endif

typedef struct {
    bool active;
    bool sequence_initialized;
    uint16_t expected_sequence;
    TickType_t last_activity;
    uint64_t received;
    uint64_t accepted;
    uint64_t rejected;
    uint64_t discontinuities;
    uint64_t mouse_reports;
    uint64_t last_mouse_reports;
} control_state_t;

static control_state_t s_state;

/* A5/UDP 只在 M 展开；Makcu 的槽数原样传给 P。 */
static m_udp_smoothing_t s_legacy_channel;
static bool s_legacy_cancel_requested;
void dual_uart0_control_cancel_software_input(void)
{
    __atomic_store_n(&s_legacy_cancel_requested, true, __ATOMIC_RELEASE);
}
static volatile bool s_v4_owner = true;
static void legacy_motion_reset(void) { m_udp_smoothing_reset(&s_legacy_channel); }
static void legacy_motion_enqueue(const dual_frame_t *frame)
{
    m_udp_smoothing_enqueue(&s_legacy_channel, frame);
}
static void legacy_motion_tick(void)
{
    dual_frame_t frame;
    if (m_udp_smoothing_tick(&s_legacy_channel, esp_timer_get_time(), &frame) &&
        s_report_callback != NULL) s_report_callback(&frame);
}


static uint8_t s_injected_blob[HID_PROFILE_MAX_BLOB];
static hid_device_profile_t s_injected_profile;
static uint32_t s_injected_length;
static uint32_t s_injected_received;
static uint32_t s_injected_crc32;
static TickType_t s_injected_started;
static bool s_injection_active;

#if DUAL_PROXY_ENABLE_MAKCU_V4_API
static void makcu_v4_set_stream_flags_locked(bool binary_enabled,
                                             uint8_t ascii_mode)
{
    s_makcu_v4_binary_stream = binary_enabled;
    s_makcu_v4_ascii_buttons_mode = ascii_mode;
}

static bool makcu_v4_queue_event_locked(
    uint8_t button,
    uint8_t value,
    uint8_t physical_mask,
    bool binary,
    bool ascii)
{
    if (s_makcu_v4_event_count >= MAKCU_V4_EVENT_CAPACITY) {
        s_makcu_v4_overflow_binary = s_makcu_v4_overflow_binary || binary;
        s_makcu_v4_event_overflow = true;
        s_makcu_v4_event_count = 0U;
        s_makcu_v4_event_read = s_makcu_v4_event_write;
        makcu_v4_set_stream_flags_locked(false, 0U);
        return false;
    }
    s_makcu_v4_events[s_makcu_v4_event_write] = (makcu_v4_button_event_t){
        .button = button,
        .value = value,
        .physical_mask = physical_mask,
        .binary = binary,
        .ascii = ascii,
    };
    s_makcu_v4_event_write =
        (uint8_t)((s_makcu_v4_event_write + 1U) % MAKCU_V4_EVENT_CAPACITY);
    ++s_makcu_v4_event_count;
    return true;
}

static void makcu_v4_queue_baseline_locked(bool binary, bool ascii)
{
    uint8_t cumulative = 0U;
    const uint8_t physical = s_makcu_v4_physical_buttons & 0x1FU;
    for (uint8_t button = 0U; button < MAKCU_V4_MOUSE_BUTTONS; ++button) {
        const uint8_t bit = (uint8_t)(1U << button);
        if ((physical & bit) == 0U) {
            continue;
        }
        cumulative |= bit;
        if (!makcu_v4_queue_event_locked(button, 1U, cumulative,
                                         binary, ascii)) {
            return;
        }
    }
}

void dual_uart0_control_v4_physical_buttons(uint8_t buttons)
{
    buttons &= 0x1FU;
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    const uint8_t previous = s_makcu_v4_physical_buttons;
    s_makcu_v4_physical_buttons = buttons;
    const uint8_t changed = (uint8_t)(previous ^ buttons);
    const bool binary = s_makcu_v4_binary_stream;
    const bool ascii = s_makcu_v4_ascii_buttons_mode != 0U;
    uint8_t cumulative = previous;
    for (uint8_t button = 0U; button < MAKCU_V4_MOUSE_BUTTONS; ++button) {
        const uint8_t bit = (uint8_t)(1U << button);
        if ((changed & bit) == 0U) {
            continue;
        }
        const bool pressed = (buttons & bit) != 0U;
        cumulative = pressed ? (uint8_t)(cumulative | bit)
                             : (uint8_t)(cumulative & (uint8_t)~bit);
        if ((binary || ascii) &&
            !makcu_v4_queue_event_locked(button, pressed ? 1U : 0U,
                                         cumulative, binary, ascii)) {
            break;
        }
    }
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
}

void dual_uart0_control_v4_cancel_input(void)
{
    dual_uart0_control_cancel_software_input();
    /*
     * HID-host callbacks can run independently of the UART control task.
     * Do not mutate the job ring or call the software-release callback here;
     * request cancellation for the owner task to consume on its next 1 ms tick.
     */
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    s_makcu_v4_cancel_requested = true;
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
}

static bool makcu_v4_take_cancel_request(void)
{
    bool requested;
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    requested = s_makcu_v4_cancel_requested;
    s_makcu_v4_cancel_requested = false;
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
    return requested;
}

void dual_uart0_control_v4_get_physical_masks(
    uint8_t *button_mask,
    uint8_t *move_mask,
    uint8_t *wheel_mask)
{
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    if (button_mask != NULL) {
        *button_mask = s_v4_owner ? s_makcu_v4_button_mask : 0U;
    }
    if (move_mask != NULL) {
        *move_mask = s_v4_owner ? s_makcu_v4_move_mask : 0U;
    }
    if (wheel_mask != NULL) {
        *wheel_mask = s_v4_owner ? s_makcu_v4_wheel_mask : 0U;
    }
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
}

void dual_uart0_control_v4_track_physical_move(int32_t x, int32_t y)
{
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    int64_t next_x = (int64_t)s_makcu_v4_pointer_x + x;
    int64_t next_y = (int64_t)s_makcu_v4_pointer_y + y;
    if (next_x < 0) {
        next_x = 0;
    } else if (next_x >= s_makcu_v4_screen_width) {
        next_x = (int64_t)s_makcu_v4_screen_width - 1;
    }
    if (next_y < 0) {
        next_y = 0;
    } else if (next_y >= s_makcu_v4_screen_height) {
        next_y = (int64_t)s_makcu_v4_screen_height - 1;
    }
    s_makcu_v4_pointer_x = (uint16_t)next_x;
    s_makcu_v4_pointer_y = (uint16_t)next_y;
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
}

static void makcu_v4_set_screen_and_pointer(uint16_t width, uint16_t height,
                                           uint16_t x, uint16_t y)
{
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    s_makcu_v4_screen_width = width;
    s_makcu_v4_screen_height = height;
    s_makcu_v4_pointer_x = x < width ? x : (uint16_t)(width - 1U);
    s_makcu_v4_pointer_y = y < height ? y : (uint16_t)(height - 1U);
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
}

static void makcu_v4_sync_pointer(makcu_v4_state_t *state)
{
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    state->pointer_x = s_makcu_v4_pointer_x;
    state->pointer_y = s_makcu_v4_pointer_y;
    state->screen_width = s_makcu_v4_screen_width;
    state->screen_height = s_makcu_v4_screen_height;
    state->physical_buttons = s_makcu_v4_physical_buttons;
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
}

static uint16_t makcu_v4_lock_mask_from_masks(uint8_t buttons,
                                             uint8_t movement,
                                             uint8_t wheel)
{
    uint16_t locks = (uint16_t)(buttons & 0x1FU);
    if ((movement & 0x03U) == 0x03U) locks |= (uint16_t)(1U << 5U);
    if ((movement & 0x02U) != 0U) locks |= (uint16_t)(1U << 6U);
    if ((movement & 0x01U) != 0U) locks |= (uint16_t)(1U << 7U);
    if ((movement & 0x0CU) == 0x0CU) locks |= (uint16_t)(1U << 8U);
    if ((movement & 0x04U) != 0U) locks |= (uint16_t)(1U << 9U);
    if ((movement & 0x08U) != 0U) locks |= (uint16_t)(1U << 10U);
    if ((wheel & 0x03U) == 0x03U) locks |= (uint16_t)(1U << 11U);
    if ((wheel & 0x02U) != 0U) locks |= (uint16_t)(1U << 12U);
    if ((wheel & 0x01U) != 0U) locks |= (uint16_t)(1U << 13U);
    return locks;
}

static void makcu_v4_set_masks(makcu_v4_state_t *state,
                               uint8_t button_mask,
                               uint8_t move_mask,
                               uint8_t wheel_mask)
{
    state->button_mask = button_mask;
    state->move_mask = move_mask;
    state->wheel_mask = wheel_mask;
    state->lock_mask = makcu_v4_lock_mask_from_masks(
        button_mask, move_mask, wheel_mask);
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    s_makcu_v4_button_mask = button_mask;
    s_makcu_v4_move_mask = move_mask;
    s_makcu_v4_wheel_mask = wheel_mask;
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
}

static void makcu_v4_set_lock(makcu_v4_state_t *state, uint8_t target,
                              bool enabled)
{
    uint8_t button_mask = state->button_mask;
    uint8_t move_mask = state->move_mask;
    uint8_t wheel_mask = state->wheel_mask;
    uint8_t *mask = target < 5U ? &button_mask
        : (target < 11U ? &move_mask : &wheel_mask);
    uint8_t bits = 0U;
    switch (target) {
        case 0U: case 1U: case 2U: case 3U: case 4U:
            bits = (uint8_t)(1U << target);
            break;
        case 5U: bits = 0x03U; break;
        case 6U: bits = 0x02U; break;
        case 7U: bits = 0x01U; break;
        case 8U: bits = 0x0CU; break;
        case 9U: bits = 0x04U; break;
        case 10U: bits = 0x08U; break;
        case 11U: bits = 0x03U; break;
        case 12U: bits = 0x02U; break;
        case 13U: bits = 0x01U; break;
        default: return;
    }
    *mask = enabled ? (uint8_t)(*mask | bits)
                    : (uint8_t)(*mask & (uint8_t)~bits);
    makcu_v4_set_masks(state, button_mask, move_mask, wheel_mask);
}

static void makcu_v4_set_subscriptions(bool binary_enabled,
                                       uint8_t ascii_mode,
                                       uint16_t ascii_period_ms)
{
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    s_makcu_v4_event_read = 0U;
    s_makcu_v4_event_write = 0U;
    s_makcu_v4_event_count = 0U;
    s_makcu_v4_event_overflow = false;
    s_makcu_v4_overflow_binary = false;
    s_makcu_v4_ascii_buttons_period_ms = ascii_period_ms;
    makcu_v4_set_stream_flags_locked(binary_enabled, ascii_mode);
    makcu_v4_queue_baseline_locked(binary_enabled, ascii_mode != 0U);
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
}

static bool makcu_v4_pop_event(makcu_v4_button_event_t *event)
{
    bool available = false;
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    if (s_makcu_v4_event_count != 0U) {
        *event = s_makcu_v4_events[s_makcu_v4_event_read];
        s_makcu_v4_event_read =
            (uint8_t)((s_makcu_v4_event_read + 1U) % MAKCU_V4_EVENT_CAPACITY);
        --s_makcu_v4_event_count;
        available = true;
    }
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
    return available;
}

static bool makcu_v4_take_overflow(bool *binary)
{
    bool overflow;
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    overflow = s_makcu_v4_event_overflow;
    if (binary != NULL) {
        *binary = s_makcu_v4_overflow_binary;
    }
    s_makcu_v4_event_overflow = false;
    s_makcu_v4_overflow_binary = false;
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
    return overflow;
}

static uint16_t makcu_v4_read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static int16_t makcu_v4_read_i16(const uint8_t *bytes)
{
    return (int16_t)makcu_v4_read_u16(bytes);
}

static void makcu_v4_write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void makcu_v4_write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static esp_err_t makcu_v4_write(const uint8_t *bytes, size_t length)
{
    return dual_uart0_output_write_makcu(bytes, length);
}

static void makcu_v4_send_ascii_echo(const makcu_v4_command_t *command);

static void makcu_v4_send_error(const makcu_v4_command_t *command)
{
    if (command->transport == MAKCU_V4_TRANSPORT_BINARY) {
        uint8_t frame[MAKCU_V4_MAX_PAYLOAD + 5U];
        size_t frame_length = 0U;
        if (makcu_v4_encode_error(command->opcode, frame, sizeof(frame),
                                  &frame_length)) {
            (void)makcu_v4_write(frame, frame_length);
        }
    } else {
        if (command->opcode != 0x04U &&
            s_makcu_v4_state.echo_enabled &&
            command->source_line[0] != '\0') {
            makcu_v4_send_ascii_echo(command);
        }
        static const uint8_t error[] = "ERR\r\n>>> ";
        (void)makcu_v4_write(error, sizeof(error) - 1U);
    }
}

static void makcu_v4_send_binary(const makcu_v4_command_t *command,
                                 const uint8_t *payload,
                                 uint16_t payload_length)
{
    uint8_t frame[MAKCU_V4_MAX_PAYLOAD + 5U];
    size_t frame_length = 0U;
    if (makcu_v4_encode_frame(command->opcode, payload, payload_length,
                              frame, sizeof(frame), &frame_length)) {
        (void)makcu_v4_write(frame, frame_length);
    }
}

static void makcu_v4_send_ascii(const char *result)
{
    char response[96];
    const int length = snprintf(response, sizeof(response), "%s\r\n>>> ", result);
    if (length > 0 && (size_t)length < sizeof(response)) {
        (void)makcu_v4_write((const uint8_t *)response, (size_t)length);
    }
}

static void makcu_v4_send_ascii_echo(const makcu_v4_command_t *command)
{
    if (command->source_line[0] == '\0') {
        return;
    }
    char response[MAKCU_V4_ASCII_LINE_MAX + 16U];
    const int length = snprintf(response, sizeof(response), "%s\r\n",
                                command->source_line);
    if (length > 0 && (size_t)length < sizeof(response)) {
        (void)makcu_v4_write((const uint8_t *)response, (size_t)length);
    }
}

static bool makcu_v4_ascii_echo_enabled_for(
    const makcu_v4_command_t *command)
{
    return command->transport == MAKCU_V4_TRANSPORT_ASCII &&
           command->opcode != 0x04U && s_makcu_v4_state.echo_enabled;
}

static void makcu_v4_send_ascii_query(const makcu_v4_command_t *command,
                                      const char *result)
{
    if (makcu_v4_ascii_echo_enabled_for(command)) {
        makcu_v4_send_ascii_echo(command);
    }
    makcu_v4_send_ascii(result);
}

static void makcu_v4_finish_ascii_setter(const makcu_v4_command_t *command,
                                         bool echo_on_entry)
{
    if (echo_on_entry) {
        makcu_v4_send_ascii_echo(command);
        static const uint8_t prompt[] = ">>> ";
        (void)makcu_v4_write(prompt, sizeof(prompt) - 1U);
    }
}

static void makcu_v4_submit_mouse_report(uint8_t buttons,
                                         int16_t x,
                                         int16_t y,
                                         int8_t wheel,
                                         int8_t pan,
                                         uint8_t smoothing_slots)
{
    if (s_report_callback == NULL) {
        return;
    }
    dual_frame_t report = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_MOUSE_REPORT,
        .sequence = ++s_makcu_v4_sequence,
        .payload_length = 8U,
    };
    report.payload[0] = buttons;
    makcu_v4_write_u16(&report.payload[1], (uint16_t)x);
    makcu_v4_write_u16(&report.payload[3], (uint16_t)y);
    report.payload[5] = (uint8_t)wheel;
    report.payload[6] = (uint8_t)pan;
    report.payload[7] = smoothing_slots;
    s_report_callback(&report);
    ++s_state.mouse_reports;
}

static void makcu_v4_emit_events(void)
{
    bool overflow_binary = false;
    const bool overflow = makcu_v4_take_overflow(&overflow_binary);
    if (overflow && overflow_binary) {
        static const uint8_t overflow_payload[] = {1U, 0xFFU, 0xFFU};
        uint8_t frame[8];
        size_t length = 0U;
        if (makcu_v4_encode_frame(0x53U, overflow_payload,
                                  sizeof(overflow_payload), frame,
                                  sizeof(frame), &length)) {
            (void)makcu_v4_write(frame, length);
        }
    }
    makcu_v4_button_event_t event;
    if (makcu_v4_pop_event(&event)) {
        bool binary_enabled;
        uint8_t ascii_mode;
        portENTER_CRITICAL(&s_makcu_v4_input_mux);
        binary_enabled = s_makcu_v4_binary_stream;
        ascii_mode = s_makcu_v4_ascii_buttons_mode;
        portEXIT_CRITICAL(&s_makcu_v4_input_mux);
        if (event.binary && binary_enabled) {
            const uint8_t payload[] = {1U, event.button, event.value};
            uint8_t frame[8];
            size_t length = 0U;
            if (makcu_v4_encode_frame(0x53U, payload, sizeof(payload),
                                      frame, sizeof(frame), &length)) {
                (void)makcu_v4_write(frame, length);
            }
        }
        if (event.ascii && ascii_mode != 0U) {
            if (ascii_mode == 3U) {
                (void)makcu_v4_write(&event.physical_mask, 1U);
            } else {
                uint8_t frame[8] = {'k', 'm', '.', event.physical_mask,
                                    '\r', '\n', 0U, 0U};
                (void)makcu_v4_write(frame, 6U);
            }
        }
    }
}

static void makcu_v4_get_stream_state(bool *binary, uint8_t *ascii_mode,
                                      uint16_t *ascii_period_ms)
{
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    if (binary != NULL) *binary = s_makcu_v4_binary_stream;
    if (ascii_mode != NULL) *ascii_mode = s_makcu_v4_ascii_buttons_mode;
    if (ascii_period_ms != NULL) {
        *ascii_period_ms = s_makcu_v4_ascii_buttons_period_ms;
    }
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
}

static uint8_t makcu_v4_software_buttons(void);
static bool makcu_v4_enqueue_move(int32_t x, int32_t y, int32_t wheel,
                                  uint32_t now_ms, bool move_now);
static bool makcu_v4_enqueue_click(uint8_t button, uint8_t count,
                                   uint16_t hold_ms, uint32_t now_ms);
static bool makcu_v4_enqueue_silent(int32_t x, int32_t y, uint32_t now_ms);
static bool makcu_v4_has_pending_move(void);

static int32_t makcu_v4_clamp_coordinate(int64_t value, uint16_t extent)
{
    if (value < 0) return 0;
    if (value >= extent) return (int32_t)extent - 1;
    return (int32_t)value;
}

static void makcu_v4_projected_pointer(int32_t *x, int32_t *y)
{
    int32_t projected_x = s_makcu_v4_state.pointer_x;
    int32_t projected_y = s_makcu_v4_state.pointer_y;
    uint8_t index = s_makcu_v4_job_read;
    for (uint8_t count = 0U; count < s_makcu_v4_job_count; ++count) {
        const makcu_v4_job_t *job = &s_makcu_v4_jobs[index];
        if (job->kind == MAKCU_V4_JOB_MOVE ||
            (job->kind == MAKCU_V4_JOB_SILENT && job->click_phase != 0U)) {
            projected_x = makcu_v4_clamp_coordinate(
                (int64_t)projected_x + job->remaining_x,
                s_makcu_v4_state.screen_width);
            projected_y = makcu_v4_clamp_coordinate(
                (int64_t)projected_y + job->remaining_y,
                s_makcu_v4_state.screen_height);
        }
        index = (uint8_t)((index + 1U) % MAKCU_V4_JOB_CAPACITY);
    }
    if (x != NULL) *x = projected_x;
    if (y != NULL) *y = projected_y;
}

static uint32_t makcu_v4_now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void makcu_v4_handle_command(const makcu_v4_command_t *command,
                                    void *context)
{
    (void)context;
    if (command == NULL) return;
    if (command->error != MAKCU_V4_ERROR_NONE) {
        makcu_v4_send_error(command);
        return;
    }
    if (s_role != DUAL_ROLE_MOUSE_HOST) {
        makcu_v4_send_error(command);
        return;
    }
    makcu_v4_sync_pointer(&s_makcu_v4_state);
    const bool echo_on_entry = makcu_v4_ascii_echo_enabled_for(command);

    bool stream_enabled = false;
    uint8_t ascii_mode = 0U;
    uint16_t ascii_period_ms = 0U;
    makcu_v4_get_stream_state(&stream_enabled, &ascii_mode,
                              &ascii_period_ms);
    uint8_t result[MAKCU_V4_MAX_PAYLOAD] = {0U};
    char text_result[64];
    bool is_query = command->query;

    const uint32_t now_ms = makcu_v4_now_ms();
    const bool binary = command->transport == MAKCU_V4_TRANSPORT_BINARY;
    switch (command->opcode) {
        case 0x00U: /* echo */
            if (is_query) {
                (void)snprintf(text_result, sizeof(text_result), "%u",
                    s_makcu_v4_state.echo_enabled ? 1U : 0U);
                makcu_v4_send_ascii_query(command, text_result);
            } else {
                makcu_v4_finish_ascii_setter(command, echo_on_entry);
                s_makcu_v4_state.echo_enabled = command->argument[0] != 0;
            }
            return;
        case 0x02U: /* DEVICE */
            if (binary) {
                result[0] = 0x01U;
                makcu_v4_send_binary(command, result, 1U);
            } else {
                makcu_v4_send_ascii_query(command, "mouse");
            }
            return;
        case 0x04U: /* text protocol identity; binary version is bridge API level */
            if (binary) {
                makcu_v4_write_u32(result, 4U);
                makcu_v4_send_binary(command, result, 4U);
            } else {
                makcu_v4_send_ascii_query(command, "km.MAKCU");
            }
            return;
        case 0x10U: /* ASCII event mode; binary mouse button subscription */
            if (!binary) {
                if (is_query) {
                    (void)snprintf(text_result, sizeof(text_result), "%u",
                                   (unsigned)ascii_mode);
                    makcu_v4_send_ascii_query(command, text_result);
                    return;
                }
                ascii_mode = (uint8_t)command->argument[0];
                ascii_period_ms = command->argument_count > 1U
                    ? (uint16_t)command->argument[1] : 0U;
                makcu_v4_set_subscriptions(stream_enabled, ascii_mode,
                                           ascii_period_ms);
                s_makcu_v4_state.ascii_buttons_mode = ascii_mode;
                s_makcu_v4_state.ascii_buttons_period_ms = ascii_period_ms;
                makcu_v4_finish_ascii_setter(command, echo_on_entry);
                return;
            }
            if (is_query) {
                result[0] = stream_enabled ? 1U : 0U;
                makcu_v4_send_binary(command, result, 1U);
                return;
            }
            stream_enabled = command->payload[0] != 0U;
            makcu_v4_set_subscriptions(stream_enabled, ascii_mode,
                                       ascii_period_ms);
            s_makcu_v4_state.buttons_enabled = stream_enabled;
            s_makcu_v4_state.mouse_stream_enabled = stream_enabled;
            return;
        case 0x52U: /* INPUT_STREAM(mouse) shares the 0x10 binary state */
            if (is_query) {
                if (binary) {
                    result[0] = stream_enabled ? 1U : 0U;
                    makcu_v4_send_binary(command, result, 1U);
                } else {
                    (void)snprintf(text_result, sizeof(text_result), "%u",
                                   stream_enabled ? 1U : 0U);
                    makcu_v4_send_ascii_query(command, text_result);
                }
                return;
            }
            stream_enabled = binary ? command->payload[1] != 0U
                                    : command->argument[1] != 0;
            makcu_v4_set_subscriptions(stream_enabled, ascii_mode,
                                       ascii_period_ms);
            s_makcu_v4_state.buttons_enabled = stream_enabled;
            s_makcu_v4_state.mouse_stream_enabled = stream_enabled;
            makcu_v4_finish_ascii_setter(command, echo_on_entry);
            return;
        case 0x11U: case 0x12U: case 0x13U: case 0x14U: case 0x15U: {
            const uint8_t button = (uint8_t)(command->opcode - 0x11U);
            if (is_query) {
                if (binary) {
                    result[0] = (uint8_t)((makcu_v4_software_buttons() >> button) & 1U);
                    makcu_v4_send_binary(command, result, 1U);
                } else {
                    makcu_v4_state_t button_state = s_makcu_v4_state;
                    button_state.injected_buttons = makcu_v4_software_buttons();
                    (void)snprintf(text_result, sizeof(text_result), "%u",
                        (unsigned)makcu_v4_state_ascii_button(
                            &button_state, button));
                    makcu_v4_send_ascii_query(command, text_result);
                }
                return;
            }
            const uint8_t value = binary ? command->payload[0]
                                         : (uint8_t)command->argument[0];
            if (!makcu_v4_state_set_button(&s_makcu_v4_state, button, value)) {
                makcu_v4_send_error(command);
                return;
            }
            makcu_v4_state_touch(&s_makcu_v4_state, now_ms);
            makcu_v4_submit_mouse_report(makcu_v4_software_buttons(), 0, 0, 0, 0, 0U);
            makcu_v4_finish_ascii_setter(command, echo_on_entry);
            return;
        }
        case 0x16U: case 0x17U:
        case 0x1AU: case 0x1BU: case 0x1CU: case 0x1DU: case 0x1EU: {
            uint8_t button_mask = s_makcu_v4_state.button_mask;
            uint8_t move_mask = s_makcu_v4_state.move_mask;
            uint8_t wheel_mask = s_makcu_v4_state.wheel_mask;
            if (command->opcode == 0x16U) {
                const uint8_t left = binary ? command->payload[0]
                                            : (uint8_t)command->argument[0];
                const uint8_t right = binary ? command->payload[1]
                                             : (uint8_t)command->argument[1];
                const uint8_t down = binary ? command->payload[2]
                                            : (uint8_t)command->argument[2];
                const uint8_t up = binary ? command->payload[3]
                                          : (uint8_t)command->argument[3];
                move_mask = (uint8_t)(left | (right << 1U) |
                                      (down << 2U) | (up << 3U));
            } else if (command->opcode == 0x17U) {
                const uint8_t down = binary ? command->payload[0]
                                            : (uint8_t)command->argument[0];
                const uint8_t up = binary ? command->payload[1]
                                          : (uint8_t)command->argument[1];
                wheel_mask = (uint8_t)(down | (up << 1U));
            } else {
                const uint8_t value = binary ? command->payload[0]
                                             : (uint8_t)command->argument[0];
                const uint8_t button = (uint8_t)(command->opcode - 0x1AU);
                const uint8_t bit = (uint8_t)(1U << button);
                button_mask = value != 0U ? (uint8_t)(button_mask | bit)
                                          : (uint8_t)(button_mask & (uint8_t)~bit);
            }
            makcu_v4_set_masks(&s_makcu_v4_state, button_mask,
                               move_mask, wheel_mask);
            makcu_v4_finish_ascii_setter(command, echo_on_entry);
            return;
        }
        case 0x18U: case 0x19U: case 0x67U: {
            int32_t x = 0, y = 0, wheel = 0;
            if (command->opcode == 0x19U) {
                wheel = binary ? makcu_v4_read_i16(command->payload)
                               : command->argument[0];
            } else if (binary) {
                x = makcu_v4_read_i16(command->payload);
                y = makcu_v4_read_i16(&command->payload[2]);
            } else {
                x = command->argument[0];
                y = command->argument[1];
            }
            if (!makcu_v4_enqueue_move(x, y, wheel, now_ms,
                                       command->opcode == 0x67U)) {
                makcu_v4_send_error(command);
                return;
            }
            makcu_v4_finish_ascii_setter(command, echo_on_entry);
            return;
        }
        case 0x1FU: { /* INTERPOLATE: 255 is the documented AUTO value. */
            if (is_query) {
                if (binary) {
                    result[0] = s_makcu_v4_state.interpolate_percent;
                    makcu_v4_send_binary(command, result, 1U);
                } else {
                    (void)snprintf(text_result, sizeof(text_result), "%u",
                        (unsigned)s_makcu_v4_state.interpolate_percent);
                    makcu_v4_send_ascii_query(command, text_result);
                }
                return;
            }
            const uint8_t requested = binary
                ? command->payload[0] : (uint8_t)command->argument[0];
            uint8_t normalized = 0U;
            if (!makcu_v4_normalize_interpolate(requested, &normalized)) {
                makcu_v4_send_error(command);
                return;
            }
            s_makcu_v4_state.interpolate_percent = normalized;
            if (binary) {
                result[0] = normalized;
                makcu_v4_send_binary(command, result, 1U);
            } else {
                makcu_v4_finish_ascii_setter(command, echo_on_entry);
            }
            return;
        }
        case 0x55U: /* raw physical button state */
            if (binary) {
                result[0] = s_makcu_v4_state.physical_buttons & 0x1FU;
                makcu_v4_send_binary(command, result, 1U);
            } else {
                (void)snprintf(text_result, sizeof(text_result), "%u",
                    (unsigned)(s_makcu_v4_state.physical_buttons & 0x1FU));
                makcu_v4_send_ascii_query(command, text_result);
            }
            return;
        case 0x56U: /* 40-byte bridge injected-state snapshot */
            if (!binary) {
                makcu_v4_send_error(command);
                return;
            }
            result[0] = makcu_v4_software_buttons();
            makcu_v4_write_u16(&result[1], s_makcu_v4_state.lock_mask);
            result[3] = 0U; /* keyboard modifier state is outside mouse scope */
            memset(&result[4], 0, 32U);
            result[36] = s_makcu_v4_state.interpolate_percent;
            result[37] = result[38] = result[39] = 0U;
            makcu_v4_send_binary(command, result, 40U);
            return;
        case 0x60U: { /* LOCK aliases share the physical-mask state. */
            const uint8_t target = binary ? command->payload[0]
                                          : (uint8_t)command->argument[0];
            if (is_query) {
                const uint8_t locked = (uint8_t)(
                    (s_makcu_v4_state.lock_mask >> target) & 1U);
                if (binary) {
                    result[0] = locked;
                    makcu_v4_send_binary(command, result, 1U);
                } else {
                    (void)snprintf(text_result, sizeof(text_result), "%u",
                                   (unsigned)locked);
                    makcu_v4_send_ascii_query(command, text_result);
                }
                return;
            }
            const uint8_t enabled = binary ? command->payload[1]
                : (uint8_t)command->argument[1];
            makcu_v4_set_lock(&s_makcu_v4_state, target, enabled != 0U);
            makcu_v4_finish_ascii_setter(command, echo_on_entry);
            return;
        }
        case 0x61U: { /* click(button=1..5[,count[,hold_ms]]) */
            const uint8_t button_id = binary ? command->payload[0]
                : (uint8_t)command->argument[0];
            const uint8_t count = binary
                ? (command->payload_length >= 2U ? command->payload[1] : 1U)
                : (command->argument_count >= 2U
                    ? (uint8_t)command->argument[1] : 1U);
            const uint16_t hold_ms = binary
                ? (command->payload_length == 4U
                    ? makcu_v4_read_u16(&command->payload[2]) : 0U)
                : (command->argument_count >= 3U
                    ? (uint16_t)command->argument[2] : 0U);
            if (!makcu_v4_enqueue_click((uint8_t)(button_id - 1U), count,
                                        hold_ms, now_ms)) {
                makcu_v4_send_error(command);
                return;
            }
            makcu_v4_finish_ascii_setter(command, echo_on_entry);
            return;
        }
        case 0x62U: { /* MOVETO on the tracked virtual screen */
            uint16_t x = binary ? makcu_v4_read_u16(command->payload)
                                : (uint16_t)command->argument[0];
            uint16_t y = binary ? makcu_v4_read_u16(&command->payload[2])
                                : (uint16_t)command->argument[1];
            if (x >= s_makcu_v4_state.screen_width) {
                x = (uint16_t)(s_makcu_v4_state.screen_width - 1U);
            }
            if (y >= s_makcu_v4_state.screen_height) {
                y = (uint16_t)(s_makcu_v4_state.screen_height - 1U);
            }
            int32_t projected_x, projected_y;
            makcu_v4_projected_pointer(&projected_x, &projected_y);
            const int32_t dx = (int32_t)x - projected_x;
            const int32_t dy = (int32_t)y - projected_y;
            if (!makcu_v4_enqueue_move(dx, dy, 0, now_ms, false)) {
                makcu_v4_send_error(command);
                return;
            }
            makcu_v4_finish_ascii_setter(command, echo_on_entry);
            return;
        }
        case 0x63U: /* GETPOS */
            if (binary) {
                makcu_v4_write_u16(result, s_makcu_v4_state.pointer_x);
                makcu_v4_write_u16(&result[2], s_makcu_v4_state.pointer_y);
                makcu_v4_send_binary(command, result, 4U);
            } else {
                (void)snprintf(text_result, sizeof(text_result), "km.getpos(%u,%u)",
                    (unsigned)s_makcu_v4_state.pointer_x,
                    (unsigned)s_makcu_v4_state.pointer_y);
                makcu_v4_send_ascii_query(command, text_result);
            }
            return;
        case 0x64U: /* SCREEN GET/SET; setter recenters tracked pointer. */
            if (is_query) {
                if (binary) {
                    makcu_v4_write_u16(result, s_makcu_v4_state.screen_width);
                    makcu_v4_write_u16(&result[2], s_makcu_v4_state.screen_height);
                    makcu_v4_send_binary(command, result, 4U);
                } else {
                    (void)snprintf(text_result, sizeof(text_result), "km.screen(%u,%u)",
                        (unsigned)s_makcu_v4_state.screen_width,
                        (unsigned)s_makcu_v4_state.screen_height);
                    makcu_v4_send_ascii_query(command, text_result);
                }
                return;
            }
            {
                const uint16_t width = binary
                    ? makcu_v4_read_u16(command->payload)
                    : (uint16_t)command->argument[0];
                const uint16_t height = binary
                    ? makcu_v4_read_u16(&command->payload[2])
                    : (uint16_t)command->argument[1];
                const uint16_t center_x = (uint16_t)(width / 2U);
                const uint16_t center_y = (uint16_t)(height / 2U);
                makcu_v4_set_screen_and_pointer(width, height,
                                                center_x, center_y);
                makcu_v4_sync_pointer(&s_makcu_v4_state);
                makcu_v4_finish_ascii_setter(command, echo_on_entry);
            }
            return;
        case 0x65U: { /* SILENT: move+left, release+move back */
            uint16_t target_x = binary ? makcu_v4_read_u16(command->payload)
                                       : (uint16_t)command->argument[0];
            uint16_t target_y = binary ? makcu_v4_read_u16(&command->payload[2])
                                       : (uint16_t)command->argument[1];
            if (target_x >= s_makcu_v4_state.screen_width) {
                target_x = (uint16_t)(s_makcu_v4_state.screen_width - 1U);
            }
            if (target_y >= s_makcu_v4_state.screen_height) {
                target_y = (uint16_t)(s_makcu_v4_state.screen_height - 1U);
            }
            int32_t projected_x, projected_y;
            makcu_v4_projected_pointer(&projected_x, &projected_y);
            const int32_t dx = (int32_t)target_x - projected_x;
            const int32_t dy = (int32_t)target_y - projected_y;
            if (!makcu_v4_enqueue_silent(dx, dy, now_ms)) {
                makcu_v4_send_error(command);
                return;
            }
            makcu_v4_finish_ascii_setter(command, echo_on_entry);
            return;
        }
        case 0x66U: { /* MOVING */
            const uint8_t moving = makcu_v4_has_pending_move() ? 1U : 0U;
            if (binary) {
                result[0] = moving;
                makcu_v4_send_binary(command, result, 1U);
            } else {
                (void)snprintf(text_result, sizeof(text_result), "%u",
                               (unsigned)moving);
                makcu_v4_send_ascii_query(command, text_result);
            }
            return;
        }
        case 0xA4U: /* GET_BAUD */
            if (binary) {
                makcu_v4_write_u32(result, s_makcu_v4_baud);
                makcu_v4_send_binary(command, result, 4U);
            } else {
                (void)snprintf(text_result, sizeof(text_result), "%" PRIu32,
                               s_makcu_v4_baud);
                makcu_v4_send_ascii_query(command, text_result);
            }
            return;
        case 0xA5U: { /* SET_BAUD, volatile and M-only */
            const uint32_t requested = binary
                ? (uint32_t)command->payload[0] |
                    ((uint32_t)command->payload[1] << 8U) |
                    ((uint32_t)command->payload[2] << 16U) |
                    ((uint32_t)command->payload[3] << 24U)
                : (uint32_t)command->argument[0];
            const uint32_t baud = requested == 0U
                ? CONTROL_BAUD_M : requested;
            if ((baud != CONTROL_BAUD_M && baud != 4000000U) ||
                uart_wait_tx_done(CONTROL_UART, pdMS_TO_TICKS(100U)) != ESP_OK) {
                makcu_v4_send_error(command);
                return;
            }
            makcu_v4_finish_ascii_setter(command, echo_on_entry);
            if (uart_set_baudrate(CONTROL_UART, (int)baud) != ESP_OK) {
                makcu_v4_send_error(command);
                return;
            }
            s_makcu_v4_baud = baud;
            return;
        }
        default:
            makcu_v4_send_error(command);
            return;
    }
}

static uint8_t makcu_v4_software_buttons(void)
{
    return (uint8_t)(s_makcu_v4_state.injected_buttons |
                     s_makcu_v4_transient_buttons);
}

static uint32_t makcu_v4_move_steps(int32_t x, int32_t y, int32_t wheel)
{
    /* Only split when an individual MouseReport field would overflow. */
    return makcu_v4_motion_steps(x, y, wheel, 1U);
}

static uint8_t makcu_v4_move_smoothing_slots(uint32_t now_ms, bool move_now)
{
    if (move_now) {
        return 0U;
    }
    uint8_t prior_interval = s_makcu_v4_state.last_injected_interval_ms;
    if (s_makcu_v4_state.interpolate_percent == 255U &&
        s_makcu_v4_last_move_valid) {
        const uint32_t elapsed = now_ms - s_makcu_v4_last_move_ms;
        prior_interval = elapsed > UINT8_MAX ? UINT8_MAX
            : (elapsed == 0U ? 1U : (uint8_t)elapsed);
    }
    return makcu_v4_interpolate_slots(
        s_makcu_v4_state.interpolate_percent, prior_interval);
}

static uint32_t makcu_v4_pending_delay_ms(void)
{
    uint32_t total = 0U;
    uint8_t index = s_makcu_v4_job_read;
    for (uint8_t count = 0U; count < s_makcu_v4_job_count; ++count) {
        const makcu_v4_job_t *job = &s_makcu_v4_jobs[index];
        if (job->kind == MAKCU_V4_JOB_MOVE) {
            total += job->steps_left == 0U ? 1U : job->steps_left;
        } else if (job->kind == MAKCU_V4_JOB_CLICK) {
            const uint32_t hold = job->hold_ms == 0U ? 75U : job->hold_ms;
            total += (uint32_t)job->clicks_left * (hold + 1U);
        } else if (job->kind == MAKCU_V4_JOB_SILENT) {
            total += job->click_phase == 0U ? 2U : 1U;
        }
        index = (uint8_t)((index + 1U) % MAKCU_V4_JOB_CAPACITY);
    }
    return total;
}

static bool makcu_v4_enqueue_move(int32_t x, int32_t y, int32_t wheel,
                                  uint32_t now_ms, bool move_now)
{
    if (x == 0 && y == 0 && wheel == 0) return true;
    const uint8_t smoothing_slots = makcu_v4_move_smoothing_slots(now_ms, move_now);
    if (s_makcu_v4_job_count != 0U) {
        const uint8_t last_index = (uint8_t)(
            (s_makcu_v4_job_write + MAKCU_V4_JOB_CAPACITY - 1U) %
            MAKCU_V4_JOB_CAPACITY);
        makcu_v4_job_t *last = &s_makcu_v4_jobs[last_index];
        if (last->kind == MAKCU_V4_JOB_MOVE && !last->started &&
            last->smoothing_slots == smoothing_slots &&
            last->due_ms == now_ms) {
            const int64_t sum_x = (int64_t)last->remaining_x + x;
            const int64_t sum_y = (int64_t)last->remaining_y + y;
            const int64_t sum_wheel = (int64_t)last->remaining_wheel + wheel;
            if (sum_x < INT32_MIN || sum_x > INT32_MAX ||
                sum_y < INT32_MIN || sum_y > INT32_MAX ||
                sum_wheel < INT32_MIN || sum_wheel > INT32_MAX) {
                return false;
            }
            last->remaining_x = (int32_t)sum_x;
            last->remaining_y = (int32_t)sum_y;
            last->remaining_wheel = (int32_t)sum_wheel;
            last->steps_left = makcu_v4_move_steps(
                last->remaining_x, last->remaining_y, last->remaining_wheel);
            goto accepted;
        }
    }
    if (s_makcu_v4_job_count >= MAKCU_V4_JOB_CAPACITY) return false;
    s_makcu_v4_jobs[s_makcu_v4_job_write] = (makcu_v4_job_t){
        .kind = MAKCU_V4_JOB_MOVE,
        .remaining_x = x,
        .remaining_y = y,
        .remaining_wheel = wheel,
        .steps_left = makcu_v4_move_steps(x, y, wheel),
        .due_ms = now_ms,
        .smoothing_slots = smoothing_slots,
    };
    s_makcu_v4_job_write = (uint8_t)(
        (s_makcu_v4_job_write + 1U) % MAKCU_V4_JOB_CAPACITY);
    ++s_makcu_v4_job_count;
accepted:
    if (s_makcu_v4_last_move_valid) {
        const uint32_t elapsed = now_ms - s_makcu_v4_last_move_ms;
        s_makcu_v4_state.last_injected_interval_ms = elapsed > UINT8_MAX
            ? UINT8_MAX : (elapsed == 0U ? 1U : (uint8_t)elapsed);
    }
    s_makcu_v4_last_move_ms = now_ms;
    s_makcu_v4_last_move_valid = true;
    makcu_v4_state_touch(&s_makcu_v4_state, now_ms);
    return true;
}

static bool makcu_v4_enqueue_click(uint8_t button, uint8_t count,
                                   uint16_t hold_ms, uint32_t now_ms)
{
    if (s_makcu_v4_job_count >= MAKCU_V4_JOB_CAPACITY) return false;
    s_makcu_v4_jobs[s_makcu_v4_job_write] = (makcu_v4_job_t){
        .kind = MAKCU_V4_JOB_CLICK,
        .button = button,
        .clicks_left = count,
        .hold_ms = hold_ms,
        .due_ms = now_ms + makcu_v4_pending_delay_ms(),
    };
    s_makcu_v4_job_write = (uint8_t)(
        (s_makcu_v4_job_write + 1U) % MAKCU_V4_JOB_CAPACITY);
    ++s_makcu_v4_job_count;
    const uint32_t default_hold = hold_ms == 0U ? 75U : hold_ms;
    const uint32_t deadline = s_makcu_v4_jobs[
        (uint8_t)((s_makcu_v4_job_write + MAKCU_V4_JOB_CAPACITY - 1U) %
                  MAKCU_V4_JOB_CAPACITY)].due_ms +
        (uint32_t)count * (default_hold + 1U);
    s_makcu_v4_state.lease_active = true;
    s_makcu_v4_state.scheduled_click_deadline_ms = deadline;
    s_makcu_v4_state.lease_deadline_ms = deadline + MAKCU_V4_INPUT_LEASE_MS;
    return true;
}

static bool makcu_v4_enqueue_silent(int32_t x, int32_t y, uint32_t now_ms)
{
    if (s_makcu_v4_job_count >= MAKCU_V4_JOB_CAPACITY) return false;
    s_makcu_v4_jobs[s_makcu_v4_job_write] = (makcu_v4_job_t){
        .kind = MAKCU_V4_JOB_SILENT,
        .remaining_x = x,
        .remaining_y = y,
        .steps_left = makcu_v4_motion_steps(-x, -y, 0, 1U),
        .due_ms = now_ms + makcu_v4_pending_delay_ms(),
    };
    s_makcu_v4_job_write = (uint8_t)(
        (s_makcu_v4_job_write + 1U) % MAKCU_V4_JOB_CAPACITY);
    ++s_makcu_v4_job_count;
    const uint32_t deadline = s_makcu_v4_jobs[
        (uint8_t)((s_makcu_v4_job_write + MAKCU_V4_JOB_CAPACITY - 1U) %
                  MAKCU_V4_JOB_CAPACITY)].due_ms + 2U;
    s_makcu_v4_state.lease_active = true;
    s_makcu_v4_state.scheduled_click_deadline_ms = deadline;
    s_makcu_v4_state.lease_deadline_ms = deadline + MAKCU_V4_INPUT_LEASE_MS;
    return true;
}

static bool makcu_v4_has_pending_move(void)
{
    uint8_t index = s_makcu_v4_job_read;
    for (uint8_t count = 0U; count < s_makcu_v4_job_count; ++count) {
        if (s_makcu_v4_jobs[index].kind == MAKCU_V4_JOB_MOVE ||
            s_makcu_v4_jobs[index].kind == MAKCU_V4_JOB_SILENT) {
            return true;
        }
        index = (uint8_t)((index + 1U) % MAKCU_V4_JOB_CAPACITY);
    }
    return false;
}

static void makcu_v4_advance_jobs(uint32_t now_ms)
{
    if (s_makcu_v4_job_count == 0U) return;
    makcu_v4_job_t *job = &s_makcu_v4_jobs[s_makcu_v4_job_read];
    if ((int32_t)(now_ms - job->due_ms) < 0) return;
    if (job->kind == MAKCU_V4_JOB_MOVE) {
        job->started = true;
        const uint32_t steps = job->steps_left == 0U ? 1U : job->steps_left;
        const int32_t x = makcu_v4_motion_step(job->remaining_x, steps);
        const int32_t y = makcu_v4_motion_step(job->remaining_y, steps);
        const int32_t wheel = makcu_v4_motion_step(job->remaining_wheel, steps);
        job->remaining_x -= x;
        job->remaining_y -= y;
        job->remaining_wheel -= wheel;
        if (job->steps_left != 0U) --job->steps_left;
        makcu_v4_submit_mouse_report(makcu_v4_software_buttons(),
            (int16_t)x, (int16_t)y, (int8_t)wheel, 0, job->smoothing_slots);
        dual_uart0_control_v4_track_physical_move(x, y);
        makcu_v4_sync_pointer(&s_makcu_v4_state);
        if (job->remaining_x == 0 && job->remaining_y == 0 &&
            job->remaining_wheel == 0) {
            s_makcu_v4_job_read = (uint8_t)(
                (s_makcu_v4_job_read + 1U) % MAKCU_V4_JOB_CAPACITY);
            --s_makcu_v4_job_count;
        } else {
            job->due_ms = now_ms + 1U;
        }
        return;
    }
    if (job->kind == MAKCU_V4_JOB_CLICK) {
        const uint8_t bit = (uint8_t)(1U << job->button);
        if (job->click_phase == 0U) {
            s_makcu_v4_transient_buttons |= bit;
            makcu_v4_submit_mouse_report(makcu_v4_software_buttons(),
                                         0, 0, 0, 0, 0U);
            uint16_t hold_ms = job->hold_ms;
            if (hold_ms == 0U) hold_ms = (uint16_t)(35U + esp_random() % 41U);
            job->click_phase = 1U;
            job->due_ms = now_ms + hold_ms;
        } else {
            s_makcu_v4_transient_buttons &= (uint8_t)~bit;
            makcu_v4_submit_mouse_report(makcu_v4_software_buttons(),
                                         0, 0, 0, 0, 0U);
            --job->clicks_left;
            if (job->clicks_left == 0U) {
                s_makcu_v4_job_read = (uint8_t)(
                    (s_makcu_v4_job_read + 1U) % MAKCU_V4_JOB_CAPACITY);
                --s_makcu_v4_job_count;
            } else {
                job->click_phase = 0U;
                job->due_ms = now_ms + 1U;
            }
        }
        return;
    }
    if (job->kind == MAKCU_V4_JOB_SILENT) {
        if (job->click_phase == 0U) {
            s_makcu_v4_transient_buttons |= 0x01U;
            makcu_v4_submit_mouse_report(makcu_v4_software_buttons(),
                (int16_t)job->remaining_x, (int16_t)job->remaining_y, 0, 0, 0U);
            dual_uart0_control_v4_track_physical_move(
                job->remaining_x, job->remaining_y);
            makcu_v4_sync_pointer(&s_makcu_v4_state);
            job->remaining_x = -job->remaining_x;
            job->remaining_y = -job->remaining_y;
            job->steps_left = makcu_v4_motion_steps(
                job->remaining_x, job->remaining_y, 0, 1U);
            job->click_phase = 1U;
            job->due_ms = now_ms + 1U;
        } else {
            s_makcu_v4_transient_buttons &= (uint8_t)~0x01U;
            const uint32_t steps = job->steps_left == 0U
                ? 1U : job->steps_left;
            const int32_t x = makcu_v4_motion_step(job->remaining_x, steps);
            const int32_t y = makcu_v4_motion_step(job->remaining_y, steps);
            makcu_v4_submit_mouse_report(makcu_v4_software_buttons(),
                (int16_t)x, (int16_t)y, 0, 0, 0U);
            dual_uart0_control_v4_track_physical_move(x, y);
            makcu_v4_sync_pointer(&s_makcu_v4_state);
            job->remaining_x -= x;
            job->remaining_y -= y;
            if (job->steps_left != 0U) --job->steps_left;
            if (job->remaining_x == 0 && job->remaining_y == 0) {
                s_makcu_v4_job_read = (uint8_t)(
                    (s_makcu_v4_job_read + 1U) % MAKCU_V4_JOB_CAPACITY);
                --s_makcu_v4_job_count;
            } else {
                job->due_ms = now_ms + 1U;
            }
        }
    }
}

static void makcu_v4_cancel_all_input(void)
{
    const bool had_injected = s_makcu_v4_state.injected_buttons != 0U ||
        s_makcu_v4_transient_buttons != 0U || s_makcu_v4_job_count != 0U;
    memset(s_makcu_v4_jobs, 0, sizeof(s_makcu_v4_jobs));
    s_makcu_v4_job_read = 0U;
    s_makcu_v4_job_write = 0U;
    s_makcu_v4_job_count = 0U;
    s_makcu_v4_transient_buttons = 0U;
    makcu_v4_state_release(&s_makcu_v4_state);
    if (had_injected && s_release_callback != NULL) {
        s_release_callback();
    }
}

static void makcu_v4_runtime_init(void)
{
    makcu_v4_state_init(&s_makcu_v4_state);
    memset(s_makcu_v4_jobs, 0, sizeof(s_makcu_v4_jobs));
    s_makcu_v4_job_read = 0U;
    s_makcu_v4_job_write = 0U;
    s_makcu_v4_job_count = 0U;
    s_makcu_v4_transient_buttons = 0U;
    s_makcu_v4_sequence = 0U;
    s_makcu_v4_baud = CONTROL_BAUD_M;
    s_makcu_v4_last_move_valid = false;
    s_makcu_v4_last_move_ms = 0U;
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    s_makcu_v4_button_mask = 0U;
    s_makcu_v4_move_mask = 0U;
    s_makcu_v4_wheel_mask = 0U;
    s_makcu_v4_binary_stream = false;
    s_makcu_v4_ascii_buttons_mode = 0U;
    s_makcu_v4_ascii_buttons_period_ms = 0U;
    s_makcu_v4_event_read = 0U;
    s_makcu_v4_event_write = 0U;
    s_makcu_v4_event_count = 0U;
    s_makcu_v4_event_overflow = false;
    s_makcu_v4_overflow_binary = false;
    s_makcu_v4_cancel_requested = false;
    s_makcu_v4_screen_width = 1920U;
    s_makcu_v4_screen_height = 1080U;
    s_makcu_v4_pointer_x = 960U;
    s_makcu_v4_pointer_y = 540U;
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
    makcu_v4_sync_pointer(&s_makcu_v4_state);
}
#endif

static void send_diag_frame(const dual_frame_t *request, uint8_t type,
                            const uint8_t *payload, uint8_t length)
{
    dual_frame_t response = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = type,
        .sequence = request->sequence,
        .payload_length = length,
    };
    if (length != 0U) {
        memcpy(response.payload, payload, length);
    }
    uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
    size_t serialized_length = 0U;
    if (dual_frame_serialize(&response, serialized, sizeof(serialized),
                             &serialized_length) == ESP_OK) {
        (void)dual_uart0_output_write(serialized, serialized_length);
    }
}

static void send_injection_result(const dual_frame_t *request, uint8_t status)
{
    send_diag_frame(request, DUAL_MESSAGE_DIAG_PROFILE_RESULT, &status, 1U);
}

static void send_stats_response(const dual_frame_t *request)
{
    dual_stats_snapshot_t snapshot;
    uint8_t status = 0U;
    if (request->payload_length != 0U) {
        status = 1U; /* bad request */
    }
    if (s_role != DUAL_ROLE_PC_DEVICE && s_role != DUAL_ROLE_MOUSE_HOST) {
        status = 2U; /* role unresolved */
    }
    dual_stats_snapshot_collect(s_role, &snapshot);
    if (snapshot.overflow) {
        status = 3U; /* Snapshot capacity exceeded; do not return truncated metrics. */
    }
    uint8_t counter_pages = status == 0U
        ? (uint8_t)((snapshot.counter_count + 4U) / 5U) : 0U;
    uint8_t queue_pages = status == 0U
        ? (uint8_t)((snapshot.queue_count + 1U) / 2U) : 0U;
    if (status != 0U) {
        counter_pages = 1U;
        snapshot.counter_count = 0U;
        snapshot.queue_count = 0U;
    }
    const uint8_t kinds[] = { DUAL_STATS_PAGE_COUNTERS, DUAL_STATS_PAGE_QUEUES };
    const uint8_t page_counts[] = { counter_pages, queue_pages };
    for (size_t kind_index = 0U; kind_index < 2U; ++kind_index) {
        const uint8_t pages = page_counts[kind_index];
        for (uint8_t page = 0U; page < pages; ++page) {
            dual_frame_t response = {
                .version = DUAL_PROXY_PROTOCOL_VERSION,
                .type = DUAL_MESSAGE_STATS_SNAPSHOT_RESPONSE,
                .sequence = request->sequence,
            };
            if (status == 0U) {
                if (!dual_stats_snapshot_serialize_page(
                        &snapshot, kinds[kind_index], page, pages, status,
                        response.payload, sizeof(response.payload),
                        &response.payload_length)) {
                    return;
                }
            } else {
                response.payload[0] = DUAL_STATS_SNAPSHOT_SCHEMA_VERSION;
                response.payload[1] = s_role;
                response.payload[2] = kinds[kind_index];
                response.payload[3] = page;
                response.payload[4] = pages;
                response.payload[5] = status;
                response.payload[6] = 0U;
                response.payload[7] = 0U;
                const uint32_t uptime_ms = snapshot.uptime_ms;
                for (uint8_t byte = 0; byte < 4U; ++byte) {
                    response.payload[8U + byte] = (uint8_t)(uptime_ms >> (byte * 8U));
                }
                response.payload_length = DUAL_STATS_SNAPSHOT_HEADER_LENGTH;
            }
            uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
            size_t serialized_length = 0U;
            if (dual_frame_serialize(&response, serialized, sizeof(serialized),
                                     &serialized_length) != ESP_OK ||
                dual_uart0_output_write(serialized, serialized_length) != ESP_OK) {
                return;
            }
        }
    }
}

static void handle_profile_read(const dual_frame_t *request)
{
    if (request->payload_length != 4U || s_role != DUAL_ROLE_MOUSE_HOST) {
        send_injection_result(request, 1U);
        return;
    }
    uint32_t offset = 0U;
    memcpy(&offset, request->payload, sizeof(offset));
    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD] = {0};
    uint32_t total = 0U;
    const size_t copied = dual_hid_host_copy_profile_blob(
        offset, &payload[8], DUAL_PROXY_MAX_PAYLOAD - 8U, &total);
    memcpy(&payload[0], &offset, sizeof(offset));
    memcpy(&payload[4], &total, sizeof(total));
    send_diag_frame(request, DUAL_MESSAGE_DIAG_PROFILE_DATA, payload,
                    (uint8_t)(8U + copied));
}

static void handle_profile_injection(const dual_frame_t *request)
{
    if (s_role != DUAL_ROLE_PC_DEVICE) {
        s_injection_active = false;
        send_injection_result(request, 2U);
        return;
    }
    if (request->type == DUAL_MESSAGE_DIAG_PROFILE_BEGIN) {
        s_injection_active = false;
        if (request->payload_length != 8U) {
            send_injection_result(request, 1U);
            return;
        }
        memcpy(&s_injected_length, &request->payload[0], 4U);
        memcpy(&s_injected_crc32, &request->payload[4], 4U);
        if (s_injected_length == 0U || s_injected_length > HID_PROFILE_MAX_BLOB) {
            send_injection_result(request, 1U);
            return;
        }
        s_injected_received = 0U;
        s_injected_started = xTaskGetTickCount();
        s_injection_active = true;
        send_injection_result(request, 0U);
        return;
    }
    if (!s_injection_active ||
        xTaskGetTickCount() - s_injected_started > pdMS_TO_TICKS(10000)) {
        s_injection_active = false;
        send_injection_result(request, 3U);
        return;
    }
    if (request->type == DUAL_MESSAGE_DIAG_PROFILE_CHUNK) {
        uint32_t offset = 0U;
        if (request->payload_length < 5U) {
            send_injection_result(request, 1U);
            return;
        }
        memcpy(&offset, request->payload, 4U);
        const uint32_t count = request->payload_length - 4U;
        if (offset != s_injected_received || count > s_injected_length - s_injected_received) {
            s_injection_active = false;
            send_injection_result(request, 1U);
            return;
        }
        memcpy(&s_injected_blob[offset], &request->payload[4], count);
        s_injected_received += count;
        send_injection_result(request, 0U);
        return;
    }
    s_injection_active = false;
    if (request->payload_length != 0U || s_injected_received != s_injected_length ||
        hid_profile_crc32(s_injected_blob, s_injected_length) != s_injected_crc32 ||
        !hid_device_profile_deserialize(&s_injected_profile, s_injected_blob,
                                        s_injected_length)) {
        send_injection_result(request, 4U);
        return;
    }
    const bool was_manual = dual_proxy_manual_profile_enabled();
    dual_proxy_set_manual_profile(true);
    const esp_err_t result = dual_pc_hid_schedule_reconfigure(
        &s_injected_profile, 0U, s_injected_crc32);
    if (result != ESP_OK && !was_manual) {
        dual_proxy_set_manual_profile(false);
    }
    send_injection_result(request, result == ESP_OK ? 0U : 5U);
    ESP_LOGW(TAG, "离线Profile注入：length=%" PRIu32 " crc=%08" PRIX32 " result=%s",
             s_injected_length, s_injected_crc32, esp_err_to_name(result));
}

static void handle_diagnostic_injection(const dual_frame_t *request)
{
    /* route=1: P 的 UART1 接收侧；route=2: M 的物理鼠标控制侧。 */
    uint8_t reply[2] = {0U, 1U};
    if (request->payload_length < 3U) {
        send_diag_frame(request, DUAL_MESSAGE_DIAG_INJECT_RESULT, reply, sizeof(reply));
        return;
    }
    const uint8_t route = request->payload[0];
    const uint8_t type = request->payload[1];
    reply[0] = route;
    dual_frame_t injected = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = type,
        .sequence = request->sequence,
        .payload_length = (uint8_t)(request->payload_length - 2U),
    };
    memcpy(injected.payload, &request->payload[2], injected.payload_length);
    uint16_t transaction_id = 0U;
    uint8_t status = 0U;
    uint8_t interface_number = 0U;
    uint8_t report_id = 0U;
    uint8_t report_type = 0U;
    uint8_t requested_length = 0U;
    /* 设备级 Vendor 控制请求用的字段（2026-09-27）。 */
    uint8_t bm_request_type = 0U;
    uint8_t b_request = 0U;
    uint16_t w_value = 0U;
    uint16_t w_index = 0U;
    uint16_t w_length = 0U;
    const uint8_t *data = NULL;
    size_t data_length = 0U;
    bool valid = false;
    if (type == DUAL_MESSAGE_RAW_HID_INPUT) {
        valid = dual_hid_raw_input_decode(injected.payload, injected.payload_length,
                                          &interface_number, &report_id, &data, &data_length);
    } else if (type == DUAL_MESSAGE_HID_GET_REPORT_RESPONSE) {
        valid = dual_hid_get_response_decode(injected.payload, injected.payload_length,
                                              &transaction_id, &status, &interface_number,
                                              &report_id, &data, &data_length);
    } else if (type == DUAL_MESSAGE_HID_SET_REPORT) {
        valid = dual_hid_set_report_decode(injected.payload, injected.payload_length,
                                            &transaction_id, &interface_number, &report_id,
                                            &report_type, &data, &data_length);
    } else if (type == DUAL_MESSAGE_HID_GET_REPORT_REQUEST) {
        valid = dual_hid_get_request_decode(injected.payload, injected.payload_length,
                                             &transaction_id, &interface_number, &report_id,
                                             &report_type, &requested_length);
    } else if (type == DUAL_MESSAGE_VENDOR_CONTROL_REQUEST) {
        /* 2026-09-27：设备级 Vendor 控制请求也走注入通道（验证 M 侧通用 EP0 转发）。 */
        valid = dual_vendor_control_request_decode(
            injected.payload, injected.payload_length, &transaction_id, &bm_request_type,
            &b_request, &w_value, &w_index, &w_length, &data, &data_length);
    }
    if (!valid) {
        reply[1] = 1U;
        send_diag_frame(request, DUAL_MESSAGE_DIAG_INJECT_RESULT, reply, sizeof(reply));
        return;
    }
    if (route == 1U && s_role == DUAL_ROLE_PC_DEVICE &&
        (type == DUAL_MESSAGE_RAW_HID_INPUT ||
         type == DUAL_MESSAGE_HID_GET_REPORT_RESPONSE)) {
        dual_pc_hid_handle_vendor_frame(&injected);
        reply[1] = 0U;
    } else if (route == 2U && s_role == DUAL_ROLE_MOUSE_HOST &&
               (type == DUAL_MESSAGE_HID_SET_REPORT ||
                type == DUAL_MESSAGE_HID_GET_REPORT_REQUEST ||
                /* 2026-09-27：注入通道也允许投递设备级 Vendor 控制请求——
                 * Windows 用户态对 HID 类设备发不了 vendor 请求，没有主机侧触发
                 * 手段时，用这条注入路径单独验证 M 侧的通用 EP0 转发链路。 */
                type == DUAL_MESSAGE_VENDOR_CONTROL_REQUEST)) {
        dual_hid_host_handle_control_frame(&injected);
        reply[1] = 0U;
    } else {
        reply[1] = 2U;
    }
    send_diag_frame(request, DUAL_MESSAGE_DIAG_INJECT_RESULT, reply, sizeof(reply));
}

static void send_device_hello(const dual_frame_t *probe)
{
    static const uint8_t signature[] = {'H', 'I', 'D', 'B', 'R', 'D', 'G', '2'};
    if (probe->payload_length != 8) {
        return;
    }
    dual_frame_t hello = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_DEVICE_HELLO,
        .sequence = probe->sequence,
        .payload_length = sizeof(signature) + 8 + 1,
    };
    memcpy(hello.payload, signature, sizeof(signature));
    memcpy(&hello.payload[sizeof(signature)], probe->payload, 8);
    hello.payload[sizeof(signature) + 8] = s_role;
    uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
    size_t serialized_length = 0;
    if (dual_frame_serialize(&hello, serialized, sizeof(serialized), &serialized_length) != ESP_OK) {
        return;
    }
    (void)dual_uart0_output_write(serialized, serialized_length);
}

static bool accept_input_frame(const dual_frame_t *frame)
{
    ++s_state.received;
    if (frame->type == DUAL_MESSAGE_SESSION_START) {
        legacy_motion_reset();
        s_state.active = true;
        s_state.sequence_initialized = true;
        s_state.expected_sequence = (uint16_t)(frame->sequence + 1U);
        s_state.last_activity = xTaskGetTickCount();
        if (s_release_callback != NULL) {
            s_release_callback();
        }
        ++s_state.accepted;
        ESP_LOGI(TAG, "主机软件输入会话已建立");
        return false;
    }
    if (!s_state.active && frame->type == DUAL_MESSAGE_PING) {
        s_state.active = true;
        s_state.sequence_initialized = true;
        s_state.expected_sequence = (uint16_t)(frame->sequence + 1U);
        s_state.last_activity = xTaskGetTickCount();
        ++s_state.accepted;
        return false;
    }
    if (!s_state.active) {
        ++s_state.rejected;
        return false;
    }
    s_state.last_activity = xTaskGetTickCount();
    if (s_state.sequence_initialized && frame->sequence != s_state.expected_sequence) {
        ++s_state.discontinuities;
        ESP_LOGW(TAG, "主机帧序号不连续：期望=%u 实际=%u", s_state.expected_sequence, frame->sequence);
    }
    s_state.expected_sequence = (uint16_t)(frame->sequence + 1U);
    s_state.sequence_initialized = true;
    ++s_state.accepted;
    return true;
}

static void on_control_frame(const dual_frame_t *frame, void *context)
{
    (void)context;
    dual_diag_stream_record(DUAL_DIAG_SOURCE_UART0_RX, frame->type,
                            frame->payload, frame->payload_length);
    if (frame->type == DUAL_MESSAGE_DEVICE_PROBE) {
        send_device_hello(frame);
        return;
    }
    if (frame->type == DUAL_MESSAGE_STATS_SNAPSHOT_REQUEST) {
        ++s_state.accepted;
        send_stats_response(frame);
        return;
    }
    if (frame->type == DUAL_MESSAGE_DIAG_PROFILE_READ) {
        ++s_state.accepted;
        handle_profile_read(frame);
        return;
    }
    if (s_role != DUAL_ROLE_MOUSE_HOST &&
        (frame->type == DUAL_MESSAGE_DIAG_VENDOR_TEST_REQUEST ||
         frame->type == DUAL_MESSAGE_DIAG_REPORT_INJECT_REQUEST ||
         frame->type == DUAL_MESSAGE_DIAG_PROFILE_REFRESH_REQUEST)) {
        ++s_state.rejected;
        if (frame->type == DUAL_MESSAGE_DIAG_VENDOR_TEST_REQUEST) {
            send_injection_result(frame, 1U);
        }
        return;
    }
    if (frame->type == DUAL_MESSAGE_DIAG_PROFILE_BEGIN ||
        frame->type == DUAL_MESSAGE_DIAG_PROFILE_CHUNK ||
        frame->type == DUAL_MESSAGE_DIAG_PROFILE_COMMIT) {
        ++s_state.accepted;
        handle_profile_injection(frame);
        return;
    }
    if (frame->type == DUAL_MESSAGE_DIAG_PROFILE_MODE) {
        if (frame->payload_length != 1U || frame->payload[0] != 0U ||
            s_role != DUAL_ROLE_PC_DEVICE) {
            send_injection_result(frame, 1U);
        } else {
            dual_proxy_set_manual_profile(false);
            send_injection_result(frame, 0U);
        }
        return;
    }
    if (frame->type == DUAL_MESSAGE_DIAG_INJECT_REQUEST) {
        handle_diagnostic_injection(frame);
        return;
    }
    if (frame->type == DUAL_MESSAGE_DIAG_REPORT_INJECT_REQUEST) {
        /* 物理报告注入（2026-09-27）：payload 即原始报告字节，投进 M 的 RX 回调，
         * 使位移统计/入队/转发与真实鼠标报告完全同路。 */
        const bool ok = frame->payload_length != 0U &&
            dual_hid_host_inject_report(frame->payload, frame->payload_length);
        ESP_LOGI(TAG, "报告注入：%u 字节 → %s",
                 (unsigned)frame->payload_length,
                 ok ? "已投入物理RX路径" : "失败（无活动鼠标接口或长度非法）");
        return;
    }
    if (frame->type == DUAL_MESSAGE_DIAG_PROFILE_REFRESH_REQUEST) {
        /*
         * 强制重新采集并重新提议（2026-09-28）：用于按需复现恢复流程里的
         * "M 重新提议 → P 复用/重装" 一段。只在鼠标侧板有意义。
         */
        const bool present = dual_hid_host_mouse_present();
        const esp_err_t refresh = (s_role == DUAL_ROLE_MOUSE_HOST && present)
            ? dual_hid_host_request_profile_refresh() : ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "强制Profile重采集：role=%u mouse_present=%u → %s",
                 (unsigned)s_role, present ? 1U : 0U, esp_err_to_name(refresh));
        send_injection_result(frame, refresh == ESP_OK ? 0U : 1U);
        return;
    }
    if (frame->type == DUAL_MESSAGE_DIAG_STREAM_CONTROL) {
        if (frame->payload_length == 1U && frame->payload[0] <= 1U) {
            dual_diag_stream_set_enabled(frame->payload[0] != 0U);
        }
        dual_diag_stream_send_status(frame->sequence);
        return;
    }
    if (frame->type == DUAL_MESSAGE_SESSION_START ||
        frame->type == DUAL_MESSAGE_PING ||
        frame->type == DUAL_MESSAGE_MOUSE_REPORT ||
        frame->type == DUAL_MESSAGE_RELEASE_ALL) {
        if (s_role != DUAL_ROLE_MOUSE_HOST) {
            ++s_state.rejected;
            return;
        }
        if (!accept_input_frame(frame)) {
            return;
        }
        if (frame->type == DUAL_MESSAGE_RELEASE_ALL) {
            legacy_motion_reset();
            if (s_release_callback != NULL) {
                s_release_callback();
            }
            return;
        }
        if (frame->type == DUAL_MESSAGE_MOUSE_REPORT) {
            if ((frame->payload_length != 7 && frame->payload_length != 8) ||
                (frame->payload_length == 8 &&
                 !mouse_motion_smoother_valid_slot_count(frame->payload[7]))) {
                ++s_state.rejected;
                ESP_LOGW(TAG, "拒绝长度或平滑槽非法的 MouseReport");
                return;
            }
            ++s_state.mouse_reports;
            legacy_motion_enqueue(frame);
        }
        return;
    }
    ++s_state.rejected;
}

#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API
static void makcu_write_text(const char *text)
{
    if (text == NULL) {
        return;
    }
    const size_t length = strlen(text);
    (void)dual_uart0_output_write_makcu_ascii(text, length);
}

static void makcu_send_prompt(void)
{
    makcu_write_text("\r\n>>> ");
}

static void makcu_send_ack(const makcu_ascii_command_t *command)
{
    if (s_makcu_session.echo_enabled) {
        char response[MAKCU_ASCII_COMMAND_MAX + 16U];
        const int length = snprintf(response, sizeof(response), "km.%s\r\n>>> ",
                                    command->body);
        if (length > 0 && (size_t)length < sizeof(response)) {
            makcu_write_text(response);
        }
    } else {
        makcu_send_prompt();
    }
}

static void makcu_send_query(
    const makcu_ascii_command_t *command,
    const char *value)
{
    char response[MAKCU_ASCII_COMMAND_MAX * 2U + 48U];
    const int length = s_makcu_session.echo_enabled
        ? snprintf(response, sizeof(response), "km.%s\r\nkm.%s\r\n>>> ",
                   command->body, value)
        : snprintf(response, sizeof(response), "km.%s\r\n>>> ", value);
    if (length > 0 && (size_t)length < sizeof(response)) {
        makcu_write_text(response);
    }
}

static void makcu_send_error(makcu_ascii_error_t error)
{
    const char *name = "syntax";
    switch (error) {
        case MAKCU_ASCII_ERROR_ARGUMENT:
            name = "argument";
            break;
        case MAKCU_ASCII_ERROR_UNSUPPORTED:
            name = "unsupported";
            break;
        case MAKCU_ASCII_ERROR_TOO_LONG:
            name = "too_long";
            break;
        case MAKCU_ASCII_ERROR_SYNTAX:
        case MAKCU_ASCII_ERROR_NONE:
        default:
            name = "syntax";
            break;
    }
    char response[48];
    const int length = snprintf(response, sizeof(response),
                                "km.error(%s)\r\n>>> ", name);
    if (length > 0 && (size_t)length < sizeof(response)) {
        makcu_write_text(response);
    }
}

static void makcu_submit_mouse_report(int16_t x, int16_t y, int8_t wheel)
{
    dual_frame_t frame = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_MOUSE_REPORT,
        .sequence = ++s_makcu_sequence,
        .payload_length = 8U,
    };
    const uint16_t encoded_x = (uint16_t)x;
    const uint16_t encoded_y = (uint16_t)y;
    frame.payload[0] = s_makcu_session.injected_buttons;
    frame.payload[1] = (uint8_t)encoded_x;
    frame.payload[2] = (uint8_t)(encoded_x >> 8U);
    frame.payload[3] = (uint8_t)encoded_y;
    frame.payload[4] = (uint8_t)(encoded_y >> 8U);
    frame.payload[5] = (uint8_t)wheel;
    frame.payload[6] = 0U;
    frame.payload[7] = 0U;
    if (s_report_callback != NULL) {
        s_report_callback(&frame);
    }
}

static void on_makcu_ascii_command(
    const makcu_ascii_command_t *command,
    void *context)
{
    (void)context;
    if (command == NULL) {
        return;
    }
    if (command->kind == MAKCU_ASCII_COMMAND_ERROR) {
        makcu_send_error(command->error);
        return;
    }

    const uint32_t now_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    switch (command->kind) {
        case MAKCU_ASCII_COMMAND_MOVE:
            makcu_ascii_session_touch(&s_makcu_session, now_ms);
            if (command->argument[0] != 0 || command->argument[1] != 0) {
                makcu_submit_mouse_report((int16_t)command->argument[0],
                                          (int16_t)command->argument[1], 0);
            }
            makcu_send_ack(command);
            return;
        case MAKCU_ASCII_COMMAND_WHEEL: {
            makcu_ascii_session_touch(&s_makcu_session, now_ms);
            const int8_t wheel = command->argument[0] > 0 ? 1 :
                (command->argument[0] < 0 ? -1 : 0);
            if (wheel != 0) {
                makcu_submit_mouse_report(0, 0, wheel);
            }
            makcu_send_ack(command);
            return;
        }
        case MAKCU_ASCII_COMMAND_BUTTON:
            if (command->query) {
                const uint8_t physical = s_physical_buttons_callback != NULL
                    ? s_physical_buttons_callback() : 0U;
                char value[4];
                (void)snprintf(value, sizeof(value), "%u",
                    (unsigned)makcu_ascii_button_state(
                        command->button_index, physical,
                        s_makcu_session.injected_buttons));
                makcu_send_query(command, value);
            } else {
                bool send_report = false;
                if (!makcu_ascii_session_set_button(
                        &s_makcu_session, command->button_index,
                        command->argument[0], &send_report)) {
                    makcu_send_error(MAKCU_ASCII_ERROR_ARGUMENT);
                    return;
                }
                makcu_ascii_session_touch(&s_makcu_session, now_ms);
                if (send_report) {
                    makcu_submit_mouse_report(0, 0, 0);
                }
                makcu_send_ack(command);
            }
            return;
        case MAKCU_ASCII_COMMAND_ECHO:
            if (command->query) {
                makcu_send_query(command,
                    s_makcu_session.echo_enabled ? "1" : "0");
            } else {
                s_makcu_session.echo_enabled = command->argument[0] != 0;
                makcu_send_ack(command);
            }
            return;
        case MAKCU_ASCII_COMMAND_VERSION:
            makcu_send_query(command, "ESP32-S3-HID-Bridge-MakcuASCII-1");
            return;
        case MAKCU_ASCII_COMMAND_DEVICE:
            makcu_send_query(command, "mouse");
            return;
        case MAKCU_ASCII_COMMAND_RELEASE:
            makcu_ascii_session_expire(&s_makcu_session);
            if (s_release_callback != NULL) {
                s_release_callback();
            }
            makcu_send_ack(command);
            return;
        case MAKCU_ASCII_COMMAND_ERROR:
        default:
            makcu_send_error(MAKCU_ASCII_ERROR_UNSUPPORTED);
            return;
    }
}
#endif

#if DUAL_PROXY_ENABLE_MAKCU_V4_API
static void select_input_owner(bool v4)
{
    if (s_v4_owner == v4) return;
    makcu_v4_cancel_all_input();
    legacy_motion_reset();
    s_state.active = false;
    s_state.sequence_initialized = false;
    portENTER_CRITICAL(&s_makcu_v4_input_mux);
    s_makcu_v4_event_read = s_makcu_v4_event_write = s_makcu_v4_event_count = 0U;
    portEXIT_CRITICAL(&s_makcu_v4_input_mux);
    if (s_release_callback != NULL) s_release_callback();
    s_v4_owner = v4;
    dual_diag_stream_set_enabled(false);
    dual_uart0_output_set_makcu_mode(v4);
}

static void routed_a5(const dual_frame_t *frame, void *context)
{
    if (frame->type == DUAL_MESSAGE_MOUSE_REPORT &&
        ((frame->payload_length != 7U && frame->payload_length != 8U) ||
         (frame->payload_length == 8U &&
          !mouse_motion_smoother_valid_slot_count(frame->payload[7])))) return;
    /* 完整 CRC 帧握手即选择 A5；未知消息不能抢走输入所有权。 */
    if (frame->type < DUAL_MESSAGE_KEYBOARD_REPORT || frame->type > 0x1DU) return;
    select_input_owner(false);
    on_control_frame(frame, context);
}

static void routed_v4(const makcu_v4_command_t *command, void *context)
{
    if (command->error == MAKCU_V4_ERROR_NONE) select_input_owner(true);
    if (s_v4_owner) makcu_v4_handle_command(command, context);
}
#endif

static void control_task(void *argument)
{
    (void)argument;
    dual_parser_t parser;
    dual_parser_init(&parser, on_control_frame, NULL);
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
    uart0_protocol_router_t router;
    uart0_protocol_router_init(&router, routed_a5, routed_v4, NULL);
#endif
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API
    makcu_ascii_parser_t makcu_parser;
    makcu_ascii_parser_init(&makcu_parser);
    makcu_ascii_session_init(&s_makcu_session);
#endif
#if DUAL_PROXY_ENABLE_MAKCU_V4_API || DUAL_PROXY_ENABLE_MAKCU_ASCII_API
    bool makcu_mode_active = false;
#endif
    TickType_t last_statistics = xTaskGetTickCount();
    while (true) {
        if (__atomic_exchange_n(&s_legacy_cancel_requested, false, __ATOMIC_ACQ_REL)) {
            legacy_motion_reset();
            s_state.active = false;
            s_state.sequence_initialized = false;
            if (s_release_callback != NULL) s_release_callback();
        }
        uint8_t buffer[128];
        /* 有限超时既允许1 ms任务继续驱动插值/点击，也允许角色切换生效。 */
        TickType_t read_timeout = pdMS_TO_TICKS(CONTROL_POLL_MS);
        int received = uart_read_bytes(CONTROL_UART, buffer, 1, read_timeout);
        if (received > 0) {
            size_t buffered = 0;
            if (uart_get_buffered_data_len(CONTROL_UART, &buffered) == ESP_OK && buffered > 0) {
                const size_t remaining = sizeof(buffer) - (size_t)received;
                const size_t drain_length = buffered < remaining ? buffered : remaining;
                const int drained = uart_read_bytes(
                    CONTROL_UART,
                    buffer + received,
                    drain_length,
                    0);
                if (drained > 0) {
                    received += drained;
                }
            }
        }
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
        const bool requested_v4_mode = __atomic_load_n(
            &s_makcu_v4_requested, __ATOMIC_ACQUIRE);
        if (requested_v4_mode != makcu_mode_active) {
            makcu_mode_active = requested_v4_mode;
            dual_parser_init(&parser, on_control_frame, NULL);
            uart0_protocol_router_init(&router, routed_a5, routed_v4, NULL);
            s_v4_owner = makcu_mode_active;
            if (makcu_mode_active) {
                makcu_v4_runtime_init();
                s_state.active = false;
                s_state.sequence_initialized = false;
                if (s_release_callback != NULL) {
                    s_release_callback();
                }
            } else {
                makcu_v4_cancel_all_input();
            }
        }
#endif
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API
        const bool requested_makcu_mode = __atomic_load_n(
            &s_makcu_ascii_requested, __ATOMIC_ACQUIRE);
        if (requested_makcu_mode != makcu_mode_active) {
            makcu_mode_active = requested_makcu_mode;
            dual_parser_init(&parser, on_control_frame, NULL);
            makcu_ascii_parser_init(&makcu_parser);
            if (makcu_mode_active) {
                makcu_ascii_session_init(&s_makcu_session);
                s_state.active = false;
                s_state.sequence_initialized = false;
                if (s_release_callback != NULL) {
                    s_release_callback();
                }
            }
        }
#endif
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
        if (makcu_mode_active) {
            const uint32_t now_ms = makcu_v4_now_ms();
            uart0_protocol_router_tick(&router, now_ms);
            if (makcu_v4_take_cancel_request()) {
                makcu_v4_cancel_all_input();
                legacy_motion_reset();
                if (s_release_callback != NULL) s_release_callback();
            }
            if (s_v4_owner && makcu_v4_state_lease_expired(&s_makcu_v4_state, now_ms)) {
                makcu_v4_cancel_all_input();
            }
            if (received > 0) uart0_protocol_router_feed(
                &router, buffer, (size_t)received, now_ms);
            if (s_v4_owner) {
                makcu_v4_advance_jobs(now_ms);
                makcu_v4_emit_events();
            } else {
                legacy_motion_tick();
            }
        } else if (received > 0) {
            dual_parser_feed(&parser, buffer, (size_t)received);
        }
#elif DUAL_PROXY_ENABLE_MAKCU_ASCII_API
        if (received > 0) {
            if (makcu_mode_active) {
                makcu_ascii_parser_feed(&makcu_parser, buffer,
                                        (size_t)received,
                                        on_makcu_ascii_command, NULL);
            } else {
                dual_parser_feed(&parser, buffer, (size_t)received);
            }
        }
        if (makcu_mode_active && makcu_ascii_session_lease_expired(
                &s_makcu_session,
                (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS)) {
            makcu_ascii_session_expire(&s_makcu_session);
            if (s_release_callback != NULL) {
                s_release_callback();
            }
        }
#else
        if (received > 0) {
            dual_parser_feed(&parser, buffer, (size_t)received);
        }
#endif
#if DUAL_PROXY_ENABLE_MAKCU_V4_API || DUAL_PROXY_ENABLE_MAKCU_ASCII_API
        if (s_role == DUAL_ROLE_MOUSE_HOST && !makcu_mode_active) legacy_motion_tick();
#else
        if (s_role == DUAL_ROLE_MOUSE_HOST) legacy_motion_tick();
#endif
        bool allow_legacy_lease = true;
#if DUAL_PROXY_ENABLE_MAKCU_V4_API || DUAL_PROXY_ENABLE_MAKCU_ASCII_API
        allow_legacy_lease = !makcu_mode_active || !s_v4_owner;
#endif
        if (allow_legacy_lease &&
            s_state.active && xTaskGetTickCount() - s_state.last_activity > pdMS_TO_TICKS(CONTROL_LEASE_MS)) {
            legacy_motion_reset();
            s_state.active = false;
            s_state.sequence_initialized = false;
            ESP_LOGW(TAG, "主机软件输入租约超时，释放软件输入");
            if (s_release_callback != NULL) {
                s_release_callback();
            }
        }
        if (xTaskGetTickCount() - last_statistics >= pdMS_TO_TICKS(1000)) {
            const uint64_t mouse_reports_window =
                s_state.mouse_reports - s_state.last_mouse_reports;
            /*
             * 本周期没有鼠标报告就不打印（2026-09-28）：空闲时这条统计每秒刷一行没有信息量。
             * 注意计数器与时间戳仍要无条件推进，否则窗口增量会越算越错。
             */
            if (mouse_reports_window > 0U) {
                ESP_LOGI(TAG, "UART0协议统计：接收=%" PRIu64 " MouseReport=%" PRIu64
                         " MouseReportHz=%" PRIu64
                         " 接受=%" PRIu64 " 拒绝=%" PRIu64 " 序号不连续=%" PRIu64,
                         s_state.received, s_state.mouse_reports, mouse_reports_window,
                         s_state.accepted, s_state.rejected, s_state.discontinuities);
            }
            s_state.last_mouse_reports = s_state.mouse_reports;
            last_statistics = xTaskGetTickCount();
        }
    }
}

esp_err_t dual_uart0_control_start(
    uint8_t role,
    dual_software_report_callback_t report_callback,
    dual_software_release_callback_t release_callback,
    dual_software_physical_buttons_callback_t physical_buttons_callback)
{
    if (role != DUAL_ROLE_UNRESOLVED && role != DUAL_ROLE_PC_DEVICE &&
        role != DUAL_ROLE_MOUSE_HOST) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_control_started) {
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
        const uint8_t previous_role = s_role;
#endif
        s_role = role;
        s_report_callback = report_callback;
        s_release_callback = release_callback;
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
        const bool enable_makcu_v4 = role == DUAL_ROLE_MOUSE_HOST;
        __atomic_store_n(&s_makcu_v4_requested, enable_makcu_v4,
                         __ATOMIC_RELEASE);
        dual_uart0_output_set_makcu_mode(enable_makcu_v4);
        if (enable_makcu_v4) {
            dual_diag_stream_set_enabled(false);
        }
        if (previous_role != role) {
            const uint32_t role_baud = enable_makcu_v4
                ? CONTROL_BAUD_M : CONTROL_BAUD_P;
            if (uart_set_baudrate(CONTROL_UART, (int)role_baud) != ESP_OK) {
                return ESP_FAIL;
            }
            s_makcu_v4_baud = role_baud;
        }
#endif
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API
        s_physical_buttons_callback = physical_buttons_callback;
        const bool enable_makcu_ascii = role == DUAL_ROLE_MOUSE_HOST;
        __atomic_store_n(&s_makcu_ascii_requested, enable_makcu_ascii,
                         __ATOMIC_RELEASE);
        dual_uart0_output_set_makcu_ascii_mode(enable_makcu_ascii);
        if (enable_makcu_ascii) {
            dual_diag_stream_set_enabled(false);
        }
#else
        (void)physical_buttons_callback;
#endif
        return ESP_OK;
    }
    esp_err_t result = dual_uart0_output_init();
    if (result != ESP_OK) {
        return result;
    }
    s_role = role;
    s_report_callback = report_callback;
    s_release_callback = release_callback;
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API
    s_physical_buttons_callback = physical_buttons_callback;
    const bool enable_makcu_ascii = role == DUAL_ROLE_MOUSE_HOST;
    __atomic_store_n(&s_makcu_ascii_requested, enable_makcu_ascii,
                     __ATOMIC_RELEASE);
    dual_uart0_output_set_makcu_ascii_mode(enable_makcu_ascii);
#else
    (void)physical_buttons_callback;
#endif
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
    const bool enable_makcu_v4 = role == DUAL_ROLE_MOUSE_HOST;
    __atomic_store_n(&s_makcu_v4_requested, enable_makcu_v4,
                     __ATOMIC_RELEASE);
    dual_uart0_output_set_makcu_mode(enable_makcu_v4);
#endif
    const uart_config_t config = {
        .baud_rate = role == DUAL_ROLE_MOUSE_HOST
            ? CONTROL_BAUD_M : CONTROL_BAUD_P,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    result = uart_param_config(CONTROL_UART, &config);
    if (result != ESP_OK) {
        return result;
    }
    result = uart_set_pin(CONTROL_UART, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (result != ESP_OK) {
        return result;
    }
    result = uart_driver_install(CONTROL_UART, CONTROL_RX_BUFFER_SIZE, CONTROL_TX_BUFFER_SIZE, 0, NULL, 0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    dual_uart0_output_set_driver_ready(true);
    result = dual_diag_stream_start(CONTROL_UART);
    if (result != ESP_OK) {
        return result;
    }
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
    s_makcu_v4_baud = role == DUAL_ROLE_MOUSE_HOST
        ? CONTROL_BAUD_M : CONTROL_BAUD_P;
    if (enable_makcu_v4) {
        dual_diag_stream_set_enabled(false);
    }
#elif DUAL_PROXY_ENABLE_MAKCU_ASCII_API
    if (enable_makcu_ascii) {
        dual_diag_stream_set_enabled(false);
    }
#endif
    if (xTaskCreate(control_task, "dual_uart0", CONTROL_TASK_STACK_SIZE,
                    NULL, 7, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_control_started = true;
    return ESP_OK;
}
