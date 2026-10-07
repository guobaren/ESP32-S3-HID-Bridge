#include "uart0_protocol_router.h"
#include "m_udp_smoothing.h"
#include "usb_stall_recovery_logic.h"
#include "vendor_urb_pending_logic.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bridge_protocol.h"
#include "dual_input_aggregator.h"
#include "hid_device_profile.h"
#include "hid_clone_descriptor.h"
#include "hid_report_layout.h"
#include "hid_vendor_session_logic.h"
#include "link_recovery_logic.h"
#include "makcu_ascii_logic.h"
#include "makcu_v4_logic.h"
#include "mouse_motion_smoother.h"
#include "dual_proxy_runtime_config.h"
#include "dual_status_led_logic.h"
#include "usb_cdc_control_logic.h"
#include "uart1_vendor_queue_logic.h"

_Static_assert(DUAL_PROXY_REQUIRED_FREERTOS_HZ == 1000U, "FreeRTOS tick必须保持1000Hz");
_Static_assert(DUAL_PROXY_LINK_TX_BATCH_LIMIT > 0U, "UART1批量上限回归保护失败");
_Static_assert(DUAL_PROXY_HID_PERIOD_US == 1000U, "HID周期必须保持1000us");
_Static_assert(MOUSE_MOTION_SMOOTHING_MAX_SLOTS == 20U, "软件移动平滑最大窗必须保持20个1ms槽");
_Static_assert(DUAL_MESSAGE_PROFILE_BEGIN == 0x24, "ProfileBegin消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_PROFILE_CHUNK == 0x25, "ProfileChunk消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_PROFILE_COMMIT == 0x26, "ProfileCommit消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_PROFILE_ACK == 0x2E, "ProfileAck消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_ROLE_ACK == 0x2F, "RoleAck消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_PROFILE_REQUEST == 0x30, "ProfileRequest消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_PROFILE_OFFER == 0x31, "ProfileOffer消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_FLOW_ACK == 0x32, "FlowAck消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_VENDOR_SESSION_BEGIN == 0x35, "VendorSessionBegin类型回归保护失败");
_Static_assert(DUAL_MESSAGE_VENDOR_SESSION_ACK == 0x36, "VendorSessionAck类型回归保护失败");
_Static_assert(DUAL_LINK_VENDOR_SESSION_LENGTH == 12U, "VendorSession长度回归保护失败");
_Static_assert(DUAL_LINK_ROLE_ACK_LENGTH == 5U, "RoleAck长度回归保护失败");
_Static_assert(DUAL_LINK_PROFILE_REQUEST_LENGTH == 8U, "ProfileRequest长度回归保护失败");
_Static_assert(DUAL_LINK_PROFILE_OFFER_LENGTH == 12U, "ProfileOffer长度回归保护失败");
_Static_assert(DUAL_LINK_DEVICE_GONE_LENGTH == 13U, "DeviceGone长度回归保护失败");
_Static_assert(DUAL_LINK_FLOW_ACK_LENGTH == 14U, "FlowAck长度回归保护失败");
_Static_assert(DUAL_LINK_DEVICE_GONE_EVENT_ID_OFFSET + 4U ==
               DUAL_LINK_DEVICE_GONE_REASON_OFFSET,
               "DeviceGone事件ID/原因字段偏移回归保护失败");
_Static_assert(DUAL_LINK_FLOW_ACK_SENDER_GENERATION_OFFSET + 4U ==
               DUAL_LINK_FLOW_ACK_LENGTH,
               "FlowAck双向generation字段偏移回归保护失败");
_Static_assert(DUAL_MESSAGE_SOFTWARE_MOUSE == 0x2B, "SoftwareMouse消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_SOFTWARE_RELEASE == 0x2C, "SoftwareRelease消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_DEVICE_GONE == 0x2D, "DeviceGone消息类型回归保护失败");
_Static_assert(HID_PROFILE_MAX_CHUNK_DATA + HID_PROFILE_FRAME_CHUNK_HEADER ==
               DUAL_PROXY_MAX_PAYLOAD, "Profile分片必须适配64字节UART帧");

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

static void test_rolling_smoother_preserves_overlapping_500hz_motion(void)
{
    mouse_motion_smoother_t smoother;
    mouse_motion_smoother_reset(&smoother);

    mouse_motion_smoother_enqueue(
        &smoother, (mouse_motion_delta_t){.x = 20, .y = -10}, 5);
    mouse_motion_delta_t first = mouse_motion_smoother_take_next(&smoother);
    mouse_motion_delta_t second = mouse_motion_smoother_take_next(&smoother);
    assert(first.x == 4 && first.y == -2);
    assert(second.x == 4 && second.y == -2);

    /* 2 ms 后新命令叠加到未来滚动槽，而不排在旧命令之后。 */
    mouse_motion_smoother_enqueue(
        &smoother, (mouse_motion_delta_t){.x = -5, .y = 5}, 5);
    int64_t total_x = first.x + second.x;
    int64_t total_y = first.y + second.y;
    for (int index = 0; index < 5; ++index) {
        const mouse_motion_delta_t next = mouse_motion_smoother_take_next(&smoother);
        total_x += next.x;
        total_y += next.y;
    }
    assert(total_x == 15 && total_y == -5);
    assert(!mouse_motion_smoother_has_pending(&smoother));

    mouse_motion_smoother_enqueue(
        &smoother, (mouse_motion_delta_t){.x = 9, .y = 1}, 5);
    mouse_motion_smoother_reset(&smoother);
    assert(!mouse_motion_smoother_has_pending(&smoother));
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

typedef struct {
    unsigned count;
    makcu_ascii_command_t commands[16];
} makcu_ascii_capture_t;

static void capture_makcu_ascii_command(
    const makcu_ascii_command_t *command,
    void *context)
{
    makcu_ascii_capture_t *capture = (makcu_ascii_capture_t *)context;
    assert(capture != NULL && command != NULL);
    assert(capture->count < sizeof(capture->commands) / sizeof(capture->commands[0]));
    capture->commands[capture->count++] = *command;
}

static void test_makcu_ascii_parser_and_session(void)
{
    makcu_ascii_parser_t parser;
    makcu_ascii_parser_init(&parser);
    makcu_ascii_capture_t capture = {0};
    static const uint8_t first[] = "noise.move(12,-3,";
    static const uint8_t second[] = ")km.left(1).wheel(-9)";
    makcu_ascii_parser_feed(&parser, first, sizeof(first) - 1U,
                            capture_makcu_ascii_command, &capture);
    assert(capture.count == 0U);
    makcu_ascii_parser_feed(&parser, second, sizeof(second) - 1U,
                            capture_makcu_ascii_command, &capture);
    assert(capture.count == 3U);
    assert(capture.commands[0].kind == MAKCU_ASCII_COMMAND_MOVE);
    assert(capture.commands[0].argument[0] == 12 &&
           capture.commands[0].argument[1] == -3);
    assert(strcmp(capture.commands[0].body, "move(12,-3,)") == 0);
    assert(capture.commands[1].kind == MAKCU_ASCII_COMMAND_BUTTON &&
           capture.commands[1].button_index == 0U &&
           capture.commands[1].argument[0] == 1);
    assert(capture.commands[2].kind == MAKCU_ASCII_COMMAND_WHEEL &&
           capture.commands[2].argument[0] == -9);

    static const uint8_t query[] = ".side2()";
    makcu_ascii_parser_feed(&parser, query, sizeof(query) - 1U,
                            capture_makcu_ascii_command, &capture);
    assert(capture.count == 4U);
    assert(capture.commands[3].kind == MAKCU_ASCII_COMMAND_BUTTON &&
           capture.commands[3].query && capture.commands[3].button_index == 4U);

    static const uint8_t malformed[] =
        ".move(32768,0).left(3).left(2).move(1,x).unknown()";
    makcu_ascii_parser_feed(&parser, malformed, sizeof(malformed) - 1U,
                            capture_makcu_ascii_command, &capture);
    assert(capture.count == 9U);
    assert(capture.commands[4].kind == MAKCU_ASCII_COMMAND_ERROR &&
           capture.commands[4].error == MAKCU_ASCII_ERROR_ARGUMENT);
    assert(capture.commands[5].kind == MAKCU_ASCII_COMMAND_ERROR &&
           capture.commands[5].error == MAKCU_ASCII_ERROR_ARGUMENT);
    assert(capture.commands[6].kind == MAKCU_ASCII_COMMAND_ERROR &&
           capture.commands[6].error == MAKCU_ASCII_ERROR_UNSUPPORTED);
    assert(capture.commands[7].kind == MAKCU_ASCII_COMMAND_ERROR &&
           capture.commands[7].error == MAKCU_ASCII_ERROR_ARGUMENT);
    assert(capture.commands[8].kind == MAKCU_ASCII_COMMAND_ERROR &&
           capture.commands[8].error == MAKCU_ASCII_ERROR_UNSUPPORTED);

    char overlong[MAKCU_ASCII_COMMAND_MAX + 16U];
    overlong[0] = '.';
    memset(&overlong[1], 'x', sizeof(overlong) - 3U);
    overlong[sizeof(overlong) - 2U] = '(';
    overlong[sizeof(overlong) - 1U] = ')';
    const unsigned before_overlong = capture.count;
    makcu_ascii_parser_feed(&parser, (const uint8_t *)overlong,
                            sizeof(overlong), capture_makcu_ascii_command,
                            &capture);
    assert(capture.count == before_overlong + 1U);
    assert(capture.commands[before_overlong].kind == MAKCU_ASCII_COMMAND_ERROR &&
           capture.commands[before_overlong].error == MAKCU_ASCII_ERROR_TOO_LONG);

    const hid_mouse_report_layout_t layout = {
        .valid = true,
        .report_bytes = 2U,
        .buttons_bit_offset = 3U,
        .button_count = 6U,
    };
    const uint8_t report[] = {(uint8_t)((1U << 3U) | (1U << 5U)), 0x02U};
    uint8_t physical_buttons = 0U;
    assert(hid_mouse_report_read_buttons(report, sizeof(report), &layout,
                                         &physical_buttons));
    assert(physical_buttons == 0x05U);
    assert(!hid_mouse_report_read_buttons(report, 1U, &layout,
                                          &physical_buttons));

    makcu_ascii_session_t session;
    makcu_ascii_session_init(&session);
    assert(session.echo_enabled);
    bool send_report = false;
    assert(makcu_ascii_session_set_button(&session, 0U, 1, &send_report));
    assert(send_report && session.injected_buttons == 1U);
    makcu_ascii_session_touch(&session, 100U);
    assert(makcu_ascii_button_state(0U, 1U, session.injected_buttons) == 3U);
    assert(makcu_ascii_button_state(0U, 1U, 0U) == 1U);
    assert(makcu_ascii_button_state(0U, 0U, session.injected_buttons) == 2U);
    assert(!makcu_ascii_session_lease_expired(&session, 1599U));

    /* A GET is read-only: the caller does not touch the safety lease. */
    (void)makcu_ascii_button_state(0U, 0U, session.injected_buttons);
    assert(session.last_activity_ms == 100U);
    assert(makcu_ascii_session_lease_expired(&session, 1600U));

    assert(makcu_ascii_session_set_button(&session, 0U, 0, &send_report));
    assert(send_report && session.injected_buttons == 0U);
    makcu_ascii_session_touch(&session, 2000U);
    assert(!makcu_ascii_session_lease_expired(&session, 3499U));
    assert(makcu_ascii_session_lease_expired(&session, 3500U));
    makcu_ascii_session_expire(&session);
    assert(session.injected_buttons == 0U && !session.lease_active);
}

typedef struct {
    unsigned count;
    makcu_v4_command_t commands[64];
} makcu_v4_capture_t;

static void capture_makcu_v4_command(
    const makcu_v4_command_t *command,
    void *context)
{
    makcu_v4_capture_t *capture = (makcu_v4_capture_t *)context;
    assert(capture != NULL && command != NULL);
    assert(capture->count < sizeof(capture->commands) / sizeof(capture->commands[0]));
    capture->commands[capture->count++] = *command;
}

static void feed_makcu_v4(makcu_v4_stream_parser_t *parser,
                          makcu_v4_capture_t *capture,
                          const uint8_t *bytes, size_t length,
                          uint32_t now_ms)
{
    makcu_v4_stream_parser_feed(parser, bytes, length, now_ms,
                                capture_makcu_v4_command, capture);
}

static void test_makcu_v4_parsers_and_state(void)
{
    makcu_v4_stream_parser_t parser;
    makcu_v4_stream_parser_init(&parser);
    makcu_v4_capture_t capture = {0};

    /* Official ASCII spellings, CR-only probe termination, and V4 parameters. */
    static const uint8_t ascii[] =
        "km.move(20,-30,1,2,3,4,5)\r"
        "km.stream(mouse, 1)\r\n"
        "km.lock_mx+(1)\r"
        "km.buttons(3,1000)\r"
        "km.interpolate(255)\r"
        "km.click(5,2,5000)\r"
        "km.screen(32767,32767)\r"
        "km.side2()\r";
    feed_makcu_v4(&parser, &capture, ascii, sizeof(ascii) - 1U, 100U);
    assert(capture.count == 8U);
    assert(capture.commands[0].error == MAKCU_V4_ERROR_NONE &&
           capture.commands[0].opcode == 0x18U &&
           capture.commands[0].argument_count == 2U &&
           capture.commands[0].argument[0] == 20 &&
           capture.commands[0].argument[1] == -30);
    assert(capture.commands[1].opcode == 0x52U &&
           capture.commands[1].argument_count == 2U &&
           capture.commands[1].argument[0] == 1 &&
           capture.commands[1].argument[1] == 1);
    assert(capture.commands[2].opcode == 0x60U &&
           capture.commands[2].argument[0] == 6 &&
           capture.commands[2].argument[1] == 1);
    assert(capture.commands[3].opcode == 0x10U &&
           capture.commands[3].argument[0] == 3 &&
           capture.commands[3].argument[1] == 1000);
    assert(capture.commands[4].opcode == 0x1FU &&
           capture.commands[4].argument[0] == 255);
    assert(capture.commands[5].opcode == 0x61U &&
           capture.commands[5].argument[0] == 5 &&
           capture.commands[5].argument[2] == 5000);
    assert(capture.commands[6].opcode == 0x64U &&
           capture.commands[6].argument[0] == 32767 &&
           capture.commands[6].argument[1] == 32767);
    assert(capture.commands[7].opcode == 0x15U &&
           capture.commands[7].query);

    static const uint8_t bad_ascii[] =
        "km.lock_left(1)\rkm.click(0)\rkm.screen(32768,1)\r"
        "km.move_mask(1,0,2,0)\rkm.stream(keyboard,1)\r";
    const unsigned before_bad = capture.count;
    feed_makcu_v4(&parser, &capture, bad_ascii, sizeof(bad_ascii) - 1U, 101U);
    assert(capture.count == before_bad + 5U);
    for (unsigned index = before_bad; index < capture.count; ++index) {
        assert(capture.commands[index].error != MAKCU_V4_ERROR_NONE);
    }
    assert(capture.commands[before_bad + 4U].error == MAKCU_V4_ERROR_UNSUPPORTED);

    /* A binary payload containing a valid-looking text command stays binary. */
    const uint8_t payload_move_text[] = {'k','m','.','m','o','v','e','(','1',',','2',')'};
    uint8_t frame[5U + sizeof(payload_move_text)];
    size_t frame_length = 0U;
    assert(makcu_v4_encode_frame(0x18U, payload_move_text,
                                 sizeof(payload_move_text), frame,
                                 sizeof(frame), &frame_length));
    const unsigned before_binary_error = capture.count;
    feed_makcu_v4(&parser, &capture, frame, frame_length, 110U);
    assert(capture.count == before_binary_error + 1U);
    assert(capture.commands[before_binary_error].transport ==
           MAKCU_V4_TRANSPORT_BINARY);
    assert(capture.commands[before_binary_error].error ==
           MAKCU_V4_ERROR_ARGUMENT);

    const uint8_t device_get[] = {0xDEU, 0xADU, 0U, 0U, 0x02U};
    feed_makcu_v4(&parser, &capture, device_get, sizeof(device_get), 111U);
    assert(capture.commands[capture.count - 1U].opcode == 0x02U &&
           capture.commands[capture.count - 1U].query);

    /* Legacy V3/AIO baud envelopes must not consume the next command byte. */
    static const uint8_t legacy_a5_then_device[] = {
        0xDEU, 0xADU, 5U, 0U, 0xA5U, 0x00U, 0xC2U, 0x01U, 0x00U,
        0xDEU, 0xADU, 0U, 0U, 0x02U,
    };
    const unsigned before_legacy_a5 = capture.count;
    feed_makcu_v4(&parser, &capture, legacy_a5_then_device,
                  sizeof(legacy_a5_then_device), 111U);
    assert(capture.count == before_legacy_a5 + 2U);
    assert(capture.commands[before_legacy_a5].opcode == 0xA5U &&
           capture.commands[before_legacy_a5].error == MAKCU_V4_ERROR_NONE &&
           capture.commands[before_legacy_a5].payload_length == 4U);
    assert(capture.commands[before_legacy_a5 + 1U].opcode == 0x02U &&
           capture.commands[before_legacy_a5 + 1U].error ==
               MAKCU_V4_ERROR_NONE);

    static const uint8_t legacy_a4_then_device[] = {
        0xDEU, 0xADU, 1U, 0U, 0xA4U,
        0xDEU, 0xADU, 0U, 0U, 0x02U,
    };
    const unsigned before_legacy_a4 = capture.count;
    feed_makcu_v4(&parser, &capture, legacy_a4_then_device,
                  sizeof(legacy_a4_then_device), 111U);
    assert(capture.count == before_legacy_a4 + 2U);
    assert(capture.commands[before_legacy_a4].opcode == 0xA4U &&
           capture.commands[before_legacy_a4].query &&
           capture.commands[before_legacy_a4].error == MAKCU_V4_ERROR_NONE);
    assert(capture.commands[before_legacy_a4 + 1U].opcode == 0x02U &&
           capture.commands[before_legacy_a4 + 1U].error ==
               MAKCU_V4_ERROR_NONE);

    const uint8_t click5[] = {0xDEU, 0xADU, 1U, 0U, 0x61U, 5U};
    feed_makcu_v4(&parser, &capture, click5, sizeof(click5), 112U);
    assert(capture.commands[capture.count - 1U].error == MAKCU_V4_ERROR_NONE &&
           capture.commands[capture.count - 1U].payload[0] == 5U);
    const uint8_t click0[] = {0xDEU, 0xADU, 1U, 0U, 0x61U, 0U};
    feed_makcu_v4(&parser, &capture, click0, sizeof(click0), 113U);
    assert(capture.commands[capture.count - 1U].error ==
           MAKCU_V4_ERROR_ARGUMENT);
    const uint8_t keyboard[] = {0xDEU, 0xADU, 1U, 0U, 0x20U, 4U};
    feed_makcu_v4(&parser, &capture, keyboard, sizeof(keyboard), 114U);
    assert(capture.commands[capture.count - 1U].error ==
           MAKCU_V4_ERROR_UNSUPPORTED);

    /* An oversized declared frame is rejected and its full body is discarded. */
    uint8_t oversized[5U + 65U + sizeof(device_get)];
    oversized[0] = 0xDEU;
    oversized[1] = 0xADU;
    oversized[2] = 65U;
    oversized[3] = 0U;
    oversized[4] = 0x18U;
    memset(&oversized[5], 'x', 65U);
    memcpy(&oversized[70], device_get, sizeof(device_get));
    const unsigned before_oversized = capture.count;
    feed_makcu_v4(&parser, &capture, oversized, sizeof(oversized), 120U);
    assert(capture.count == before_oversized + 2U);
    assert(capture.commands[before_oversized].error == MAKCU_V4_ERROR_TOO_LONG);
    assert(capture.commands[before_oversized + 1U].opcode == 0x02U);

    const uint8_t partial[] = {0xDEU, 0xADU, 4U, 0U, 0x18U, 1U};
    feed_makcu_v4(&parser, &capture, partial, sizeof(partial), 200U);
    assert(parser.mode == 2U);
    makcu_v4_stream_parser_tick(&parser, 451U);
    assert(parser.mode == 0U);
    static const uint8_t after_timeout[] = "km.device()\r";
    feed_makcu_v4(&parser, &capture, after_timeout,
                  sizeof(after_timeout) - 1U, 452U);
    assert(capture.commands[capture.count - 1U].opcode == 0x02U &&
           capture.commands[capture.count - 1U].error == MAKCU_V4_ERROR_NONE);

    uint8_t error_frame[6];
    size_t error_length = 0U;
    assert(makcu_v4_encode_error(0x18U, error_frame, sizeof(error_frame),
                                 &error_length));
    const uint8_t expected_error[] = {0xDEU, 0xADU, 1U, 0U, 0x18U, 0xFFU};
    assert(error_length == sizeof(expected_error));
    assert(memcmp(error_frame, expected_error, sizeof(expected_error)) == 0);

    makcu_v4_state_t state;
    makcu_v4_state_init(&state);
    assert(state.screen_width == 1920U && state.pointer_x == 960U &&
           !state.echo_enabled && !state.mouse_stream_enabled &&
           !state.buttons_enabled);
    state.physical_buttons = 0x05U;
    assert(makcu_v4_state_ascii_button(&state, 0U) == 1U);
    assert(makcu_v4_state_set_button(&state, 0U, 1U));
    assert(makcu_v4_state_ascii_button(&state, 0U) == 3U);
    state.button_mask = 1U;
    assert(makcu_v4_state_effective_physical_buttons(&state) == 0x04U);
    assert(makcu_v4_state_ascii_button(&state, 0U) == 3U);
    makcu_v4_state_touch(&state, 100U);
    assert(!makcu_v4_state_lease_expired(&state, 1599U));
    assert(makcu_v4_state_lease_expired(&state, 1600U));
    assert(makcu_v4_state_schedule_click(&state, 2000U, 5000U));
    assert(!makcu_v4_state_lease_expired(&state, 6999U));
    assert(!makcu_v4_state_lease_expired(&state, 8499U));
    assert(makcu_v4_state_lease_expired(&state, 8500U));
    static const struct { uint8_t input; uint8_t normalized; } interpolate_cases[] = {
        {0U, 0U}, {12U, 0U}, {13U, 25U}, {37U, 25U},
        {38U, 50U}, {62U, 50U}, {63U, 75U}, {87U, 75U},
        {88U, 100U}, {100U, 100U}, {255U, 255U},
    };
    for (size_t index = 0U;
         index < sizeof(interpolate_cases) / sizeof(interpolate_cases[0]); ++index) {
        uint8_t normalized = 0xAAU;
        assert(makcu_v4_normalize_interpolate(
            interpolate_cases[index].input, &normalized));
        assert(normalized == interpolate_cases[index].normalized);
    }
    uint8_t normalized = 0U;
    assert(!makcu_v4_normalize_interpolate(101U, &normalized));
    assert(!makcu_v4_normalize_interpolate(254U, &normalized));
    assert(!makcu_v4_normalize_interpolate(25U, NULL));
    assert(makcu_v4_interpolate_slots(0U, 10U) == 0U);
    assert(makcu_v4_interpolate_slots(25U, 10U) == 5U);
    assert(makcu_v4_interpolate_slots(50U, 10U) == 10U);
    assert(makcu_v4_interpolate_slots(75U, 10U) == 15U);
    assert(makcu_v4_interpolate_slots(100U, 10U) == 20U);
    assert(makcu_v4_interpolate_slots(255U, 1U) == 0U);
    assert(makcu_v4_interpolate_slots(255U, 2U) == 0U);
    assert(makcu_v4_interpolate_slots(255U, 3U) == 5U);
    assert(makcu_v4_interpolate_slots(255U, 8U) == 10U);
    assert(makcu_v4_interpolate_slots(255U, 22U) == 20U);
    assert(makcu_v4_interpolate_slots(255U, UINT8_MAX) == 20U);
    const int32_t motion_cases[][3] = {
        {32768, 0, 0},       /* 已合并的 +32767 与 +1 */
        {-32768, 0, 0},
        {0, 0, 128},         /* 正向滚轮不能窄化为负数 */
        {0, 0, -128},
        {INT32_MIN, INT32_MAX, INT32_MIN},
    };
    for (size_t index = 0U;
         index < sizeof(motion_cases) / sizeof(motion_cases[0]); ++index) {
        int32_t remaining[3] = {
            motion_cases[index][0], motion_cases[index][1],
            motion_cases[index][2],
        };
        int64_t totals[3] = {0, 0, 0};
        const uint32_t steps = makcu_v4_motion_steps(
            remaining[0], remaining[1], remaining[2], 1U);
        const uint32_t maximum_motion_steps = 16909321U; /* ceil(2^31 / 127) */
        assert(steps > 0U && steps <= maximum_motion_steps);
        if (remaining[2] == INT32_MIN) {
            assert(steps == maximum_motion_steps);
        }
        for (uint32_t step = steps; step > 0U; --step) {
            const int32_t x = makcu_v4_motion_step(remaining[0], step);
            const int32_t y = makcu_v4_motion_step(remaining[1], step);
            const int32_t wheel = makcu_v4_motion_step(remaining[2], step);
            assert(x >= INT16_MIN && x <= INT16_MAX);
            assert(y >= INT16_MIN && y <= INT16_MAX);
            assert(wheel >= INT8_MIN && wheel <= INT8_MAX);
            totals[0] += x; totals[1] += y; totals[2] += wheel;
            remaining[0] -= x; remaining[1] -= y; remaining[2] -= wheel;
        }
        assert(remaining[0] == 0 && remaining[1] == 0 && remaining[2] == 0);
        assert(totals[0] == motion_cases[index][0]);
        assert(totals[1] == motion_cases[index][1]);
        assert(totals[2] == motion_cases[index][2]);
    }
    makcu_v4_state_track_move(&state, INT32_MAX, INT32_MIN);
    assert(state.pointer_x == state.screen_width - 1U && state.pointer_y == 0U);
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

static const uint8_t c092_mouse_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01,
    0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01,
    0x81, 0x02, 0x05, 0x01, 0x16, 0x01, 0x80, 0x26,
    0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x09, 0x30,
    0x09, 0x31, 0x81, 0x06, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81,
    0x06, 0xC0, 0xC0,
};

static const uint8_t c092_vendor_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x81, 0x03, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
    0x26, 0xFF, 0x00, 0x19, 0x00, 0x2A, 0xFF, 0x00,
    0x81, 0x00, 0xC0, 0x05, 0x0C, 0x09, 0x01, 0xA1,
    0x01, 0x85, 0x03, 0x75, 0x10, 0x95, 0x02, 0x15,
    0x01, 0x26, 0x8C, 0x02, 0x19, 0x01, 0x2A, 0x8C,
    0x02, 0x81, 0x00, 0xC0, 0x05, 0x01, 0x09, 0x80,
    0xA1, 0x01, 0x85, 0x04, 0x75, 0x02, 0x95, 0x01,
    0x15, 0x01, 0x25, 0x03, 0x09, 0x82, 0x09, 0x81,
    0x09, 0x83, 0x81, 0x60, 0x75, 0x06, 0x81, 0x03,
    0xC0, 0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01,
    0x85, 0x10, 0x75, 0x08, 0x95, 0x06, 0x15, 0x00,
    0x26, 0xFF, 0x00, 0x09, 0x01, 0x81, 0x00, 0x09,
    0x01, 0x91, 0x00, 0xC0, 0x06, 0x00, 0xFF, 0x09,
    0x02, 0xA1, 0x01, 0x85, 0x11, 0x75, 0x08, 0x95,
    0x13, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x09, 0x02,
    0x81, 0x00, 0x09, 0x02, 0x91, 0x00, 0xC0,
};

typedef struct {
    unsigned count;
    hid_device_profile_t profile;
} profile_capture_t;

static void capture_profile(const hid_device_profile_t *profile, void *context)
{
    profile_capture_t *capture = (profile_capture_t *)context;
    assert(capture != NULL);
    assert(profile != NULL);
    capture->profile = *profile;
    ++capture->count;
}

static void write_u32_le_test(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static void make_c092_profile(hid_device_profile_t *profile)
{
    static const uint8_t device_descriptor[18] = {
        18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x6D, 0x04, 0x92, 0xC0,
        0, 1, 1, 2, 3, 1,
    };
    static const uint8_t config_descriptor[9] = {
        9, 2, 0, 0, 2, 1, 0, 0xA0, 50,
    };
    hid_device_profile_init(profile);
    assert(hid_device_profile_set_device_descriptor(
        profile, device_descriptor, sizeof(device_descriptor)));
    assert(hid_device_profile_set_config_descriptor(
        profile, config_descriptor, sizeof(config_descriptor)));
    assert(hid_device_profile_set_string(
        &profile->manufacturer, "Logitech", strlen("Logitech")));
    assert(hid_device_profile_set_string(
        &profile->product, "G102 LIGHTSYNC Gaming Mouse", strlen("G102 LIGHTSYNC Gaming Mouse")));
    assert(hid_device_profile_set_string(&profile->serial, "", 0));
    assert(hid_device_profile_add_report_descriptor(
        profile, 0, 1, 2, c092_mouse_report_descriptor, sizeof(c092_mouse_report_descriptor)));
    assert(hid_device_profile_add_report_descriptor(
        profile, 1, 0, 0, c092_vendor_report_descriptor, sizeof(c092_vendor_report_descriptor)));
}

static void test_profile_model_and_roundtrip(void)
{
    static const uint8_t crc_input[] = "123456789";
    assert(hid_profile_crc32(crc_input, sizeof(crc_input) - 1) == 0xCBF43926U);
    assert(sizeof(c092_mouse_report_descriptor) == 67U);
    assert(sizeof(c092_vendor_report_descriptor) == 151U);

    hid_device_profile_t empty;
    hid_device_profile_init(&empty);
    uint8_t empty_blob[HID_PROFILE_MAX_BLOB] = {0};
    size_t empty_length = 0;
    assert(hid_device_profile_serialize(
        &empty, empty_blob, sizeof(empty_blob), &empty_length));
    assert(empty_length == HID_PROFILE_SERIAL_HEADER_SIZE);
    hid_device_profile_t empty_roundtrip;
    assert(hid_device_profile_deserialize(&empty_roundtrip, empty_blob, empty_length));
    assert(empty_roundtrip.report_descriptor_count == 0);

    hid_device_profile_t source;
    make_c092_profile(&source);
    uint8_t blob[HID_PROFILE_MAX_BLOB] = {0};
    size_t blob_length = 0;
    assert(hid_device_profile_serialize(&source, blob, sizeof(blob), &blob_length));
    hid_device_profile_t decoded;
    assert(hid_device_profile_deserialize(&decoded, blob, blob_length));
    assert(decoded.device_descriptor.length == source.device_descriptor.length);
    assert(decoded.config_descriptor.length == source.config_descriptor.length);
    assert(decoded.manufacturer.length == source.manufacturer.length);
    assert(strcmp(decoded.manufacturer.data, "Logitech") == 0);
    assert(strcmp(decoded.product.data, "G102 LIGHTSYNC Gaming Mouse") == 0);
    assert(decoded.report_descriptor_count == 2);
    assert(decoded.report_descriptors[0].interface_number == 0);
    assert(decoded.report_descriptors[0].subclass == 1);
    assert(decoded.report_descriptors[0].protocol == 2);
    assert(decoded.report_descriptors[0].length == 67);
    assert(decoded.report_descriptors[1].interface_number == 1);
    assert(decoded.report_descriptors[1].subclass == 0);
    assert(decoded.report_descriptors[1].protocol == 0);
    assert(decoded.report_descriptors[1].length == 151);
    assert(memcmp(decoded.report_descriptors[0].data,
                  c092_mouse_report_descriptor, sizeof(c092_mouse_report_descriptor)) == 0);
    assert(memcmp(decoded.report_descriptors[1].data,
                  c092_vendor_report_descriptor, sizeof(c092_vendor_report_descriptor)) == 0);

    uint8_t truncated[HID_PROFILE_MAX_BLOB] = {0};
    memcpy(truncated, blob, blob_length);
    assert(!hid_device_profile_deserialize(&decoded, truncated, blob_length - 1U));
    truncated[6] = (uint8_t)(HID_PROFILE_SERIAL_HEADER_SIZE - 1U);
    assert(!hid_device_profile_deserialize(&decoded, truncated, blob_length));

    assert(!hid_device_profile_set_string(
        &source.manufacturer, "\xC0\x80", 2));
    assert(!hid_device_profile_add_report_descriptor(
        &source, 0, 1, 2, c092_mouse_report_descriptor, sizeof(c092_mouse_report_descriptor)));
    assert(!hid_device_profile_add_report_descriptor(
        &source, 2, 0, 0, c092_vendor_report_descriptor,
        HID_PROFILE_MAX_REPORT_DESCRIPTOR + 1U));
}

static void fill_bytes(uint8_t *data, size_t length, uint8_t seed)
{
    for (size_t index = 0; index < length; ++index) {
        data[index] = (uint8_t)(seed + index);
    }
}

static void test_profile_size_boundaries(void)
{
    hid_device_profile_t maximum;
    hid_device_profile_init(&maximum);
    uint8_t device[HID_PROFILE_MAX_DEVICE_DESCRIPTOR];
    uint8_t config[1038];
    uint8_t report[HID_PROFILE_MAX_REPORT_DESCRIPTOR];
    char string[HID_PROFILE_MAX_STRING_BYTES + 1U];
    fill_bytes(device, sizeof(device), 0x10);
    fill_bytes(config, sizeof(config), 0x20);
    fill_bytes(report, sizeof(report), 0x30);
    memset(string, 'x', HID_PROFILE_MAX_STRING_BYTES);
    string[HID_PROFILE_MAX_STRING_BYTES] = '\0';
    assert(hid_device_profile_set_device_descriptor(&maximum, device, sizeof(device)));
    assert(hid_device_profile_set_config_descriptor(&maximum, config, sizeof(config)));
    assert(hid_device_profile_set_string(&maximum.manufacturer, string, HID_PROFILE_MAX_STRING_BYTES));
    assert(hid_device_profile_set_string(&maximum.product, string, HID_PROFILE_MAX_STRING_BYTES));
    assert(hid_device_profile_set_string(&maximum.serial, string, HID_PROFILE_MAX_STRING_BYTES));
    for (uint8_t interface_number = 0; interface_number < 5; ++interface_number) {
        assert(hid_device_profile_add_report_descriptor(
            &maximum, interface_number, 1, 2, report, sizeof(report)));
    }
    uint8_t blob[HID_PROFILE_MAX_BLOB] = {0};
    size_t length = 0;
    assert(hid_device_profile_serialize(&maximum, blob, sizeof(blob), &length));
    assert(length == HID_PROFILE_MAX_BLOB);
    assert(!hid_device_profile_serialize(&maximum, blob, length - 1U, &length));

    hid_device_profile_t oversized;
    hid_device_profile_init(&oversized);
    assert(hid_device_profile_set_config_descriptor(&oversized, config, sizeof(config)));
    for (uint8_t interface_number = 0; interface_number < 6; ++interface_number) {
        assert(hid_device_profile_add_report_descriptor(
            &oversized, interface_number, 0, 0, report, sizeof(report)));
    }
    assert(!hid_device_profile_serialize(&oversized, blob, sizeof(blob), &length));
}

static void test_profile_stream_fairness(void)
{
    assert(!hid_profile_stream_can_send(false, false, false, 0, 8));
    assert(!hid_profile_stream_can_send(true, true, false, 8, 8));
    assert(hid_profile_stream_can_send(true, false, false, 0, 8));
    assert(!hid_profile_stream_can_send(true, false, true, 7, 8));
    assert(hid_profile_stream_can_send(true, false, true, 8, 8));
    assert(hid_profile_stream_can_send(true, false, true, 0, 0));
    assert(hid_profile_stream_needs_restart(false, 10, 10));
    assert(!hid_profile_stream_needs_restart(true, 10, 10));
    assert(hid_profile_stream_needs_restart(true, 10, 11));
    assert(!hid_profile_stream_needs_restart(true, 0x12345678U, 0x12345678U));
    assert(hid_profile_stream_needs_restart(true, 0x12345678U, 0x92345678U));
    assert(!hid_profile_stream_retry_due(false, false, false, 11000000, 6000000, 5000000));
    assert(!hid_profile_stream_retry_due(true, true, false, 11000000, 6000000, 5000000));
    assert(!hid_profile_stream_retry_due(true, false, true, 11000000, 6000000, 5000000));
    assert(!hid_profile_stream_retry_due(true, false, false, 10999999, 6000000, 5000000));
    assert(hid_profile_stream_retry_due(true, false, false, 11000000, 6000000, 5000000));
    assert(!hid_profile_stream_retry_due(true, false, false, 5000000, 6000000, 5000000));
}

static void test_dynamic_clone_descriptor_builder(void)
{
    static const uint8_t c092_device[] = {
        18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x6D, 0x04, 0x92, 0xC0,
        0, 1, 1, 2, 3, 1,
    };
    static const uint8_t c092_config[] = {
        9, 2, 59, 0, 2, 1, 0, 0xA0, 50,
        9, 4, 0, 0, 1, 3, 1, 2, 0,
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, 67, 0,
        7, 5, 0x81, 3, 8, 0, 1,
        9, 4, 1, 0, 1, 3, 0, 0, 0,
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, 151, 0,
        7, 5, 0x82, 3, 20, 0, 1,
    };
    hid_device_profile_t profile;
    make_c092_profile(&profile);
    assert(hid_device_profile_set_device_descriptor(
        &profile, c092_device, sizeof(c092_device)));
    assert(hid_device_profile_set_config_descriptor(
        &profile, c092_config, sizeof(c092_config)));
    hid_clone_descriptor_set_t clone;
    assert(hid_clone_descriptor_build(&profile, &clone));
    assert(clone.configuration_length == 125U);
    assert(clone.configuration_descriptor[2] == 125U);
    assert(clone.configuration_descriptor[4] == 4U);
    assert(clone.hid_count == 2U);
    assert(clone.hid_interface_numbers[0] == 0U);
    assert(clone.hid_interface_numbers[1] == 1U);
    assert(clone.cdc_control_interface == 2U);
    assert(clone.cdc_data_interface == 3U);
    assert(clone.cdc_notification_endpoint == 0x83U);
    assert(clone.cdc_data_in_endpoint == 0x84U);
    assert(clone.cdc_data_out_endpoint == 0x01U);

    hid_clone_descriptor_set_t exact_clone;
    assert(hid_clone_descriptor_build_exact(&profile, &exact_clone));
    assert(exact_clone.configuration_length == 59U);
    assert(exact_clone.configuration_descriptor[2] == 59U);
    assert(exact_clone.configuration_descriptor[4] == 2U);
    assert(exact_clone.hid_count == 2U);
    assert(exact_clone.cdc_control_interface == 0U);
    assert(exact_clone.cdc_data_interface == 0U);

    profile.config_descriptor.data[2] = 58;
    assert(!hid_clone_descriptor_build(&profile, &clone));
    profile.config_descriptor.data[2] = 59;
    profile.report_descriptor_count = 1;
    assert(!hid_clone_descriptor_build(&profile, &clone));
}

static void feed_profile_frames(
    hid_profile_receiver_t *receiver,
    const uint8_t *blob,
    size_t length,
    uint32_t transfer_id,
    uint32_t crc32)
{
    dual_frame_t frame = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_PROFILE_BEGIN,
        .sequence = 1,
        .payload_length = 12,
    };
    write_u32_le_test(&frame.payload[0], transfer_id);
    write_u32_le_test(&frame.payload[4], (uint32_t)length);
    write_u32_le_test(&frame.payload[8], crc32);
    assert(hid_profile_receiver_accept_frame(receiver, &frame));

    size_t offset = 0;
    uint16_t sequence = 2;
    while (offset < length) {
        const size_t chunk_length = (length - offset) < HID_PROFILE_MAX_CHUNK_DATA
            ? length - offset : HID_PROFILE_MAX_CHUNK_DATA;
        memset(&frame, 0, sizeof(frame));
        frame.version = DUAL_PROXY_PROTOCOL_VERSION;
        frame.type = DUAL_MESSAGE_PROFILE_CHUNK;
        frame.sequence = sequence++;
        frame.payload_length = (uint8_t)(HID_PROFILE_FRAME_CHUNK_HEADER + chunk_length);
        write_u32_le_test(&frame.payload[0], transfer_id);
        write_u32_le_test(&frame.payload[4], (uint32_t)offset);
        memcpy(&frame.payload[HID_PROFILE_FRAME_CHUNK_HEADER], &blob[offset], chunk_length);
        assert(hid_profile_receiver_accept_frame(receiver, &frame));
        offset += chunk_length;
    }

    memset(&frame, 0, sizeof(frame));
    frame.version = DUAL_PROXY_PROTOCOL_VERSION;
    frame.type = DUAL_MESSAGE_PROFILE_COMMIT;
    frame.sequence = sequence;
    frame.payload_length = 12;
    write_u32_le_test(&frame.payload[0], transfer_id);
    write_u32_le_test(&frame.payload[4], (uint32_t)length);
    write_u32_le_test(&frame.payload[8], crc32);
    assert(hid_profile_receiver_accept_frame(receiver, &frame));
}

static void test_profile_receiver_state_machine(void)
{
    hid_device_profile_t source;
    make_c092_profile(&source);
    uint8_t blob[HID_PROFILE_MAX_BLOB] = {0};
    size_t length = 0;
    assert(hid_device_profile_serialize(&source, blob, sizeof(blob), &length));
    const uint32_t crc32 = hid_profile_crc32(blob, length);

    profile_capture_t capture = {0};
    hid_profile_receiver_t receiver;
    hid_profile_receiver_init(&receiver, capture_profile, &capture);
    dual_frame_t begin = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_PROFILE_BEGIN,
        .payload_length = 12,
    };
    write_u32_le_test(&begin.payload[0], 7);
    write_u32_le_test(&begin.payload[4], (uint32_t)length);
    write_u32_le_test(&begin.payload[8], crc32);
    assert(hid_profile_receiver_accept_frame(&receiver, &begin));
    assert(hid_profile_receiver_is_active(&receiver));
    assert(!hid_profile_receiver_has_profile(&receiver));

    dual_frame_t out_of_order = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_PROFILE_CHUNK,
        .payload_length = HID_PROFILE_FRAME_CHUNK_HEADER + 1,
    };
    write_u32_le_test(&out_of_order.payload[0], 7);
    write_u32_le_test(&out_of_order.payload[4], 1);
    out_of_order.payload[HID_PROFILE_FRAME_CHUNK_HEADER] = blob[0];
    assert(!hid_profile_receiver_accept_frame(&receiver, &out_of_order));
    assert(!hid_profile_receiver_is_active(&receiver));
    assert(!hid_profile_receiver_has_profile(&receiver));

    feed_profile_frames(&receiver, blob, length, 8, crc32);
    assert(capture.count == 1);
    assert(hid_profile_receiver_has_profile(&receiver));
    assert(receiver.published_transfer_id == 8U);
    assert(receiver.published_crc32 == crc32);
    assert(hid_profile_receiver_get_profile(&receiver)->report_descriptor_count == 2);

    /* A duplicate chunk, wrong transfer id, or bad commit discards the transfer. */
    assert(hid_profile_receiver_begin(&receiver, 9, (uint32_t)length, crc32));
    assert(hid_profile_receiver_chunk(&receiver, 9, 0, blob, 3));
    /* 重复到达且内容一致的片段：幂等，不重复计入也不丢弃当前传输。 */
    assert(hid_profile_receiver_chunk(&receiver, 9, 0, blob, 1));
    assert(hid_profile_receiver_chunk(&receiver, 9, 0, blob, 3));
    assert(hid_profile_receiver_is_active(&receiver));
    /* 只有内容冲突的重复片段才判失败并丢弃该传输。 */
    uint8_t conflicting = (uint8_t)(blob[0] ^ 0xFFU);
    assert(!hid_profile_receiver_chunk(&receiver, 9, 0, &conflicting, 1));
    assert(!hid_profile_receiver_is_active(&receiver));
    assert(hid_profile_receiver_has_profile(&receiver));
    assert(hid_profile_receiver_begin(&receiver, 10, (uint32_t)length, crc32));
    assert(!hid_profile_receiver_chunk(&receiver, 11, 0, blob, 1));
    assert(hid_profile_receiver_has_profile(&receiver));

    assert(hid_profile_receiver_begin(&receiver, 12, (uint32_t)length, crc32 ^ 1U));
    size_t offset = 0;
    while (offset < length) {
        const size_t chunk_length = (length - offset) < HID_PROFILE_MAX_CHUNK_DATA
            ? length - offset : HID_PROFILE_MAX_CHUNK_DATA;
        assert(hid_profile_receiver_chunk(&receiver, 12, (uint32_t)offset,
                                           &blob[offset], chunk_length));
        offset += chunk_length;
    }
    assert(!hid_profile_receiver_commit(&receiver, 12, (uint32_t)length, crc32 ^ 1U));
    assert(hid_profile_receiver_has_profile(&receiver));

    /* A new begin replaces an incomplete transfer rather than publishing it. */
    assert(hid_profile_receiver_begin(&receiver, 13, (uint32_t)length, crc32));
    assert(hid_profile_receiver_chunk(&receiver, 13, 0, blob, 2));
    assert(hid_profile_receiver_begin(&receiver, 14, (uint32_t)length, crc32));
    assert(!hid_profile_receiver_commit(&receiver, 13, (uint32_t)length, crc32));
    assert(hid_profile_receiver_has_profile(&receiver));
    feed_profile_frames(&receiver, blob, length, 14, crc32);
    assert(capture.count == 2);
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

static void assert_logitech_mouse_layout(
    const uint8_t *descriptor,
    size_t length,
    uint8_t expected_report_id)
{
    hid_mouse_report_layout_t layout;
    memset(&layout, 0, sizeof(layout));
    assert(hid_report_find_mouse_layout(descriptor, length, &layout));
    assert(layout.valid);
    assert(layout.report_id == expected_report_id);
    assert(layout.report_bytes == 8U);
    assert(layout.buttons_bit_offset == 0U);
    assert(layout.button_count == 16U);
    assert(layout.x.bit_offset == 16U && layout.x.bit_size == 16U);
    assert(layout.y.bit_offset == 32U && layout.y.bit_size == 16U);
    assert(layout.wheel.bit_offset == 48U && layout.wheel.bit_size == 8U);
    assert(layout.pan.bit_offset == 56U && layout.pan.bit_size == 8U);
}

static void test_dynamic_mouse_report_layout(void)
{
    static const uint8_t c092_mouse[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01,
        0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10,
        0x15, 0x00, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01,
        0x81, 0x02, 0x05, 0x01, 0x16, 0x01, 0x80, 0x26,
        0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x09, 0x30,
        0x09, 0x31, 0x81, 0x06, 0x15, 0x81, 0x25, 0x7f,
        0x75, 0x08, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06,
        0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01, 0x81,
        0x06, 0xc0, 0xc0,
    };
    static const uint8_t c539_mouse[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02,
        0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
        0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x95, 0x10,
        0x75, 0x01, 0x81, 0x02, 0x05, 0x01, 0x16, 0x01,
        0x80, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02,
        0x09, 0x30, 0x09, 0x31, 0x81, 0x06, 0x15, 0x81,
        0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38,
        0x81, 0x06, 0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95,
        0x01, 0x81, 0x06, 0xc0, 0xc0, 0x05, 0x0c, 0x09,
        0x01, 0xa1, 0x01, 0x85, 0x03, 0x75, 0x10, 0x95,
        0x02, 0x15, 0x01, 0x26, 0xff, 0x02, 0x19, 0x01,
        0x2a, 0xff, 0x02, 0x81, 0x00, 0xc0, 0x05, 0x01,
        0x09, 0x80, 0xa1, 0x01, 0x85, 0x04, 0x75, 0x02,
        0x95, 0x01, 0x15, 0x01, 0x25, 0x03, 0x09, 0x82,
        0x09, 0x81, 0x09, 0x83, 0x81, 0x60, 0x75, 0x06,
        0x81, 0x03, 0xc0, 0x06, 0xbc, 0xff, 0x09, 0x88,
        0xa1, 0x01, 0x85, 0x08, 0x19, 0x01, 0x29, 0xff,
        0x15, 0x01, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95,
        0x01, 0x81, 0x00, 0xc0,
    };
    static const uint8_t vendor_only[] = {
        0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85,
        0x10, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xc0,
    };
    static const uint8_t malformed[] = {0x05};

    assert_logitech_mouse_layout(c092_mouse, sizeof(c092_mouse), 0U);
    assert_logitech_mouse_layout(c539_mouse, sizeof(c539_mouse), 2U);
    hid_mouse_report_layout_t layout;
    assert(!hid_report_find_mouse_layout(
        vendor_only, sizeof(vendor_only), &layout));
    assert(!hid_report_find_mouse_layout(
        malformed, sizeof(malformed), &layout));

    assert(hid_report_find_mouse_layout(
        c539_mouse, sizeof(c539_mouse), &layout));
    uint8_t report[8] = {
        0x01, 0x80, /* physical button 1 and physical button 16 */
        0x7b, 0x00, /* stale physical X, must not repeat */
        0x38, 0xff, /* stale physical Y, must not repeat */
        0x04, 0xfb, /* stale wheel/pan, must not repeat */
    };
    assert(hid_mouse_report_apply_overlay(
        &layout, report, sizeof(report), 0x02U, 20, -5, 1, -1));
    static const uint8_t expected[] = {
        0x03, 0x80, 0x14, 0x00, 0xfb, 0xff, 0x01, 0xff,
    };
    assert(memcmp(report, expected, sizeof(expected)) == 0);

    uint8_t physical_masked[8] = {
        0x01, 0x00, 0x7b, 0x00, 0x38, 0xff, 0x04, 0xfb,
    };
    assert(hid_mouse_report_apply_physical_masks(
        &layout, physical_masked, sizeof(physical_masked),
        0x01U, 0x0AU, 0x02U));
    uint8_t physical_buttons = 0xFFU;
    int32_t masked_x = 1, masked_y = 1, masked_wheel = 1, masked_pan = 1;
    assert(hid_mouse_report_read_buttons(physical_masked,
        sizeof(physical_masked), &layout, &physical_buttons));
    assert(physical_buttons == 0U);
    assert(hid_mouse_report_read_axes(physical_masked, sizeof(physical_masked),
        &layout, &masked_x, &masked_y, &masked_wheel, &masked_pan));
    assert(masked_x == 0 && masked_y == 0 && masked_wheel == 0 &&
           masked_pan == -5);
    assert(!hid_mouse_report_apply_overlay(
        &layout, report, sizeof(report) - 1U, 0, 0, 0, 0, 0));
}

static void test_status_led_logic(void)
{
    dual_status_led_state_t state;
    dual_status_led_logic_init(&state);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_OFF);

    dual_status_led_logic_set_role(&state, DUAL_STATUS_LED_ROLE_MOUSE_HOST);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_RED);
    dual_status_led_logic_set_peer_connected(&state, true);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_BLUE);
    dual_status_led_logic_set_flow_error(&state, true);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_RED);
    assert(dual_status_led_logic_color(&state, 250) == DUAL_STATUS_LED_COLOR_OFF);
    dual_status_led_logic_set_flow_error(&state, false);
    dual_status_led_logic_set_host_mouse_ready(&state, true);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_BLUE);
    dual_status_led_logic_set_peer_usb_ready(&state, true);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_GREEN);
    dual_status_led_logic_set_host_mouse_ready(&state, false);
    assert(dual_status_led_logic_color(&state, 0) == DUAL_STATUS_LED_COLOR_BLUE);

    dual_status_led_logic_set_role(&state, DUAL_STATUS_LED_ROLE_PC_DEVICE);
    assert(dual_status_led_logic_color(&state, 1000) == DUAL_STATUS_LED_COLOR_BLUE);
    dual_status_led_logic_set_pc_mounted(&state, true);
    assert(dual_status_led_logic_color(&state, 1000) == DUAL_STATUS_LED_COLOR_BLUE);
    dual_status_led_logic_notify_software_success(&state, 1000);
    assert(dual_status_led_logic_color(&state, 1039) == DUAL_STATUS_LED_COLOR_FLASH_OFF);
    assert(dual_status_led_logic_color(&state, 1040) == DUAL_STATUS_LED_COLOR_BLUE);
    dual_status_led_logic_set_pc_mounted(&state, false);
    assert(dual_status_led_logic_color(&state, 1020) == DUAL_STATUS_LED_COLOR_BLUE);

    dual_status_led_logic_set_pc_mounted(&state, true);
    dual_status_led_logic_notify_software_success(&state, 2000U);
    assert(dual_status_led_logic_color(&state, 2010U) == DUAL_STATUS_LED_COLOR_FLASH_OFF);
    dual_status_led_logic_set_role(&state, DUAL_STATUS_LED_ROLE_NONE);
    assert(dual_status_led_logic_color(&state, 20U) == DUAL_STATUS_LED_COLOR_OFF);
}

static void test_usb_role_claim_complement(void)
{
    assert(DUAL_ROLE_UNRESOLVED == 0);
    assert(DUAL_ROLE_PC_DEVICE == 1);
    assert(DUAL_ROLE_MOUSE_HOST == 2);
    assert(dual_role_opposite(DUAL_ROLE_PC_DEVICE) == DUAL_ROLE_MOUSE_HOST);
    assert(dual_role_opposite(DUAL_ROLE_MOUSE_HOST) == DUAL_ROLE_PC_DEVICE);
    assert(dual_role_opposite(DUAL_ROLE_UNRESOLVED) == DUAL_ROLE_UNRESOLVED);
    assert(dual_role_resolve(DUAL_ROLE_PC_DEVICE, DUAL_ROLE_MOUSE_HOST) == DUAL_ROLE_MOUSE_HOST);
    assert(dual_role_resolve(DUAL_ROLE_MOUSE_HOST, DUAL_ROLE_PC_DEVICE) == DUAL_ROLE_PC_DEVICE);
    assert(dual_role_resolve(DUAL_ROLE_UNRESOLVED, DUAL_ROLE_PC_DEVICE) == DUAL_ROLE_PC_DEVICE);
    assert(dual_role_resolve(DUAL_ROLE_UNRESOLVED, DUAL_ROLE_MOUSE_HOST) == DUAL_ROLE_MOUSE_HOST);
    assert(dual_role_resolve(DUAL_ROLE_UNRESOLVED, DUAL_ROLE_UNRESOLVED) == DUAL_ROLE_UNRESOLVED);
}

static void test_peer_profile_invalidation(void)
{
    assert(!dual_peer_profile_invalidated(false, 0, 1, false, 0, 0));
    assert(!dual_peer_profile_invalidated(true, 1, 1, false, 0, 0));
    assert(dual_peer_profile_invalidated(true, 1, 2, false, 0, 0));
    assert(!dual_peer_profile_invalidated(true, 1, 1, true, 0, 0));
    assert(!dual_peer_profile_invalidated(true, 1, 1, true, 0, 1));
    assert(dual_peer_profile_invalidated(true, 1, 1, true, 2, 0));
    assert(dual_peer_profile_invalidated(true, 1, 1, true, 2, 3));
}

static void test_vendor_session_invalidates_inflight_work(void)
{
    const uint32_t old_session = 17U;
    const uint32_t new_session = hid_vendor_session_advance(old_session);
    assert(new_session == 18U);
    assert(hid_vendor_session_matches(old_session, old_session));
    assert(!hid_vendor_session_matches(old_session, new_session));
    assert(!hid_vendor_session_matches(0U, new_session));
    assert(hid_vendor_session_advance(UINT32_MAX) == 1U);
}

static void test_device_gone_barrier_and_reconfigure_epoch(void)
{
    assert(!dual_disconnect_barrier_allows_profile_offer(true, false));
    assert(!dual_disconnect_barrier_allows_profile_offer(false, true));
    assert(dual_disconnect_barrier_allows_profile_offer(false, false));

    assert(dual_flow_id_is_newer(11U, 10U));
    assert(!dual_flow_id_is_newer(10U, 10U));
    assert(!dual_flow_id_is_newer(9U, 10U));
    assert(dual_flow_id_is_newer(1U, UINT32_MAX));

    assert(dual_profile_operation_is_current(7U, 7U, false));
    assert(!dual_profile_operation_is_current(7U, 8U, false));
    assert(!dual_profile_operation_is_current(7U, 7U, true));

    assert(dual_profile_request_may_send(
        DUAL_ROLE_PC_DEVICE, true, true, true, false, false));
    assert(!dual_profile_request_may_send(
        DUAL_ROLE_PC_DEVICE, true, true, false, false, false));
    assert(!dual_profile_request_may_send(
        DUAL_ROLE_PC_DEVICE, true, true, true, true, false));
    assert(!dual_profile_request_may_send(
        DUAL_ROLE_PC_DEVICE, true, true, true, false, true));
    assert(!dual_profile_request_may_send(
        DUAL_ROLE_MOUSE_HOST, true, true, true, false, false));
}

/*
 * 以下是双板连接恢复的纯逻辑回归（原「连接恢复修复方案」第 1、5、7 阶段；该文档已删除，
 * 现行规则见 docs/protocol.md 的「有界重试与去重规则」与 docs/连接流程.md）：
 * 事务身份、去重规则、共享预算、失败分类，以及按方案表格逐项做的故障注入。
 * 这些是模型级验证，不替代双板实机、PnP、G HUB 或输入性能验收。
 */

static void test_link_flow_transaction_model(void)
{
    link_flow_t flow;
    link_flow_reset(&flow);
    assert(flow.state == LINK_FLOW_IDLE);
    assert(!link_flow_incomplete(&flow));
    assert(link_flow_poll(&flow, 1000) == LINK_FLOW_ACTION_NONE);

    const int64_t t0 = 1000000;
    link_flow_start(&flow, DUAL_MESSAGE_DEVICE_GONE, 42U, t0,
                    LINK_GONE_MAX_ATTEMPTS, LINK_GONE_RETRY_INTERVAL_US,
                    LINK_GONE_STAGE_TIMEOUT_US);
    /* 新建事务是 QUEUED：受门控也要占用身份，不能被当成“门已开放”。 */
    assert(link_flow_queued(&flow));
    assert(link_flow_incomplete(&flow));
    assert(link_flow_action_due(&flow, t0));
    assert(link_flow_poll(&flow, t0) == LINK_FLOW_ACTION_SEND);

    flow.peer_generation = 7U;
    link_flow_mark_sent(&flow, t0);
    assert(link_flow_pending(&flow));
    assert(flow.attempts == 1U);
    assert(!flow.resend_requested);

    /* 重发只在间隔到点或显式要求时发生，且 ID 保持不变。 */
    assert(!link_flow_action_due(&flow, t0 + LINK_GONE_RETRY_INTERVAL_US - 1));
    assert(link_flow_poll(&flow, t0 + LINK_GONE_RETRY_INTERVAL_US - 1) ==
           LINK_FLOW_ACTION_NONE);
    assert(link_flow_poll(&flow, t0 + LINK_GONE_RETRY_INTERVAL_US) ==
           LINK_FLOW_ACTION_RESEND);
    link_flow_mark_sent(&flow, t0 + LINK_GONE_RETRY_INTERVAL_US);
    assert(flow.id == 42U && flow.attempts == 2U);

    /* 只有 type/ID/generation 全匹配才算同一事务。 */
    assert(link_flow_matches(&flow, DUAL_MESSAGE_DEVICE_GONE, 42U, 7U));
    assert(!link_flow_matches(&flow, DUAL_MESSAGE_DEVICE_GONE, 42U, 8U));
    assert(!link_flow_matches(&flow, DUAL_MESSAGE_DEVICE_GONE, 43U, 7U));
    assert(!link_flow_matches(&flow, DUAL_MESSAGE_PROFILE_OFFER, 42U, 7U));
    assert(!link_flow_matches(&flow, DUAL_MESSAGE_DEVICE_GONE, 0U, 7U));

    /* 对端报告清理失败：保留同一身份并安排有界重发。 */
    link_flow_request_resend(&flow);
    assert(link_flow_pending(&flow));
    assert(link_flow_poll(&flow, t0 + LINK_GONE_RETRY_INTERVAL_US + 1) ==
           LINK_FLOW_ACTION_RESEND);
    link_flow_mark_sent(&flow, t0 + LINK_GONE_RETRY_INTERVAL_US + 1);

    /* 匹配确认结束事务，重复确认不再改变状态。 */
    assert(link_flow_ack(&flow, true));
    assert(flow.state == LINK_FLOW_ACCEPTED);
    assert(!link_flow_ack(&flow, true));
    assert(flow.state == LINK_FLOW_ACCEPTED);
    assert(link_flow_completed(&flow));
    assert(!link_flow_incomplete(&flow));
    assert(link_flow_poll(&flow, t0 + LINK_GONE_STAGE_TIMEOUT_US) ==
           LINK_FLOW_ACTION_NONE);
}

static void test_link_flow_retry_budget_exhaustion(void)
{
    link_flow_t flow;
    link_flow_reset(&flow);
    const int64_t t0 = 500000;
    link_flow_start(&flow, DUAL_MESSAGE_DEVICE_GONE, 1U, t0, 2U, 100000LL,
                    LINK_GONE_STAGE_TIMEOUT_US);
    link_flow_mark_sent(&flow, t0);
    link_flow_mark_sent(&flow, t0 + 100000LL);
    /* 次数预算用尽：立即明确放弃，不无限重发。 */
    assert(link_flow_poll(&flow, t0 + 200000LL) == LINK_FLOW_ACTION_GIVE_UP);

    /* 截止时间到点同样放弃。 */
    link_flow_reset(&flow);
    link_flow_start(&flow, DUAL_MESSAGE_DEVICE_GONE, 2U, t0, 100U, 100LL, 5000LL);
    link_flow_mark_sent(&flow, t0);
    assert(link_flow_poll(&flow, t0 + 5000LL) == LINK_FLOW_ACTION_GIVE_UP);
}

static void test_link_recovery_shared_budget(void)
{
    const int64_t start = 2000000;
    /* 无起点时按本阶段期望窗口计时。 */
    assert(link_recovery_stage_timeout_us(0, start, 5000000LL) == 5000000LL);
    assert(!link_recovery_budget_exhausted(0, start));

    /* 预算充足：本阶段拿走自己的窗口。 */
    assert(link_recovery_stage_timeout_us(start, start, 5000000LL) == 5000000LL);
    /* 预算不足：本阶段被裁剪到剩余预算，不串联第二个完整 5 秒等待。 */
    assert(link_recovery_stage_timeout_us(
        start, start + LINK_RECOVERY_BUDGET_US - 2000000LL, 5000000LL) ==
        2000000LL);
    assert(link_recovery_stage_timeout_us(start, start + LINK_RECOVERY_BUDGET_US,
                                          5000000LL) == 0);
    assert(link_recovery_budget_exhausted(start, start + LINK_RECOVERY_BUDGET_US));
    assert(!link_recovery_budget_exhausted(start,
        start + LINK_RECOVERY_BUDGET_US - 1));
}

static void test_link_transaction_dedup_rules(void)
{
    /* 无历史：全新事务。 */
    assert(link_transaction_classify(false, 0U, 0U, false, 9U, 100U) ==
           LINK_DUP_NEW);
    /* 同一 generation、同一 ID：进行中或已完成。 */
    assert(link_transaction_classify(true, 9U, 100U, false, 9U, 100U) ==
           LINK_DUP_IN_FLIGHT);
    assert(link_transaction_classify(true, 9U, 100U, true, 9U, 100U) ==
           LINK_DUP_COMPLETED);
    /* 更新的 ID：新事务。 */
    assert(link_transaction_classify(true, 9U, 100U, true, 9U, 101U) ==
           LINK_DUP_NEW);
    /* 更旧的 ID：忽略，不能撤销新会话。 */
    assert(link_transaction_classify(true, 9U, 100U, true, 9U, 99U) ==
           LINK_DUP_SUPERSEDED);
    /* 对端重启：旧 ID 不再可比，一律当新事务。 */
    assert(link_transaction_classify(true, 9U, 100U, true, 10U, 5U) ==
           LINK_DUP_NEW);

    /* 同一非零事件仍在途时，新的拔出必须沿用原 event ID。 */
    assert(link_gone_keeps_existing_event(true, false, 100U));
    assert(!link_gone_keeps_existing_event(true, true, 100U));
    assert(!link_gone_keeps_existing_event(false, false, 100U));
    assert(!link_gone_keeps_existing_event(true, false, 0U));
}

static void test_link_late_ack_and_release_plan(void)
{
    /* 迟到但双方 generation 与 event ID 都匹配的确认仍然有效。 */
    assert(link_late_ack_matches_completed(true, 5U, 77U, false, 5U, 77U));
    assert(!link_late_ack_matches_completed(true, 5U, 77U, true, 5U, 77U));
    assert(!link_late_ack_matches_completed(true, 5U, 77U, false, 6U, 77U));
    assert(!link_late_ack_matches_completed(true, 5U, 77U, false, 5U, 78U));
    assert(!link_late_ack_matches_completed(false, 5U, 77U, false, 5U, 77U));

    /* 快速插回不允许废旧屏障：屏障未完成时禁止 OFFER。 */
    assert(!dual_disconnect_barrier_allows_profile_offer(true, false));
    assert(!dual_disconnect_barrier_allows_profile_offer(false, true));
    assert(dual_disconnect_barrier_allows_profile_offer(false, false));

    /* 输入错误 vs 真实断开：前者不得触发 GONE/清理/重枚举。 */
    const link_release_plan_t input_error = link_release_plan(false);
    assert(input_error.release_inputs);
    assert(input_error.record_input_error);
    assert(!input_error.mark_usb_disconnected);
    assert(!input_error.cancel_cached_profile);
    assert(!input_error.start_device_gone);

    const link_release_plan_t device_gone = link_release_plan(true);
    assert(device_gone.release_inputs);
    assert(!device_gone.record_input_error);
    assert(device_gone.mark_usb_disconnected);
    assert(device_gone.cancel_cached_profile);
    assert(device_gone.start_device_gone);
}

static void test_link_failure_classification_and_cleanup_policy(void)
{
    assert(link_failure_is_retryable(LINK_CAUSE_NOT_ENUMERATED));
    assert(link_failure_is_retryable(LINK_CAUSE_QUEUE_BUSY));
    assert(link_failure_is_retryable(LINK_CAUSE_USB_TEARDOWN_FAILED));
    assert(link_failure_is_retryable(LINK_CAUSE_USB_INSTALL_FAILED));
    assert(link_failure_is_retryable(LINK_CAUSE_ACK_TIMEOUT));
    /* 不可克隆/不支持描述符必须与超时区分，且不作为可重试原因。 */
    assert(!link_failure_is_retryable(LINK_CAUSE_UNSUPPORTED_DESCRIPTOR));
    assert(!link_failure_is_retryable(LINK_CAUSE_INVALID_STATE));
    assert(!link_failure_is_retryable(LINK_CAUSE_NONE));
    assert(strcmp(link_failure_cause_name(LINK_CAUSE_UNSUPPORTED_DESCRIPTOR),
                  link_failure_cause_name(LINK_CAUSE_ACK_TIMEOUT)) != 0);
    assert(strcmp(link_failure_cause_name(LINK_CAUSE_NOT_ENUMERATED),
                  "not_enumerated") == 0);

    /* 清理失败保持门关闭并有界重试；只有实际成功才允许成功确认。 */
    assert(link_cleanup_should_retry(0U, LINK_CLEANUP_MAX_ATTEMPTS));
    assert(link_cleanup_should_retry(LINK_CLEANUP_MAX_ATTEMPTS - 1U,
                                     LINK_CLEANUP_MAX_ATTEMPTS));
    assert(!link_cleanup_should_retry(LINK_CLEANUP_MAX_ATTEMPTS,
                                      LINK_CLEANUP_MAX_ATTEMPTS));
    assert(link_cleanup_result_is_accepted(LINK_CAUSE_NONE));
    assert(!link_cleanup_result_is_accepted(LINK_CAUSE_USB_TEARDOWN_FAILED));

    /* 清理已成功、只有 ACK 入队失败：只补发 ACK。 */
    assert(link_ack_retry_skips_side_effects(true, true));
    assert(!link_ack_retry_skips_side_effects(false, true));
    assert(!link_ack_retry_skips_side_effects(true, false));
}

static void test_profile_ack_session_binding(void)
{
    _Static_assert(DUAL_LINK_PROFILE_ACK_LENGTH == 17U,
                   "PROFILE_ACK必须携带双方generation");
    _Static_assert(DUAL_LINK_PROFILE_ACK_TRANSFER_ID_OFFSET == 0U,
                   "PROFILE_ACK transfer ID偏移回归");
    _Static_assert(DUAL_LINK_PROFILE_ACK_CRC32_OFFSET == 4U,
                   "PROFILE_ACK CRC偏移回归");
    _Static_assert(DUAL_LINK_PROFILE_ACK_STATUS_OFFSET == 8U,
                   "PROFILE_ACK status偏移回归");
    _Static_assert(DUAL_LINK_PROFILE_ACK_RECIPIENT_GENERATION_OFFSET == 9U,
                   "PROFILE_ACK目标generation偏移回归");
    _Static_assert(DUAL_LINK_PROFILE_ACK_SENDER_GENERATION_OFFSET + 4U ==
                   DUAL_LINK_PROFILE_ACK_LENGTH,
                   "PROFILE_ACK发送方generation必须落在末尾");

    /* 匹配：本地 generation、对端 generation 与活动传输三者一致。 */
    assert(link_profile_ack_is_for_session(11U, 22U, 11U, true, 22U, 5U, 0xABU, 5U, 0xABU));
    /* 旧会话的确认一律不匹配。 */
    assert(!link_profile_ack_is_for_session(10U, 22U, 11U, true, 22U, 5U, 0xABU, 5U, 0xABU));
    assert(!link_profile_ack_is_for_session(11U, 21U, 11U, true, 22U, 5U, 0xABU, 5U, 0xABU));
    assert(!link_profile_ack_is_for_session(11U, 22U, 11U, false, 22U, 5U, 0xABU, 5U, 0xABU));
    /* 非活动传输或不存在的 transfer ID 不匹配。 */
    assert(!link_profile_ack_is_for_session(11U, 22U, 11U, true, 22U, 4U, 0xABU, 5U, 0xABU));
    assert(!link_profile_ack_is_for_session(11U, 22U, 11U, true, 22U, 5U, 0xACU, 5U, 0xABU));
    assert(!link_profile_ack_is_for_session(11U, 22U, 11U, true, 22U, 0U, 0xABU, 0U, 0xABU));

    /* 只有匹配的成功确认才推进 M 的 HID 状态。 */
    assert(link_profile_ack_updates_hid_state(true, true));
    assert(!link_profile_ack_updates_hid_state(true, false));
    assert(!link_profile_ack_updates_hid_state(false, true));
}

static void test_profile_receiver_idempotent_replay(void)
{
    hid_device_profile_t source;
    make_c092_profile(&source);
    uint8_t blob[HID_PROFILE_MAX_BLOB] = {0};
    size_t length = 0;
    assert(hid_device_profile_serialize(&source, blob, sizeof(blob), &length));
    const uint32_t crc32 = hid_profile_crc32(blob, length);

    profile_capture_t capture = {0};
    hid_profile_receiver_t receiver;
    hid_profile_receiver_init(&receiver, capture_profile, &capture);
    feed_profile_frames(&receiver, blob, length, 21U, crc32);
    assert(capture.count == 1);
    assert(!receiver.last_commit_was_duplicate);

    /* 最终确认丢失后的等价重放：同一 transfer 再走一遍 BEGIN/CHUNK/COMMIT。 */
    feed_profile_frames(&receiver, blob, length, 21U, crc32);
    assert(capture.count == 1);
    assert(receiver.last_commit_was_duplicate);
    /* 重放标记保持：该 transfer 后续任何重复都继续幂等，直到出现新 transfer。 */
    assert(receiver.published_transfer_replay);
    assert(receiver.published_transfer_id == 21U);
    assert(receiver.published_crc32 == crc32);

    /* 重放期间的分片被幂等忽略，不会把已发布 Profile 丢掉。 */
    assert(hid_profile_receiver_has_profile(&receiver));
    assert(hid_profile_receiver_begin(&receiver, 21U, (uint32_t)length, crc32));
    assert(hid_profile_receiver_chunk(&receiver, 21U, 0U, blob,
                                      HID_PROFILE_MAX_CHUNK_DATA));
    assert(hid_profile_receiver_has_profile(&receiver));
    assert(hid_profile_receiver_commit(&receiver, 21U, (uint32_t)length, crc32));
    assert(receiver.last_commit_was_duplicate);
    assert(capture.count == 1);

    /* 新的 transfer 仍然是真正的新事务。 */
    feed_profile_frames(&receiver, blob, length, 22U, crc32);
    assert(capture.count == 2);
    assert(!receiver.last_commit_was_duplicate);
}

/* P 侧清理/确认重试的模型：与 pc_hid_output.c 的重配置任务使用同一组谓词。 */
typedef struct {
    uint8_t cleanup_attempts;
    uint8_t ack_attempts;
    bool cleanup_done;
    bool gate_closed;
    bool failed;
    uint32_t teardowns;
    uint32_t acks;
} p_cleanup_sim_t;

typedef enum {
    P_SIM_DONE = 0,
    P_SIM_RETRY_CLEANUP,
    P_SIM_RETRY_ACK,
    P_SIM_FAILED,
} p_sim_action_t;

static p_sim_action_t p_cleanup_step(
    p_cleanup_sim_t *sim, bool cleanup_fails, bool ack_fails)
{
    if (!sim->cleanup_done) {
        ++sim->teardowns;
        ++sim->cleanup_attempts;
        if (cleanup_fails) {
            sim->gate_closed = true;
            if (link_cleanup_should_retry(sim->cleanup_attempts,
                                          LINK_CLEANUP_MAX_ATTEMPTS)) {
                return P_SIM_RETRY_CLEANUP;
            }
            sim->failed = true;
            return P_SIM_FAILED;
        }
        /* 先登记清理结果：之后只允许补发确认帧。 */
        sim->cleanup_done = true;
    }
    if (ack_fails) {
        ++sim->ack_attempts;
        if (link_cleanup_should_retry(sim->ack_attempts,
                                      LINK_ACK_ENQUEUE_MAX_ATTEMPTS)) {
            return P_SIM_RETRY_ACK;
        }
        sim->failed = true;
        sim->gate_closed = true;
        return P_SIM_FAILED;
    }
    ++sim->acks;
    sim->cleanup_done = false;
    sim->cleanup_attempts = 0;
    sim->ack_attempts = 0;
    sim->failed = false;
    sim->gate_closed = false;
    return P_SIM_DONE;
}

static void test_injection_p_cleanup_failure_recovers(void)
{
    p_cleanup_sim_t sim = {0};
    /* 清理连续失败两次后成功：门一直关闭，成功后同一 generation 可恢复。 */
    assert(p_cleanup_step(&sim, true, false) == P_SIM_RETRY_CLEANUP);
    assert(sim.gate_closed && !sim.failed);
    assert(p_cleanup_step(&sim, true, false) == P_SIM_RETRY_CLEANUP);
    assert(sim.gate_closed && !sim.failed);
    assert(p_cleanup_step(&sim, false, false) == P_SIM_DONE);
    assert(!sim.gate_closed && !sim.failed);
    assert(sim.teardowns == 3U);
    assert(sim.acks == 1U);

    /* 重试预算耗尽：明确失败并保持门关闭，不永久拒绝同 generation 的恢复。 */
    p_cleanup_sim_t exhausted = {0};
    for (uint8_t attempt = 0; attempt < LINK_CLEANUP_MAX_ATTEMPTS - 1U; ++attempt) {
        assert(p_cleanup_step(&exhausted, true, false) == P_SIM_RETRY_CLEANUP);
    }
    assert(p_cleanup_step(&exhausted, true, false) == P_SIM_FAILED);
    assert(exhausted.gate_closed && exhausted.failed);
    /* 重新排队后可以再次成功，而不是永久拒绝。 */
    assert(p_cleanup_step(&exhausted, false, false) == P_SIM_DONE);
    assert(!exhausted.failed && !exhausted.gate_closed);
}

static void test_injection_cleanup_done_ack_enqueue_failure(void)
{
    p_cleanup_sim_t sim = {0};
    /* 清理成功但确认帧入队失败：只补发 ACK，不重复卸载 USB。 */
    assert(p_cleanup_step(&sim, false, true) == P_SIM_RETRY_ACK);
    assert(sim.teardowns == 1U);
    assert(p_cleanup_step(&sim, false, true) == P_SIM_RETRY_ACK);
    assert(sim.teardowns == 1U);
    assert(p_cleanup_step(&sim, false, false) == P_SIM_DONE);
    assert(sim.teardowns == 1U);
    assert(sim.acks == 1U);
    assert(link_ack_retry_skips_side_effects(true, true));
}

static void test_injection_gone_ack_loss_and_late_ack(void)
{
    link_flow_t gone;
    link_flow_reset(&gone);
    const int64_t t0 = 3000000;
    link_flow_start(&gone, DUAL_MESSAGE_DEVICE_GONE, 77U, t0,
                    LINK_GONE_MAX_ATTEMPTS, LINK_GONE_RETRY_INTERVAL_US,
                    LINK_GONE_STAGE_TIMEOUT_US);
    gone.peer_generation = 4U;
    link_flow_mark_sent(&gone, t0);

    /* 确认丢失：同一 event ID 有界重发，克隆门保持关闭。 */
    int sends = 1;
    int64_t now = t0 + LINK_GONE_RETRY_INTERVAL_US;
    while (link_flow_poll(&gone, now) == LINK_FLOW_ACTION_RESEND) {
        assert(gone.id == 77U);
        assert(gone.peer_generation == 4U);
        link_flow_mark_sent(&gone, now);
        ++sends;
        now += LINK_GONE_RETRY_INTERVAL_US;
    }
    assert(sends > 1);
    assert(sends <= (int)LINK_GONE_MAX_ATTEMPTS);
    assert(!dual_disconnect_barrier_allows_profile_offer(
        link_flow_incomplete(&gone), gone.state == LINK_FLOW_FAILED));

    /* 预算耗尽后明确放弃，但同一事件的迟到确认仍然有效。 */
    assert(link_flow_poll(&gone, t0 + LINK_GONE_STAGE_TIMEOUT_US) ==
           LINK_FLOW_ACTION_GIVE_UP);
    gone.state = LINK_FLOW_FAILED;
    assert(!dual_disconnect_barrier_allows_profile_offer(false, true));
    assert(link_late_ack_matches_completed(true, 4U, 77U, false, 4U, 77U));
    gone.state = LINK_FLOW_ACCEPTED;
    assert(dual_disconnect_barrier_allows_profile_offer(
        link_flow_incomplete(&gone), gone.state == LINK_FLOW_FAILED));
    /* 新事件不会清掉新连接状态：不同 event ID 不匹配旧屏障。 */
    assert(!link_late_ack_matches_completed(true, 4U, 77U, false, 4U, 78U));
}

static void test_injection_quick_replug_waits_for_barrier(void)
{
    link_flow_t gone;
    link_flow_t offer;
    link_flow_reset(&gone);
    link_flow_reset(&offer);
    const int64_t t0 = 4000000;

    /* 拔出：建立屏障，新的 Profile 先采集但只缓存等待。 */
    link_flow_start(&gone, DUAL_MESSAGE_DEVICE_GONE, 90U, t0,
                    LINK_GONE_MAX_ATTEMPTS, LINK_GONE_RETRY_INTERVAL_US,
                    LINK_GONE_STAGE_TIMEOUT_US);
    link_flow_mark_sent(&gone, t0);
    link_flow_start(&offer, DUAL_MESSAGE_PROFILE_OFFER, 91U, t0 + 1000,
                    LINK_FLOW_MAX_ATTEMPTS, LINK_FLOW_RETRY_INTERVAL_US,
                    LINK_PROFILE_STAGE_TIMEOUT_US);
    assert(link_flow_queued(&offer));
    assert(!dual_disconnect_barrier_allows_profile_offer(
        link_flow_incomplete(&gone), gone.state == LINK_FLOW_FAILED));
    /* 屏障未完成：OFFER 不能发出，事件身份不被新连接覆盖。 */
    assert(gone.id == 90U);
    assert(offer.id == 91U);

    /* 屏障完成后同一个 OFFER 事务才允许发出。 */
    assert(link_flow_ack(&gone, true));
    assert(dual_disconnect_barrier_allows_profile_offer(
        link_flow_incomplete(&gone), gone.state == LINK_FLOW_FAILED));
    assert(link_flow_poll(&offer, t0 + 2000) == LINK_FLOW_ACTION_SEND);
    assert(offer.id == 91U);
}

static void test_injection_generation_reset_invalidates_old_work(void)
{
    /* P 或 M 复位：旧 generation 的确认、Profile 与任务全部失效。 */
    assert(link_transaction_classify(true, 7U, 500U, true, 8U, 500U) ==
           LINK_DUP_NEW);
    assert(!link_profile_ack_is_for_session(8U, 7U, 8U, true, 9U, 3U, 1U, 3U, 1U));
    assert(link_profile_ack_is_for_session(8U, 9U, 8U, true, 9U, 3U, 1U, 3U, 1U));
    /* 新会话可以立刻建立新事务，不必等旧事务超时。 */
    link_flow_t flow;
    link_flow_reset(&flow);
    link_flow_start(&flow, DUAL_MESSAGE_PROFILE_REQUEST, 600U, 9000,
                    LINK_FLOW_MAX_ATTEMPTS, LINK_FLOW_RETRY_INTERVAL_US,
                    LINK_PROFILE_STAGE_TIMEOUT_US);
    assert(link_flow_poll(&flow, 9000) == LINK_FLOW_ACTION_SEND);
}

static void test_injection_request_dedup_and_not_enumerated(void)
{
    /* M 侧按 flow ID 去重：同一 flow 重放不重复采集。 */
    uint32_t seen_flow = 0;
    uint32_t collections = 0;
    bool device_present = false;
    bool waiting_device = false;
    for (int replay = 0; replay < 3; ++replay) {
        const uint32_t flow_id = 55U;
        if (flow_id == seen_flow) {
            /* 重复申请：只重发结果；设备到达后才补一次采集。 */
            if (waiting_device && device_present) {
                waiting_device = false;
                ++collections;
            }
            continue;
        }
        seen_flow = flow_id;
        if (!device_present) {
            waiting_device = true;
            continue;
        }
        ++collections;
    }
    /* 物理鼠标尚未枚举：受理并等待，不判永久失败、也不重复采集。 */
    assert(waiting_device);
    assert(collections == 0U);
    device_present = true;
    if (waiting_device && device_present) {
        waiting_device = false;
        ++collections;
    }
    assert(collections == 1U);
    assert(!waiting_device);
}

static void test_injection_duplicate_frames_single_reenumeration(void)
{
    hid_device_profile_t source;
    make_c092_profile(&source);
    uint8_t blob[HID_PROFILE_MAX_BLOB] = {0};
    size_t length = 0;
    assert(hid_device_profile_serialize(&source, blob, sizeof(blob), &length));
    const uint32_t crc32 = hid_profile_crc32(blob, length);

    profile_capture_t capture = {0};
    hid_profile_receiver_t receiver;
    hid_profile_receiver_init(&receiver, capture_profile, &capture);
    uint32_t installs = 0;

    /* 重复 OFFER/BEGIN/CHUNK/COMMIT：只允许一次发布，即一次重新枚举。 */
    for (int round = 0; round < 3; ++round) {
        feed_profile_frames(&receiver, blob, length, 31U, crc32);
        if (capture.count > installs) {
            installs = capture.count;
        }
    }
    assert(capture.count == 1U);
    assert(installs == 1U);
    assert(receiver.last_commit_was_duplicate);
}

static void test_injection_terminal_failure_is_distinguishable(void)
{
    /* 不支持的描述符/挂载失败：可区分终态、必须释放输入、不重试。 */
    const link_failure_cause_t terminal = LINK_CAUSE_UNSUPPORTED_DESCRIPTOR;
    assert(!link_failure_is_retryable(terminal));
    assert(strcmp(link_failure_cause_name(terminal), "unsupported_descriptor") == 0);
    const link_release_plan_t release = link_release_plan(true);
    assert(release.release_inputs);

    link_flow_t commit;
    link_flow_reset(&commit);
    link_flow_start(&commit, DUAL_MESSAGE_PROFILE_COMMIT, 70U, 1000,
                    LINK_COMMIT_MAX_ATTEMPTS, LINK_COMMIT_RETRY_INTERVAL_US,
                    LINK_PROFILE_STAGE_TIMEOUT_US);
    link_flow_mark_sent(&commit, 1000);
    /* NACK 是终态：不再重发，也不再整份重传。 */
    assert(link_flow_ack(&commit, false));
    assert(commit.state == LINK_FLOW_FAILED);
    assert(link_flow_poll(&commit, 1000 + LINK_PROFILE_STAGE_TIMEOUT_US) ==
           LINK_FLOW_ACTION_NONE);
}

static void test_injection_commit_replay_without_reenumeration(void)
{
    hid_device_profile_t source;
    make_c092_profile(&source);
    uint8_t blob[HID_PROFILE_MAX_BLOB] = {0};
    size_t length = 0;
    assert(hid_device_profile_serialize(&source, blob, sizeof(blob), &length));
    const uint32_t crc32 = hid_profile_crc32(blob, length);

    profile_capture_t capture = {0};
    hid_profile_receiver_t receiver;
    hid_profile_receiver_init(&receiver, capture_profile, &capture);
    feed_profile_frames(&receiver, blob, length, 41U, crc32);
    assert(capture.count == 1);

    /* M 丢失最终确认后重放同一 transfer 的 COMMIT：只补结果。 */
    link_flow_t commit;
    link_flow_reset(&commit);
    link_flow_start(&commit, DUAL_MESSAGE_PROFILE_COMMIT, 41U, 1000,
                    LINK_COMMIT_MAX_ATTEMPTS, LINK_COMMIT_RETRY_INTERVAL_US,
                    LINK_COMMIT_STAGE_TIMEOUT_US);
    link_flow_mark_sent(&commit, 1000);
    assert(link_flow_poll(&commit, 1000 + LINK_COMMIT_RETRY_INTERVAL_US) ==
           LINK_FLOW_ACTION_RESEND);
    link_flow_mark_sent(&commit, 1000 + LINK_COMMIT_RETRY_INTERVAL_US);

    assert(hid_profile_receiver_commit(&receiver, 41U, (uint32_t)length, crc32));
    assert(receiver.last_commit_was_duplicate);
    assert(capture.count == 1);
    /* 只重放 COMMIT（M 的等价重放路径）同样幂等。 */
    assert(hid_profile_receiver_commit(&receiver, 41U, (uint32_t)length, crc32));
    assert(receiver.last_commit_was_duplicate);
    assert(capture.count == 1);
    /* 补发最终确认后事务结束，不再重放。 */
    assert(link_flow_ack(&commit, true));
    assert(link_flow_poll(&commit, 1000 + LINK_COMMIT_STAGE_TIMEOUT_US) ==
           LINK_FLOW_ACTION_NONE);
}

static void test_profile_final_ack_requires_mounted_clone(void)
{
    const uint32_t transfer = 41U;
    const uint32_t crc = 0x01234567U;
    /* 已发布数据或安装中的重复 COMMIT 都只能得到接收确认。 */
    assert(link_profile_replay_result(transfer, crc, 0U, 0U, 0U, 0U,
                                      false, false, false, true, false) ==
           LINK_PROFILE_REPLAY_PENDING);
    assert(link_profile_replay_result(transfer, crc, transfer, crc, 0U, 0U,
                                      true, true, false, true, false) ==
           LINK_PROFILE_REPLAY_PENDING);
    /* 身份相同但挂载尚未完成，或旧 epoch 正在清理，不能声称成功。 */
    assert(link_profile_replay_result(transfer, crc, transfer, crc, 0U, 0U,
                                      true, true, false, true, true) ==
           LINK_PROFILE_REPLAY_WAIT_HOST);
    assert(link_profile_replay_result(transfer, crc, transfer, crc, 0U, 0U,
                                      true, true, true, true, false) ==
           LINK_PROFILE_REPLAY_PENDING);
    assert(link_profile_replay_result(transfer, crc, transfer, crc, 0U, 0U,
                                      true, true, true, false, false) ==
           LINK_PROFILE_REPLAY_MOUNTED);
    assert(link_profile_replay_result(transfer, crc ^ 1U, transfer, crc, 0U, 0U,
                                      true, true, true, false, false) ==
           LINK_PROFILE_REPLAY_PENDING);
    /* 两次挂载均失败后的同一 transfer 重放必须补发失败确认。 */
    assert(link_profile_replay_result(transfer, crc, 0U, 0U, transfer, crc,
                                      false, false, false, false, false) ==
           LINK_PROFILE_REPLAY_FAILED);
    assert(link_profile_replay_result(transfer + 1U, crc, 0U, 0U, transfer, crc,
                                      false, false, false, false, false) ==
           LINK_PROFILE_REPLAY_PENDING);
    assert(link_profile_ack_is_wait_host(DUAL_PROFILE_ACK_STATUS_WAIT_HOST));
    assert(!link_profile_ack_is_wait_host(DUAL_PROFILE_ACK_STATUS_MOUNTED));
    assert(!link_commit_poll_enabled(true));
    assert(link_commit_poll_enabled(false));

    /* 主机即使在原 3 秒窗口后才配置，已安装克隆仍等待；仅 mounted 可最终成功。 */
    for (unsigned elapsed_ms = 0U; elapsed_ms < 3100U; ++elapsed_ms) {
        assert(link_profile_host_wait_action(true, false, true) ==
               LINK_PROFILE_HOST_WAIT);
    }
    assert(link_profile_host_wait_action(true, true, true) ==
           LINK_PROFILE_HOST_MOUNTED);
    assert(link_profile_host_wait_action(true, false, false) ==
           LINK_PROFILE_HOST_WAIT_CANCEL);
    assert(link_profile_host_wait_action(false, false, true) ==
           LINK_PROFILE_HOST_WAIT_CANCEL);
    assert(!link_profile_host_probe_due(true, 1000,
        1000 + LINK_COMMIT_HOST_PROBE_INTERVAL_US - 1));
    assert(link_profile_host_probe_due(true, 1000,
        1000 + LINK_COMMIT_HOST_PROBE_INTERVAL_US));
    assert(!link_profile_host_probe_due(false, 1000,
        1000 + 10 * LINK_COMMIT_HOST_PROBE_INTERVAL_US));

    link_flow_t commit;
    link_flow_reset(&commit);
    link_flow_start(&commit, DUAL_MESSAGE_PROFILE_COMMIT, transfer, 1000,
                    LINK_COMMIT_MAX_ATTEMPTS, LINK_COMMIT_RETRY_INTERVAL_US,
                    LINK_COMMIT_STAGE_TIMEOUT_US);
    link_flow_mark_sent(&commit, 1000);
    assert(link_flow_poll(&commit, 1000 + 6500000LL) == LINK_FLOW_ACTION_RESEND);
    assert(link_flow_poll(&commit, 1000 + LINK_COMMIT_STAGE_TIMEOUT_US) ==
           LINK_FLOW_ACTION_GIVE_UP);
}

static void test_profile_mount_retry_and_epoch_cancel(void)
{
    /* 第一轮挂载超时才允许保留同一 Profile 再装一次。 */
    assert(link_profile_mount_should_retry(true, 1U, true));
    assert(!link_profile_mount_should_retry(false, 1U, true));
    assert(!link_profile_mount_should_retry(true, 2U, true));
    assert(!link_profile_mount_should_retry(true, 1U, false));
    assert(!link_profile_mount_should_retry(true, 0U, true));

    const uint32_t epoch = 7U;
    assert(dual_profile_operation_is_current(epoch, epoch, false));
    assert(link_profile_mount_should_retry(
        true, 1U, dual_profile_operation_is_current(epoch, epoch, false)));
    /* DEVICE_GONE/新 peer 更新 epoch 时，旧安装不能进入第二次尝试。 */
    assert(!link_profile_mount_should_retry(
        true, 1U, dual_profile_operation_is_current(epoch, epoch + 1U, false)));
    assert(!link_profile_mount_should_retry(
        true, 1U, dual_profile_operation_is_current(epoch, epoch, true)));

    /* 迟到鼠标的新 Profile 从新的触发时间重新获得 COMMIT 窗口。 */
    const int64_t old_trigger = 1000000LL;
    const int64_t new_trigger = old_trigger + LINK_RECOVERY_BUDGET_US + 1000000LL;
    assert(link_recovery_stage_timeout_us(old_trigger, new_trigger,
        LINK_COMMIT_STAGE_TIMEOUT_US) == 0LL);
    assert(link_recovery_stage_timeout_us(new_trigger, new_trigger,
        LINK_COMMIT_STAGE_TIMEOUT_US) == LINK_COMMIT_STAGE_TIMEOUT_US);
}

/*
 * Profile CRC 复用判定（2026-09-28）：只有"克隆确实挂载 + 无在途操作 + CRC 完全一致"
 * 才允许跳过卸载+重装；任何一条不成立都必须退回完整路径。
 */
static void test_profile_reuse_predicate(void)
{
    const uint32_t installed = 0x85A85778U;
    /* 基准：一切正常且 CRC 一致 → 允许复用。 */
    assert(link_profile_reuse_allowed(true, true, true, false, false, false, false,
                                      installed, installed));
    /* CRC 不同（换了鼠标/Profile 变了）→ 必须完整卸载+重装。 */
    assert(!link_profile_reuse_allowed(true, true, true, false, false, false, false,
                                       installed ^ 1U, installed));
    /* 当前没有已安装 Profile（P 刚重启过，缓存为空）→ 完整路径。 */
    assert(!link_profile_reuse_allowed(true, true, true, false, false, false, false,
                                       installed, 0U));
    /* CRC32 为 0 的提议（离线注入 transfer=0）永不视为命中。 */
    assert(!link_profile_reuse_allowed(true, true, true, false, false, false, false,
                                       0U, 0U));
    /* 克隆未活动 / 未真正挂载 → 完整路径（避免把陈旧或已卸载的克隆留下）。 */
    assert(!link_profile_reuse_allowed(true, false, true, false, false, false, false,
                                       installed, installed));
    assert(!link_profile_reuse_allowed(true, true, false, false, false, false, false,
                                       installed, installed));
    /* 有在途操作（安装请求、安装中、卸载请求、卸载失败）→ 完整路径。 */
    assert(!link_profile_reuse_allowed(true, true, true, true, false, false, false,
                                       installed, installed));
    assert(!link_profile_reuse_allowed(true, true, true, false, true, false, false,
                                       installed, installed));
    assert(!link_profile_reuse_allowed(true, true, true, false, false, true, false,
                                       installed, installed));
    assert(!link_profile_reuse_allowed(true, true, true, false, false, false, true,
                                       installed, installed));
    /* 重配置通道未启用 → 不复用。 */
    assert(!link_profile_reuse_allowed(false, true, true, false, false, false, false,
                                       installed, installed));
}

static void test_usb_stall_watch_bounds(void)
{
    const int64_t now = 20000000LL;
    /* 没有 last_input 参数：静止或尚未产生过输入报告不会阻断心跳判定。 */
    assert(usb_stall_evidence_ready(now, now - 10000000, now - 250000, true, false));
    assert(!usb_stall_evidence_ready(now, now - 10000000, now - 249999, true, false));
    assert(!usb_stall_evidence_ready(now, now - 10000000, 0, true, false));
    assert(!usb_stall_evidence_ready(now, now - 9999999, now - 250000, true, false));
    assert(!usb_stall_evidence_ready(now, now - 10000000, now - 250000, false, false));
    assert(!usb_stall_evidence_ready(now, now - 10000000, now - 250000, true, true));
    assert(usb_stall_cooldown_ready(50000000, 20000000));
    assert(!usb_stall_cooldown_ready(49999999, 20000000));
    assert(usb_stall_cooldown_ready(now, 0));
    assert(usb_stall_observed_delay(now, now - 250000) == 250000);

    /* 心跳快照必须剔除普通 EP0 和尚未成功提交的 URB。 */
    assert(vendor_urb_pending_matches_snapshot(true, true, true));
    assert(!vendor_urb_pending_matches_snapshot(true, false, true));
    assert(vendor_urb_pending_matches_snapshot(true, false, false));
    assert(!vendor_urb_pending_matches_snapshot(false, true, true));

    /* 每个心跳相位覆盖 20 ms 轮询下的触发边界；不代表真实调度延迟。 */
    for (int phase = 0; phase <= 100000; phase += 1000) {
        const int64_t heartbeat_start = 20000000;
        int64_t fired = 0;
        for (int64_t elapsed = 0; elapsed <= 500000; elapsed += 20000) {
            if (elapsed >= phase && usb_stall_evidence_ready(
                    heartbeat_start + elapsed, 1,
                    heartbeat_start + phase, true, false)) {
                fired = elapsed;
                break;
            }
        }
        const int64_t observed_age = fired - phase;
        assert(observed_age >= 250000 && observed_age < 270000);
    }
    puts("usb_stall_watch_bounds: PASS (heartbeat-only logic)");
}

static void test_vendor_session_barrier_protocol(void)
{
    uint8_t payload[DUAL_LINK_VENDOR_SESSION_LENGTH] = {0};
    uint32_t p_generation = 0U;
    uint32_t m_generation = 0U;
    uint32_t epoch = 0U;
    assert(dual_vendor_session_encode(11U, 22U, 33U, payload, sizeof(payload)));
    assert(dual_vendor_session_decode(payload, sizeof(payload),
                                      &p_generation, &m_generation, &epoch));
    assert(p_generation == 11U && m_generation == 22U && epoch == 33U);
    assert(link_vendor_session_identity_matches(11U, 22U, 11U, 22U, 33U));
    assert(!link_vendor_session_identity_matches(10U, 22U, 11U, 22U, 33U));
    assert(!link_vendor_session_identity_matches(11U, 21U, 11U, 22U, 33U));
    assert(link_vendor_session_epoch_is_newer(2U, 1U));
    assert(link_vendor_session_epoch_is_newer(1U, UINT32_MAX));
    assert(!link_vendor_session_epoch_is_newer(1U, 2U));
    assert(!dual_vendor_session_encode(0U, 22U, 33U, payload, sizeof(payload)));
    assert(!dual_vendor_session_decode(payload, sizeof(payload) - 1U,
                                       &p_generation, &m_generation, &epoch));
    /* BEGIN 只使控制帧 generation 失效，物理输入/按钮边沿沿原队列保留。 */
    assert(uart1_uses_vendor_input_generation(DUAL_MESSAGE_RAW_HID_INPUT));
    assert(!uart1_uses_vendor_control_generation(DUAL_MESSAGE_RAW_HID_INPUT));
    assert(uart1_uses_vendor_control_generation(DUAL_MESSAGE_HID_SET_REPORT));
    assert(uart1_uses_vendor_control_generation(DUAL_MESSAGE_VENDOR_CONTROL_REQUEST));
    assert(!uart1_uses_vendor_control_generation(DUAL_MESSAGE_VENDOR_SESSION_BEGIN));
    assert(uart1_is_retryable_vendor_session_barrier(DUAL_MESSAGE_VENDOR_SESSION_BEGIN));
    assert(uart1_is_retryable_vendor_session_barrier(DUAL_MESSAGE_VENDOR_SESSION_ACK));
    assert(!uart1_is_retryable_vendor_session_barrier(DUAL_MESSAGE_RAW_HID_INPUT));
    assert(hid_vendor_session_ack_matches(11U, 22U, 33U, 11U, 22U, 33U));
    assert(!hid_vendor_session_ack_matches(11U, 22U, 33U, 11U, 22U, 34U));
    assert(!hid_vendor_session_ack_matches(11U, 22U, 33U, 11U, 0U, 33U));
    assert(hid_vendor_session_begin_retry_due(true, false, 0U, 10U, 0, 1000));
    assert(!hid_vendor_session_begin_retry_due(true, false, 1U, 10U, 1000, 100999));
    assert(hid_vendor_session_begin_retry_due(true, false, 1U, 10U, 1000, 101000));
    assert(!hid_vendor_session_begin_retry_due(true, false, 10U, 10U, 0, 200000));
    assert(!hid_vendor_session_begin_retry_due(true, true, 1U, 10U, 0, 200000));
    assert(!hid_vendor_session_begin_retry_due(false, false, 0U, 10U, 0, 200000));
    assert(hid_vendor_session_pending_matches(51U, 7U, 51U, 7U));
    assert(!hid_vendor_session_pending_matches(52U, 8U, 51U, 7U));
    assert(!hid_vendor_session_pending_matches(0U, 7U, 0U, 7U));

    /* 主机刚完成 SET_CONFIGURATION 时，worker尚未轮询到挂载状态；
     * 只允许当前已安装克隆的首个厂商请求进入ACK门控。 */
    assert(hid_vendor_session_control_entry_ready(
        true, true, true, true, true, false, false, 9U, 9U));
    assert(hid_vendor_session_control_entry_ready(
        true, true, true, true, false, true, false, 9U, 9U));
    assert(hid_vendor_session_control_entry_ready(
        true, true, true, false, false, false, false, 8U, 3U));
    assert(!hid_vendor_session_control_entry_ready(
        true, true, false, true, true, false, false, 9U, 9U));
    assert(!hid_vendor_session_control_entry_ready(
        false, true, true, true, true, false, false, 9U, 9U));
    assert(!hid_vendor_session_control_entry_ready(
        true, false, true, true, true, false, false, 9U, 9U));
    assert(!hid_vendor_session_control_entry_ready(
        true, true, true, true, true, false, true, 9U, 9U));
    assert(!hid_vendor_session_control_entry_ready(
        true, true, true, true, true, false, false, 10U, 9U));
    assert(!hid_vendor_session_control_entry_ready(
        true, true, true, true, false, false, false, 9U, 9U));
}


static unsigned route_a5_count, route_v4_count;
static void capture_routed_a5(const dual_frame_t *frame, void *context)
{
    (void)frame; (void)context; ++route_a5_count;
}
static void capture_routed_v4(const makcu_v4_command_t *command, void *context)
{
    (void)command; (void)context; ++route_v4_count;
}
static void test_uart0_protocol_isolation(void)
{
    uart0_protocol_router_t router;
    uart0_protocol_router_init(&router, capture_routed_a5, capture_routed_v4, NULL);
    route_a5_count = route_v4_count = 0;
    dual_frame_t frame = {.version=2, .type=DUAL_MESSAGE_DIAG_REPORT_INJECT_REQUEST};
    const uint8_t nested[] = "km.move(10,0)\r";
    frame.payload_length = sizeof(nested)-1;
    memcpy(frame.payload, nested, frame.payload_length);
    uint8_t bytes[264]; size_t length = 0;
    assert(dual_frame_serialize(&frame, bytes, sizeof(bytes), &length) == ESP_OK);
    for (size_t i=0; i<length; ++i) uart0_protocol_router_feed(&router, bytes+i, 1, 1);
    assert(route_a5_count == 1 && route_v4_count == 0);
    bytes[length-1] ^= 1;
    uart0_protocol_router_feed(&router, bytes, length, 2);
    assert(route_a5_count == 1 && route_v4_count == 0);
    bytes[length-1] ^= 1;
    uint8_t v4[80]; size_t v4_length = 0;
    assert(makcu_v4_encode_frame(0x02, bytes, (uint16_t)length, v4, sizeof(v4), &v4_length));
    uart0_protocol_router_feed(&router, v4, v4_length, 3);
    assert(route_a5_count == 1 && route_v4_count == 1);
    const uint8_t incomplete[] = {0xA5,0x5A,2,2,0,0,64};
    uart0_protocol_router_feed(&router, incomplete, sizeof(incomplete), 4);
    const uint8_t query[] = "km.interpolate()\r";
    uart0_protocol_router_feed(&router, query, sizeof(query)-1, 254);
    assert(route_v4_count == 2 && route_a5_count == 1);
    memset(bytes, 0, sizeof(bytes));
    bytes[0]=0xA5; bytes[1]=0x5A; bytes[2]=2; bytes[6]=255;
    memcpy(bytes+7, nested, sizeof(nested)-1);
    uart0_protocol_router_feed(&router, bytes, sizeof(bytes), 300);
    assert(route_v4_count == 2 && route_a5_count == 1);
    uart0_protocol_router_feed(&router, query, sizeof(query)-1, 301);
    assert(route_v4_count == 3);
}

static void test_m_udp_channel_and_single_smoothing(void)
{
    for (uint8_t slots=0; slots<=20; slots+=5) {
        m_udp_smoothing_t state = {0};
        dual_frame_t in = {.version=2, .type=DUAL_MESSAGE_MOUSE_REPORT, .payload_length=8};
        in.payload[0]=1; in.payload[1]=100; in.payload[3]=0x9C; in.payload[4]=0xFF;
        in.payload[5]=10; in.payload[7]=slots;
        m_udp_smoothing_enqueue(&state, &in);
        int total_x=0,total_y=0,total_wheel=0; unsigned reports=0;
        for (int tick=1; tick<=25; ++tick) {
            dual_frame_t out;
            if (m_udp_smoothing_tick(&state,tick*1000,&out)) {
                assert(out.payload_length == 8 && out.payload[7] == 0);
                assert(out.payload[0] == 1);
                total_x += (int16_t)((uint16_t)out.payload[1] | ((uint16_t)out.payload[2]<<8));
                total_y += (int16_t)((uint16_t)out.payload[3] | ((uint16_t)out.payload[4]<<8));
                total_wheel += (int8_t)out.payload[5]; ++reports;
                assert(!m_udp_smoothing_tick(&state,tick*1000+100,&out));
            }
        }
        assert(total_x == 100 && total_y == -100 && total_wheel == 10);
        assert(reports == (slots == 0 ? 1 : slots));
        m_udp_smoothing_enqueue(&state,&in);
        in.payload[7]=5; m_udp_smoothing_enqueue(&state,&in);
        total_x=0;
        for (int tick=26; tick<=50; ++tick) {
            dual_frame_t out;
            if (m_udp_smoothing_tick(&state,tick*1000,&out))
                total_x += (int16_t)((uint16_t)out.payload[1] | ((uint16_t)out.payload[2]<<8));
        }
        assert(total_x == 200);
        m_udp_smoothing_enqueue(&state,&in);
        m_udp_smoothing_reset(&state);
        dual_frame_t out;
        for (int tick=51; tick<=75; ++tick)
            assert(!m_udp_smoothing_tick(&state,tick*1000,&out));
    }
}

int main(void)
{
    test_uart0_protocol_isolation();
    test_m_udp_channel_and_single_smoothing();
    test_merge_and_independent_release();
    test_rolling_smoother_preserves_overlapping_500hz_motion();
    test_saturation_is_split_without_loss();
    test_peek_commit_preserves_pending_on_retry();
    test_bridge_protocol();
    test_makcu_ascii_parser_and_session();
    test_makcu_v4_parsers_and_state();
    test_cdc_parser_and_lease_release();
    test_profile_model_and_roundtrip();
    test_profile_size_boundaries();
    test_profile_stream_fairness();
    test_dynamic_clone_descriptor_builder();
    test_profile_receiver_state_machine();
    test_runtime_scheduling_guards();
    test_dynamic_mouse_report_layout();
    test_status_led_logic();
    test_usb_role_claim_complement();
    test_peer_profile_invalidation();
    test_vendor_session_invalidates_inflight_work();
    test_device_gone_barrier_and_reconfigure_epoch();
    test_link_flow_transaction_model();
    test_link_flow_retry_budget_exhaustion();
    test_link_recovery_shared_budget();
    test_link_transaction_dedup_rules();
    test_link_late_ack_and_release_plan();
    test_link_failure_classification_and_cleanup_policy();
    test_profile_ack_session_binding();
    test_profile_receiver_idempotent_replay();
    test_injection_p_cleanup_failure_recovers();
    test_injection_cleanup_done_ack_enqueue_failure();
    test_injection_gone_ack_loss_and_late_ack();
    test_injection_quick_replug_waits_for_barrier();
    test_injection_generation_reset_invalidates_old_work();
    test_injection_request_dedup_and_not_enumerated();
    test_injection_duplicate_frames_single_reenumeration();
    test_injection_terminal_failure_is_distinguishable();
    test_injection_commit_replay_without_reenumeration();
    test_profile_final_ack_requires_mounted_clone();
    test_profile_mount_retry_and_epoch_cancel();
    test_profile_reuse_predicate();
    test_vendor_session_barrier_protocol();
    test_usb_stall_watch_bounds();
    puts("dual_proxy_logic_test: PASS");
    return 0;
}
