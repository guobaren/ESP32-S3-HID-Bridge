#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define DUAL_PROXY_PROTOCOL_VERSION 2
#define DUAL_PROXY_MAX_PAYLOAD 64
#define DUAL_LINK_USB_STATE_HID_CONNECTED 2U

typedef enum {
    DUAL_MESSAGE_KEYBOARD_REPORT = 0x01,
    DUAL_MESSAGE_MOUSE_REPORT = 0x02,
    DUAL_MESSAGE_RELEASE_ALL = 0x03,
    DUAL_MESSAGE_PING = 0x04,
    DUAL_MESSAGE_SESSION_START = 0x05,
    DUAL_MESSAGE_DEVICE_PROBE = 0x06,
    DUAL_MESSAGE_DEVICE_HELLO = 0x07,
    /* 板载日志下载：主机协议侧命令，不参与输入租约。 */
    DUAL_MESSAGE_LOG_READ_REQUEST = 0x08,
    DUAL_MESSAGE_LOG_READ_RESPONSE = 0x09,
    DUAL_MESSAGE_LOG_CLEAR_REQUEST = 0x0A,
    /* 流式下载：一条请求换来连续多个 LOG_READ_RESPONSE，避免逐块往返。 */
    DUAL_MESSAGE_LOG_DUMP_REQUEST = 0x0B,
    /* 运行时暂停/恢复板载写盘（payload 1 字节：0=恢复，1=暂停）。 */
    DUAL_MESSAGE_LOG_CONTROL_REQUEST = 0x0C,
    DUAL_MESSAGE_LINK_HELLO = 0x20,
    DUAL_MESSAGE_PHYSICAL_MOUSE = 0x21,
    DUAL_MESSAGE_PHYSICAL_RELEASE = 0x22,
    DUAL_MESSAGE_LINK_PING = 0x23,
    DUAL_MESSAGE_PROFILE_BEGIN = 0x24,
    DUAL_MESSAGE_PROFILE_CHUNK = 0x25,
    DUAL_MESSAGE_PROFILE_COMMIT = 0x26,
    DUAL_MESSAGE_RAW_HID_INPUT = 0x27,
    DUAL_MESSAGE_HID_SET_REPORT = 0x28,
    DUAL_MESSAGE_HID_GET_REPORT_REQUEST = 0x29,
    DUAL_MESSAGE_HID_GET_REPORT_RESPONSE = 0x2A,
    DUAL_MESSAGE_SOFTWARE_MOUSE = 0x2B,
    DUAL_MESSAGE_SOFTWARE_RELEASE = 0x2C,
    DUAL_MESSAGE_DEVICE_GONE = 0x2D,
    DUAL_MESSAGE_PROFILE_ACK = 0x2E,
    DUAL_MESSAGE_ROLE_ACK = 0x2F,
    DUAL_MESSAGE_PROFILE_REQUEST = 0x30,
    DUAL_MESSAGE_PROFILE_OFFER = 0x31,
    DUAL_MESSAGE_FLOW_ACK = 0x32,
} dual_message_type_t;

/* UART1 peer role; unresolved is volatile boot-time discovery only. */
typedef enum {
    DUAL_ROLE_UNRESOLVED = 0,
    DUAL_ROLE_PC_DEVICE = 1,
    DUAL_ROLE_MOUSE_HOST = 2,
} dual_device_role_t;

static inline uint8_t dual_role_opposite(uint8_t role)
{
    if (role == DUAL_ROLE_PC_DEVICE) {
        return DUAL_ROLE_MOUSE_HOST;
    }
    if (role == DUAL_ROLE_MOUSE_HOST) {
        return DUAL_ROLE_PC_DEVICE;
    }
    return DUAL_ROLE_UNRESOLVED;
}

/* A confirmed peer identity wins; local USB evidence is the fallback. */
static inline uint8_t dual_role_resolve(uint8_t peer_role, uint8_t local_role)
{
    if (peer_role == DUAL_ROLE_PC_DEVICE || peer_role == DUAL_ROLE_MOUSE_HOST) {
        return dual_role_opposite(peer_role);
    }
    if (local_role == DUAL_ROLE_PC_DEVICE || local_role == DUAL_ROLE_MOUSE_HOST) {
        return local_role;
    }
    return DUAL_ROLE_UNRESOLVED;
}

static inline bool dual_peer_profile_invalidated(
    bool generation_initialized,
    uint32_t previous_generation,
    uint32_t generation,
    bool usb_state_initialized,
    uint8_t previous_usb_state,
    uint8_t usb_state)
{
    return (generation_initialized && previous_generation != generation) ||
        (usb_state_initialized &&
         previous_usb_state == DUAL_LINK_USB_STATE_HID_CONNECTED &&
         usb_state != DUAL_LINK_USB_STATE_HID_CONNECTED);
}

/*
 * PROFILE_ACK 现在同时携带双方 generation：只有绑定本机启动会话与当前
 * 对端会话的确认才能推进 M 的 HID 状态，旧队列里的迟到确认无法匹配。
 * 长度由 9 变为 17，两板必须刷同一版本固件。
 */
#define DUAL_LINK_PROFILE_ACK_LENGTH 17U
#define DUAL_LINK_PROFILE_ACK_TRANSFER_ID_OFFSET 0U
#define DUAL_LINK_PROFILE_ACK_CRC32_OFFSET 4U
#define DUAL_LINK_PROFILE_ACK_STATUS_OFFSET 8U
#define DUAL_LINK_PROFILE_ACK_RECIPIENT_GENERATION_OFFSET 9U
#define DUAL_LINK_PROFILE_ACK_SENDER_GENERATION_OFFSET 13U

#define DUAL_LOG_READ_REQUEST_LENGTH 5U
#define DUAL_LOG_DUMP_REQUEST_LENGTH 8U
#define DUAL_LOG_READ_RESPONSE_HEADER_LENGTH 8U
#define DUAL_HID_RAW_INPUT_HEADER_LENGTH 3U
#define DUAL_HID_SET_REPORT_HEADER_LENGTH 6U
#define DUAL_HID_GET_REPORT_REQUEST_LENGTH 6U
#define DUAL_HID_GET_REPORT_RESPONSE_HEADER_LENGTH 6U
#define DUAL_LINK_ROLE_ACK_LENGTH 5U
#define DUAL_LINK_PROFILE_REQUEST_LENGTH 8U
#define DUAL_LINK_PROFILE_OFFER_LENGTH 12U
#define DUAL_LINK_DEVICE_GONE_LENGTH 13U
#define DUAL_LINK_FLOW_ACK_LENGTH 14U
#define DUAL_LINK_DEVICE_GONE_SENDER_GENERATION_OFFSET 0U
#define DUAL_LINK_DEVICE_GONE_TARGET_GENERATION_OFFSET 4U
#define DUAL_LINK_DEVICE_GONE_EVENT_ID_OFFSET 8U
#define DUAL_LINK_DEVICE_GONE_REASON_OFFSET 12U
#define DUAL_LINK_FLOW_ACK_TYPE_OFFSET 0U
#define DUAL_LINK_FLOW_ACK_FLOW_ID_OFFSET 1U
#define DUAL_LINK_FLOW_ACK_STATUS_OFFSET 5U
#define DUAL_LINK_FLOW_ACK_RECIPIENT_GENERATION_OFFSET 6U
#define DUAL_LINK_FLOW_ACK_SENDER_GENERATION_OFFSET 10U

static inline bool dual_profile_request_may_send(
    uint8_t role,
    bool peer_online,
    bool role_acknowledged,
    bool cleanup_ready,
    bool request_pending,
    bool request_completed)
{
    return role == DUAL_ROLE_PC_DEVICE && peer_online && role_acknowledged &&
        cleanup_ready && !request_pending && !request_completed;
}

static inline bool dual_disconnect_barrier_allows_profile_offer(
    bool pending, bool failed)
{
    return !pending && !failed;
}

static inline bool dual_flow_id_is_newer(uint32_t candidate, uint32_t previous)
{
    const uint32_t distance = candidate - previous;
    return distance != 0U && distance < 0x80000000U;
}

static inline bool dual_profile_operation_is_current(
    uint32_t operation_epoch, uint32_t current_epoch, bool disconnect_pending)
{
    return operation_epoch != 0U && operation_epoch == current_epoch &&
        !disconnect_pending;
}

typedef enum {
    DUAL_FLOW_STATUS_ACCEPTED = 0,
    DUAL_FLOW_STATUS_FAILED = 1,
    /*
     * 已受理但物理鼠标尚未枚举：接收方明确告诉发起方“别重试、也别判失败，
     * 我在等设备到达”。没有这个区分时，P 会把“M 在等鼠标”当成失败，
     * 在鼠标插上之前就烧完重试预算并红闪（O3/O4 现象）。
     */
    DUAL_FLOW_STATUS_WAITING_DEVICE = 2,
} dual_flow_status_t;
#define DUAL_HID_RAW_INPUT_MAX_DATA \
    (DUAL_PROXY_MAX_PAYLOAD - DUAL_HID_RAW_INPUT_HEADER_LENGTH)
#define DUAL_HID_CONTROL_MAX_DATA \
    (DUAL_PROXY_MAX_PAYLOAD - DUAL_HID_SET_REPORT_HEADER_LENGTH)

typedef enum {
    DUAL_HID_REPORT_TYPE_INPUT = 1,
    DUAL_HID_REPORT_TYPE_OUTPUT = 2,
    DUAL_HID_REPORT_TYPE_FEATURE = 3,
} dual_hid_report_type_t;

typedef enum {
    DUAL_HID_REPORT_STATUS_OK = 0,
    DUAL_HID_REPORT_STATUS_INVALID = 1,
    DUAL_HID_REPORT_STATUS_TIMEOUT = 2,
    DUAL_HID_REPORT_STATUS_UNSUPPORTED = 3,
} dual_hid_report_status_t;

typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t sequence;
    uint8_t payload_length;
    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD];
} dual_frame_t;

typedef void (*dual_frame_callback_t)(const dual_frame_t *frame, void *context);

typedef struct {
    uint8_t buffer[9 + DUAL_PROXY_MAX_PAYLOAD];
    size_t length;
    size_t expected_length;
    dual_frame_callback_t callback;
    void *callback_context;
} dual_parser_t;

void dual_parser_init(dual_parser_t *parser, dual_frame_callback_t callback, void *context);
void dual_parser_feed(dual_parser_t *parser, const uint8_t *data, size_t length);
uint16_t dual_crc16_ccitt(const uint8_t *data, size_t length);
esp_err_t dual_frame_serialize(const dual_frame_t *frame, uint8_t *output, size_t capacity, size_t *length);

bool dual_hid_raw_input_encode(
    uint8_t interface_number,
    uint8_t report_id,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length);
bool dual_hid_raw_input_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *interface_number,
    uint8_t *report_id,
    const uint8_t **data,
    size_t *data_length);

bool dual_hid_set_report_encode(
    uint16_t transaction_id,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length);
bool dual_hid_set_report_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *interface_number,
    uint8_t *report_id,
    uint8_t *report_type,
    const uint8_t **data,
    size_t *data_length);

bool dual_hid_get_request_encode(
    uint16_t transaction_id,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    uint8_t requested_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length);
bool dual_hid_get_request_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *interface_number,
    uint8_t *report_id,
    uint8_t *report_type,
    uint8_t *requested_length);

bool dual_hid_get_response_encode(
    uint16_t transaction_id,
    uint8_t status,
    uint8_t interface_number,
    uint8_t report_id,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length);
bool dual_hid_get_response_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint16_t *transaction_id,
    uint8_t *status,
    uint8_t *interface_number,
    uint8_t *report_id,
    const uint8_t **data,
    size_t *data_length);

/* 板载日志下载：请求 offset + max_bytes，响应 offset + total_bytes + 数据。 */
bool dual_log_read_request_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint32_t *offset,
    uint8_t *max_bytes);
bool dual_log_read_response_encode(
    uint32_t offset,
    uint32_t total_bytes,
    const uint8_t *data,
    size_t data_length,
    uint8_t *payload,
    size_t capacity,
    uint8_t *payload_length);
bool dual_log_read_response_decode(
    const uint8_t *payload,
    size_t payload_length,
    uint32_t *offset,
    uint32_t *total_bytes,
    const uint8_t **data,
    size_t *data_length);
