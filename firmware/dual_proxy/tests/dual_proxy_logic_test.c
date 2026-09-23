#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bridge_protocol.h"
#include "dual_input_aggregator.h"
#include "hid_device_profile.h"
#include "hid_clone_descriptor.h"
#include "hid_report_layout.h"
#include "mouse_motion_smoother.h"
#include "dual_proxy_runtime_config.h"
#include "dual_status_led_logic.h"
#include "usb_cdc_control_logic.h"

_Static_assert(DUAL_PROXY_REQUIRED_FREERTOS_HZ == 1000U, "FreeRTOS tick必须保持1000Hz");
_Static_assert(DUAL_PROXY_LINK_TX_BATCH_LIMIT > 0U, "UART1批量上限回归保护失败");
_Static_assert(DUAL_PROXY_HID_PERIOD_US == 1000U, "HID周期必须保持1000us");
_Static_assert(MOUSE_MOTION_SMOOTHING_SLOTS == 5U, "软件移动必须保持5个1ms槽");
_Static_assert(DUAL_MESSAGE_PROFILE_BEGIN == 0x24, "ProfileBegin消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_PROFILE_CHUNK == 0x25, "ProfileChunk消息类型回归保护失败");
_Static_assert(DUAL_MESSAGE_PROFILE_COMMIT == 0x26, "ProfileCommit消息类型回归保护失败");
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
    assert(hid_profile_receiver_get_profile(&receiver)->report_descriptor_count == 2);

    /* A duplicate chunk, wrong transfer id, or bad commit discards the transfer. */
    assert(hid_profile_receiver_begin(&receiver, 9, (uint32_t)length, crc32));
    assert(hid_profile_receiver_chunk(&receiver, 9, 0, blob, 3));
    assert(!hid_profile_receiver_chunk(&receiver, 9, 0, blob, 1));
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
    assert(!hid_mouse_report_apply_overlay(
        &layout, report, sizeof(report) - 1U, 0, 0, 0, 0, 0));
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
    test_rolling_smoother_preserves_overlapping_500hz_motion();
    test_saturation_is_split_without_loss();
    test_peek_commit_preserves_pending_on_retry();
    test_bridge_protocol();
    test_cdc_parser_and_lease_release();
    test_profile_model_and_roundtrip();
    test_profile_size_boundaries();
    test_profile_stream_fairness();
    test_dynamic_clone_descriptor_builder();
    test_profile_receiver_state_machine();
    test_runtime_scheduling_guards();
    test_dynamic_mouse_report_layout();
    test_status_led_logic();
    puts("dual_proxy_logic_test: PASS");
    return 0;
}
