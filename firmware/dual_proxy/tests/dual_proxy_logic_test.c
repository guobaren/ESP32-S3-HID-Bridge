#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bridge_protocol.h"
#include "dual_input_aggregator.h"
#include "dual_proxy_runtime_config.h"
#include "dual_status_led_logic.h"
#include "usb_cdc_control_logic.h"

_Static_assert(DUAL_PROXY_REQUIRED_FREERTOS_HZ == 1000U, "FreeRTOS tick必须保持1000Hz");
_Static_assert(DUAL_PROXY_LINK_TX_BATCH_LIMIT > 0U, "UART1批量上限回归保护失败");
_Static_assert(DUAL_PROXY_HID_PERIOD_US == 1000U, "HID周期必须保持1000us");

static void test_merge_and_independent_release(void)
{
    dual_input_state_t state;
    dual_mouse_report_t report;
    dual_input_init(&state);

    dual_input_physical_report(&state, 1, 3, -2, 1, 0);
    dual_input_software_report(&state, 2, 4, 5, 0, -1);
    assert(dual_input_take_report(&state, &report, false));
    assert(report.buttons == 3);
    assert(report.x == 7 && report.y == 3 && report.wheel == 1 && report.pan == -1);

    dual_input_software_release(&state);
    assert(dual_input_take_report(&state, &report, false));
    assert(report.buttons == 1);
    assert(report.x == 0 && report.y == 0 && report.wheel == 0 && report.pan == 0);

    dual_input_physical_release(&state);
    assert(dual_input_take_report(&state, &report, false));
    assert(report.buttons == 0);
}

static void test_saturation_is_split_without_loss(void)
{
    dual_input_state_t state;
    dual_mouse_report_t report;
    dual_input_init(&state);
    dual_input_physical_report(&state, 0, INT16_MAX, 0, 0, 0);
    dual_input_software_report(&state, 0, 100, 0, 0, 0);
    assert(dual_input_take_report(&state, &report, false));
    assert(report.x == INT16_MAX);
    assert(dual_input_take_report(&state, &report, false));
    assert(report.x == 100);
}

static void test_peek_commit_preserves_pending_on_retry(void)
{
    dual_input_state_t state;
    dual_mouse_report_t report;
    dual_input_init(&state);
    dual_input_software_report(&state, 0, 4, 0, 0, 0);

    /* not-ready/submit-fail paths peek but do not consume the report */
    assert(dual_input_peek_report(&state, &report, false));
    assert(report.x == 4);
    assert(state.software_x == 4);
    assert(dual_input_peek_report(&state, &report, false));
    assert(report.x == 4);
    assert(state.software_x == 4);

    /* a successful submit commits exactly once */
    dual_input_commit_report(&state, &report);
    assert(state.software_x == 0);
    assert(!dual_input_peek_report(&state, &report, false));
}

typedef struct {
    unsigned count;
    dual_frame_t frames[4];
} parser_capture_t;

static void capture_frame(const dual_frame_t *frame, void *context)
{
    parser_capture_t *capture = (parser_capture_t *)context;
    assert(capture != NULL);
    assert(frame != NULL);
    assert(capture->count < sizeof(capture->frames) / sizeof(capture->frames[0]));
    capture->frames[capture->count++] = *frame;
}

static void test_bridge_protocol(void)
{
    static const uint8_t crc_input[] = "123456789";
    assert(dual_crc16_ccitt(crc_input, sizeof(crc_input) - 1) == 0x29B1);

    const dual_frame_t source = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_MOUSE_REPORT,
        .sequence = 0x1234,
        .payload_length = 2,
        .payload = {0xAA, 0x55},
    };
    static const uint8_t expected[] = {
        0xA5, 0x5A, 0x02, 0x02, 0x34, 0x12, 0x02, 0xAA, 0x55, 0xCF, 0xFF,
    };
    uint8_t serialized[sizeof(expected)] = {0};
    size_t serialized_length = 0;
    assert(dual_frame_serialize(&source, serialized, sizeof(serialized), &serialized_length) == ESP_OK);
    assert(serialized_length == sizeof(expected));
    assert(memcmp(serialized, expected, sizeof(expected)) == 0);

    parser_capture_t capture = {0};
    dual_parser_t parser;
    dual_parser_init(&parser, capture_frame, &capture);
    const uint8_t noise[] = {0x00, 0xFF, 0xA5, 0x00, 0x7E, 0x11};
    dual_parser_feed(&parser, noise, sizeof(noise));
    for (size_t index = 0; index < serialized_length; ++index) {
        dual_parser_feed(&parser, &serialized[index], 1);
    }
    assert(capture.count == 1);
    assert(capture.frames[0].type == source.type);
    assert(capture.frames[0].sequence == source.sequence);
    assert(capture.frames[0].payload_length == source.payload_length);
    assert(memcmp(capture.frames[0].payload, source.payload, source.payload_length) == 0);

    uint8_t bad_crc[sizeof(expected)];
    memcpy(bad_crc, expected, sizeof(bad_crc));
    bad_crc[sizeof(bad_crc) - 1] ^= 0x01;
    const dual_frame_t second = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_PING,
        .sequence = 0x1235,
        .payload_length = 0,
    };
    uint8_t second_serialized[9] = {0};
    size_t second_length = 0;
    assert(dual_frame_serialize(&second, second_serialized, sizeof(second_serialized), &second_length) == ESP_OK);
    dual_parser_feed(&parser, bad_crc, sizeof(bad_crc));
    assert(capture.count == 1);
    dual_parser_feed(&parser, second_serialized, second_length);
    assert(capture.count == 2);
    assert(capture.frames[1].type == DUAL_MESSAGE_PING);
    assert(capture.frames[1].sequence == second.sequence);
}

static void test_cdc_parser_and_lease_release(void)
{
    dual_cdc_session_state_t session;
    dual_cdc_session_init(&session);
    const dual_frame_t session_start = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_SESSION_START,
        .sequence = 10,
        .payload_length = 0,
    };
    assert(dual_cdc_session_accept(&session, &session_start, 1000) ==
           DUAL_CDC_ACTION_SOFTWARE_RELEASE);
    assert(session.active);

    const dual_frame_t mouse = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_MOUSE_REPORT,
        .sequence = 11,
        .payload_length = 7,
        .payload = {0, 4, 0, 0, 0, 0, 0},
    };
    assert(dual_cdc_session_accept(&session, &mouse, 1100) ==
           DUAL_CDC_ACTION_MOUSE_REPORT);

    uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD] = {0};
    size_t serialized_length = 0;
    assert(dual_frame_serialize(&mouse, serialized, sizeof(serialized), &serialized_length) == ESP_OK);
    parser_capture_t capture = {0};
    dual_parser_t parser;
    dual_parser_init(&parser, capture_frame, &capture);
    dual_parser_feed(&parser, serialized, 3);
    dual_parser_feed(&parser, &serialized[3], serialized_length - 3);
    assert(capture.count == 1);
    assert(capture.frames[0].type == DUAL_MESSAGE_MOUSE_REPORT);
    assert(capture.frames[0].payload[1] == 4);

    dual_input_state_t input;
    dual_input_init(&input);
    dual_input_physical_report(&input, 1, 1, 0, 0, 0);
    dual_input_software_report(&input, 2, 5, 0, 0, 0);
    assert(dual_cdc_session_lease_expired(&session, 2599) == false);
    assert(dual_cdc_session_lease_expired(&session, 2601));
    dual_cdc_session_expire(&session);
    dual_input_software_release(&input);
    assert(input.physical_buttons == 1 && input.physical_x == 1);
    assert(input.software_buttons == 0 && input.software_x == 0);
}

static void test_runtime_scheduling_guards(void)
{
    assert(DUAL_PROXY_REQUIRED_FREERTOS_HZ == 1000U);
    assert((1000U / DUAL_PROXY_REQUIRED_FREERTOS_HZ) == 1U);
    const unsigned queued_items = 100;
    const unsigned processed = queued_items < DUAL_PROXY_LINK_TX_BATCH_LIMIT
        ? queued_items : DUAL_PROXY_LINK_TX_BATCH_LIMIT;
    assert(processed == DUAL_PROXY_LINK_TX_BATCH_LIMIT);
    assert(queued_items - processed > 0);

    /* A backlog bypasses the heartbeat wait; an empty queue may wait. */
    assert(dual_proxy_link_should_wait_for_notification(false));
    assert(!dual_proxy_link_should_wait_for_notification(true));
    const unsigned backlog = DUAL_PROXY_LINK_TX_BATCH_LIMIT + 1U;
    assert(!dual_proxy_link_should_wait_for_notification(backlog > 0U));
}

static void test_status_led_logic(void)
{
    dual_status_led_state_t state;
    dual_status_led_logic_init(&state);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_RED);

    dual_status_led_logic_set_role(&state, DUAL_STATUS_LED_ROLE_MOUSE_HOST);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_RED);
    dual_status_led_logic_set_host_mouse_ready(&state, true);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_GREEN);
    dual_status_led_logic_set_host_mouse_ready(&state, false);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_RED);

    dual_status_led_logic_set_role(&state, DUAL_STATUS_LED_ROLE_PC_DEVICE);
    dual_status_led_logic_set_pc_mounted(&state, true);
    assert(dual_status_led_logic_color(&state, 1000) == DUAL_STATUS_LED_COLOR_BLUE);
    dual_status_led_logic_notify_software_success(&state, 1000);
    assert(dual_status_led_logic_color(&state, 1039) == DUAL_STATUS_LED_COLOR_FLASH_OFF);
    assert(dual_status_led_logic_color(&state, 1040) == DUAL_STATUS_LED_COLOR_BLUE);
    dual_status_led_logic_set_pc_mounted(&state, false);
    assert(dual_status_led_logic_color(&state, 1020) == DUAL_STATUS_LED_COLOR_RED);

    dual_status_led_logic_set_pc_mounted(&state, true);
    dual_status_led_logic_notify_software_success(&state, 2000U);
    assert(dual_status_led_logic_color(&state, 2010U) == DUAL_STATUS_LED_COLOR_FLASH_OFF);
    dual_status_led_logic_set_role(&state, DUAL_STATUS_LED_ROLE_NONE);
    assert(dual_status_led_logic_color(&state, 20U) == DUAL_STATUS_LED_COLOR_RED);
}

int main(void)
{
    test_merge_and_independent_release();
    test_saturation_is_split_without_loss();
    test_peek_commit_preserves_pending_on_retry();
    test_bridge_protocol();
    test_cdc_parser_and_lease_release();
    test_runtime_scheduling_guards();
    test_status_led_logic();
    puts("dual_proxy_logic_test: PASS");
    return 0;
}
