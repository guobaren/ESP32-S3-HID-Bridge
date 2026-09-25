#include "uart0_control.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bridge_protocol.h"
#include "onboard_log.h"
#include "uart1_link.h"

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
    if (uart_write_bytes(CONTROL_UART, serialized, serialized_length) == (int)serialized_length) {
        (void)uart_wait_tx_done(CONTROL_UART, pdMS_TO_TICKS(100));
    }
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
    if (uart_write_bytes(CONTROL_UART, serialized, serialized_length) ==
        (int)serialized_length) {
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
    (void)uart_wait_tx_done(CONTROL_UART, pdMS_TO_TICKS(500));
    /* 以空分片收尾，客户端据此确认流结束。 */
    send_log_chunk(request, offset + sent, NULL, 0U);
    (void)uart_wait_tx_done(CONTROL_UART, pdMS_TO_TICKS(500));
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
    if (uart_write_bytes(CONTROL_UART, serialized, serialized_length) ==
        (int)serialized_length) {
        (void)uart_wait_tx_done(CONTROL_UART, pdMS_TO_TICKS(200));
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
    (void)uart_write_bytes(CONTROL_UART, serialized, serialized_length);
    ESP_LOGW(TAG, "板载日志已清空：%s", esp_err_to_name(cleared));
}

static void on_control_frame(const dual_frame_t *frame, void *context)
{
    (void)context;
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
                (void)uart_write_bytes(CONTROL_UART, serialized, serialized_length);
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
    esp_err_t result = uart_param_config(CONTROL_UART, &config);
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
