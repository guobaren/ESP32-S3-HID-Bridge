#include "uart0_control.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CONTROL_UART UART_NUM_0
#define CONTROL_BAUD 921600
#define CONTROL_RX_BUFFER_SIZE 4096
#define CONTROL_TX_BUFFER_SIZE 4096
#define CONTROL_LEASE_MS 1500

static const char *TAG = "dual_uart0";
static dual_software_report_callback_t s_report_callback;
static dual_software_release_callback_t s_release_callback;

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
        .payload_length = sizeof(signature) + 8,
    };
    memcpy(hello.payload, signature, sizeof(signature));
    memcpy(&hello.payload[sizeof(signature)], probe->payload, 8);
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

static void on_control_frame(const dual_frame_t *frame, void *context)
{
    (void)context;
    if (frame->type == DUAL_MESSAGE_DEVICE_PROBE) {
        send_device_hello(frame);
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
        const int received = uart_read_bytes(CONTROL_UART, buffer, sizeof(buffer), pdMS_TO_TICKS(20));
        if (received > 0) {
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
        if (xTaskGetTickCount() - last_statistics >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "UART0协议统计：接收=%" PRIu64 " MouseReport=%" PRIu64
                     " 接受=%" PRIu64 " 拒绝=%" PRIu64 " 序号不连续=%" PRIu64,
                     s_state.received, s_state.mouse_reports, s_state.accepted,
                     s_state.rejected, s_state.discontinuities);
            last_statistics = xTaskGetTickCount();
        }
    }
}

esp_err_t dual_uart0_control_start(
    dual_software_report_callback_t report_callback,
    dual_software_release_callback_t release_callback)
{
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
