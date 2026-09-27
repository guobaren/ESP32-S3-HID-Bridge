#include "uart0_control.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bridge_protocol.h"
#include "diag_stream.h"
#include "dual_proxy_app.h"
#include "hid_device_profile.h"
#include "hid_host_mouse.h"
#include "onboard_log.h"
#include "pc_hid_output.h"
#include "uart1_link.h"
#include "uart0_output.h"

#define CONTROL_UART UART_NUM_0
#define CONTROL_BAUD 921600
#define CONTROL_RX_BUFFER_SIZE 4096
#define CONTROL_TX_BUFFER_SIZE 4096
#define CONTROL_LEASE_MS 1500

static const char *TAG = "dual_uart0";
static dual_software_report_callback_t s_report_callback;
static dual_software_release_callback_t s_release_callback;
static uint8_t s_role;
/* 只服务日志命令（电脑侧板的调试口）：不接受输入会话。 */
static bool s_log_only;

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
    uint64_t log_chunks;
} control_state_t;

static control_state_t s_state;
static uint8_t s_injected_blob[HID_PROFILE_MAX_BLOB];
static hid_device_profile_t s_injected_profile;
static uint32_t s_injected_length;
static uint32_t s_injected_received;
static uint32_t s_injected_crc32;
static TickType_t s_injected_started;
static bool s_injection_active;

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

static void send_log_chunk(const dual_frame_t *request, uint32_t offset, const uint8_t *data,
                           size_t length)
{
    dual_frame_t response = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_LOG_READ_RESPONSE,
        .sequence = request->sequence,
    };
    if (!dual_log_read_response_encode(offset, dual_onboard_log_total_bytes(), data, length,
                                       response.payload, sizeof(response.payload),
                                       &response.payload_length)) {
        return;
    }
    uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
    size_t serialized_length = 0;
    if (dual_frame_serialize(&response, serialized, sizeof(serialized),
                             &serialized_length) != ESP_OK) {
        return;
    }
    if (dual_uart0_output_write(serialized, serialized_length) == ESP_OK) {
        ++s_state.log_chunks;
    }
}

/*
 * 流式下载：一条请求连续回多个分片，避免 56 字节/次的逐块往返。
 * uart_write_bytes 在发送缓冲满时会阻塞，天然形成节流，不需要额外延时。
 */
static void send_log_dump(const dual_frame_t *request)
{
    if (request->payload_length != DUAL_LOG_DUMP_REQUEST_LENGTH) {
        return;
    }
    uint32_t offset = 0U;
    uint32_t budget = 0U;
    memcpy(&offset, &request->payload[0], sizeof(offset));
    memcpy(&budget, &request->payload[4], sizeof(budget));
    dual_onboard_log_request_flush();
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t data[DUAL_LOG_CHUNK_MAX];
    uint8_t block[1024];
    size_t block_length = 0U;
    size_t block_offset = 0U;
    uint32_t sent = 0U;
    while (sent < budget) {
        if (block_offset >= block_length) {
            /* 一次 VFS 读放 1 KB，再切成 56 字节帧：否则每帧都要 fopen/fread。 */
            size_t want = budget - sent;
            if (want > sizeof(block)) {
                want = sizeof(block);
            }
            block_length = dual_onboard_log_read(offset + sent, block, want);
            block_offset = 0U;
            if (block_length == 0U) {
                break;
            }
        }
        size_t chunk = block_length - block_offset;
        if (chunk > DUAL_LOG_CHUNK_MAX) {
            chunk = DUAL_LOG_CHUNK_MAX;
        }
        memcpy(data, &block[block_offset], chunk);
        send_log_chunk(request, offset + sent, data, chunk);
        block_offset += chunk;
        sent += (uint32_t)chunk;
    }
    /* 以空分片收尾，客户端据此确认流结束。 */
    send_log_chunk(request, offset + sent, NULL, 0U);
}

static void send_log_response(const dual_frame_t *request)
{
    uint32_t offset = 0U;
    uint8_t max_bytes = 0U;
    if (!dual_log_read_request_decode(request->payload, request->payload_length,
                                      &offset, &max_bytes)) {
        return;
    }
    if (offset == 0U) {
        /* 首次请求时让写盘任务先落盘，客户端就能一次拿到包括尾部在内的内容。 */
        dual_onboard_log_request_flush();
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    if (max_bytes > DUAL_LOG_CHUNK_MAX) {
        max_bytes = DUAL_LOG_CHUNK_MAX;
    }
    uint8_t data[DUAL_LOG_CHUNK_MAX];
    const size_t read = dual_onboard_log_read(offset, data, max_bytes);
    dual_frame_t response = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_LOG_READ_RESPONSE,
        .sequence = request->sequence,
    };
    if (!dual_log_read_response_encode(offset, dual_onboard_log_total_bytes(), data, read,
                                       response.payload, sizeof(response.payload),
                                       &response.payload_length)) {
        return;
    }
    uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
    size_t serialized_length = 0;
    if (dual_frame_serialize(&response, serialized, sizeof(serialized),
                             &serialized_length) != ESP_OK) {
        return;
    }
    if (dual_uart0_output_write(serialized, serialized_length) == ESP_OK) {
        ++s_state.log_chunks;
    }
}

static void send_log_cleared(const dual_frame_t *request)
{
    dual_frame_t response = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_LOG_READ_RESPONSE,
        .sequence = request->sequence,
        .payload_length = DUAL_LOG_READ_RESPONSE_HEADER_LENGTH,
    };
    const esp_err_t cleared = dual_onboard_log_clear();
    const uint32_t total = cleared == ESP_OK ? 0U : dual_onboard_log_total_bytes();
    if (!dual_log_read_response_encode(0U, total, NULL, 0U, response.payload,
                                       sizeof(response.payload),
                                       &response.payload_length)) {
        return;
    }
    uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
    size_t serialized_length = 0;
    if (dual_frame_serialize(&response, serialized, sizeof(serialized),
                             &serialized_length) != ESP_OK) {
        return;
    }
    (void)dual_uart0_output_write(serialized, serialized_length);
    ESP_LOGW(TAG, "板载日志已清空：%s", esp_err_to_name(cleared));
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
    /*
     * 日志命令不受输入会话约束：没插鼠标、没建会话时也要能把板载日志取走，
     * 否则恰恰在最需要日志的故障现场取不到。
     */
    if (frame->type == DUAL_MESSAGE_LOG_READ_REQUEST) {
        ++s_state.accepted;
        send_log_response(frame);
        return;
    }
    if (frame->type == DUAL_MESSAGE_LOG_CONTROL_REQUEST) {
        ++s_state.accepted;
        if (frame->payload_length == 1U) {
            dual_onboard_log_set_paused(frame->payload[0] != 0U);
            ESP_LOGW(TAG, "板载写盘%s", frame->payload[0] != 0U ? "已暂停（A/B 对照）" : "已恢复");
        }
        /* 用一条 READ_RESPONSE 回报状态：data[0]=是否暂停，total=当前字节数。 */
        const uint8_t paused_state = dual_onboard_log_paused() ? 1U : 0U;
        dual_frame_t response = {
            .version = DUAL_PROXY_PROTOCOL_VERSION,
            .type = DUAL_MESSAGE_LOG_READ_RESPONSE,
            .sequence = frame->sequence,
        };
        if (dual_log_read_response_encode(0U, dual_onboard_log_total_bytes(), &paused_state,
                                          1U, response.payload, sizeof(response.payload),
                                          &response.payload_length)) {
            uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
            size_t serialized_length = 0;
            if (dual_frame_serialize(&response, serialized, sizeof(serialized),
                                     &serialized_length) == ESP_OK) {
                (void)dual_uart0_output_write(serialized, serialized_length);
            }
        }
        return;
    }
    if (frame->type == DUAL_MESSAGE_LOG_DUMP_REQUEST) {
        ++s_state.accepted;
        send_log_dump(frame);
        return;
    }
    if (frame->type == DUAL_MESSAGE_LOG_CLEAR_REQUEST) {
        ++s_state.accepted;
        send_log_cleared(frame);
        return;
    }
    if (frame->type == DUAL_MESSAGE_DIAG_PROFILE_READ) {
        ++s_state.accepted;
        handle_profile_read(frame);
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
    if (frame->type == DUAL_MESSAGE_DIAG_STREAM_CONTROL) {
        if (frame->payload_length == 1U && frame->payload[0] <= 1U) {
            dual_diag_stream_set_enabled(frame->payload[0] != 0U);
        }
        dual_diag_stream_send_status(frame->sequence);
        return;
    }
    if (s_log_only) {
        ++s_state.rejected;
        return;
    }
    if (frame->type == DUAL_MESSAGE_SESSION_START ||
        frame->type == DUAL_MESSAGE_PING ||
        frame->type == DUAL_MESSAGE_MOUSE_REPORT ||
        frame->type == DUAL_MESSAGE_RELEASE_ALL) {
        if (!accept_input_frame(frame)) {
            return;
        }
        if (frame->type == DUAL_MESSAGE_RELEASE_ALL) {
            if (s_release_callback != NULL) {
                s_release_callback();
            }
            return;
        }
        if (frame->type == DUAL_MESSAGE_MOUSE_REPORT) {
            if ((frame->payload_length != 7 && frame->payload_length != 8) ||
                (frame->payload_length == 8 && frame->payload[7] != 0 && frame->payload[7] != 5)) {
                ++s_state.rejected;
                ESP_LOGW(TAG, "拒绝长度或平滑槽非法的 MouseReport");
                return;
            }
            ++s_state.mouse_reports;
            if (s_report_callback != NULL) {
                s_report_callback(frame);
            }
        }
        return;
    }
    ++s_state.rejected;
}

static void control_task(void *argument)
{
    (void)argument;
    dual_parser_t parser;
    dual_parser_init(&parser, on_control_frame, NULL);
    TickType_t last_statistics = xTaskGetTickCount();
    while (true) {
        uint8_t buffer[128];
        /*
         * 先阻塞等待 1 字节，再一次性排空已经到达的字节。不要用“大块读取 + 20 ms
         * 超时”：UART0 上的软件移动帧通常只有 17 字节，那种读法会把每条命令稳定
         * 留在接收路径约 20 ms，连续命令还会在进入 5 槽平滑前合并。
         */
        int received = uart_read_bytes(CONTROL_UART, buffer, 1, portMAX_DELAY);
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
            dual_parser_feed(&parser, buffer, (size_t)received);
        }
        if (s_state.active && xTaskGetTickCount() - s_state.last_activity > pdMS_TO_TICKS(CONTROL_LEASE_MS)) {
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
            ESP_LOGI(TAG, "UART0协议统计：接收=%" PRIu64 " MouseReport=%" PRIu64
                     " MouseReportHz=%" PRIu64
                     " 接受=%" PRIu64 " 拒绝=%" PRIu64 " 序号不连续=%" PRIu64,
                     s_state.received, s_state.mouse_reports, mouse_reports_window,
                     s_state.accepted, s_state.rejected, s_state.discontinuities);
            s_state.last_mouse_reports = s_state.mouse_reports;
            last_statistics = xTaskGetTickCount();
        }
    }
}

/* 内部实现：由双角色入口和日志服务入口共用。 */
static esp_err_t uart0_control_start(
    uint8_t role,
    dual_software_report_callback_t report_callback,
    dual_software_release_callback_t release_callback)
{
    if (role != DUAL_ROLE_PC_DEVICE && role != DUAL_ROLE_MOUSE_HOST) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = dual_uart0_output_init();
    if (result != ESP_OK) {
        return result;
    }
    s_role = role;
    s_report_callback = report_callback;
    s_release_callback = release_callback;
    s_report_callback = report_callback;
    s_release_callback = release_callback;
    const uart_config_t config = {
        .baud_rate = CONTROL_BAUD,
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
    if (xTaskCreate(control_task, "dual_uart0", 4096, NULL, 7, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t dual_uart0_control_start(
    uint8_t role,
    dual_software_report_callback_t report_callback,
    dual_software_release_callback_t release_callback)
{
    s_log_only = false;
    return uart0_control_start(role, report_callback, release_callback);
}

esp_err_t dual_uart0_log_service_start(uint8_t role)
{
    /* 电脑侧板的调试口只跑日志服务：不接受软件输入会话，也不转发鼠标报告。 */
    s_log_only = true;
    return uart0_control_start(role, NULL, NULL);
}
