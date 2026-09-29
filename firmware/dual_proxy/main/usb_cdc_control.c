#include "usb_cdc_control.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"

#include "usb_cdc_control_logic.h"
#include "onboard_log.h"

#define CDC_CONTROL_PORT TINYUSB_CDC_ACM_0
#define CDC_RX_BUFFER_SIZE 512
#define CDC_CONTROL_TASK_STACK 4096
#define CDC_CONTROL_TASK_PRIORITY 7
#define CDC_STATISTICS_PERIOD_MS 5000U

static const char *TAG = "dual_usb_cdc";
static TaskHandle_t s_control_task;
static SemaphoreHandle_t s_control_stopped;
static volatile bool s_stop_requested;
static dual_cdc_report_callback_t s_report_callback;
static dual_cdc_release_callback_t s_release_callback;
static dual_cdc_session_state_t s_session;
static volatile bool s_detached;
static volatile bool s_line_event_pending;
static volatile bool s_line_dtr;
static volatile bool s_line_rts;
static uint64_t s_received;
static uint64_t s_accepted;
static uint64_t s_rejected;
static uint64_t s_mouse_reports;
static uint64_t s_discontinuities;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void send_device_hello(const dual_frame_t *probe)
{
    static const uint8_t signature[] = {'H', 'I', 'D', 'B', 'R', 'D', 'G', '2'};
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
        ++s_rejected;
        ESP_LOGW(TAG, "DeviceHello编码失败");
        return;
    }
    const size_t queued = tinyusb_cdcacm_write_queue(CDC_CONTROL_PORT, serialized, serialized_length);
    if (queued != serialized_length) {
        ++s_rejected;
        ESP_LOGW(TAG, "DeviceHello入队不完整：%u/%u", (unsigned)queued, (unsigned)serialized_length);
        return;
    }
    const esp_err_t result = tinyusb_cdcacm_write_flush(CDC_CONTROL_PORT, pdMS_TO_TICKS(100));
    if (result != ESP_OK) {
        ++s_rejected;
        ESP_LOGW(TAG, "DeviceHello发送失败：%s", esp_err_to_name(result));
    }
}

static void send_log_response(const dual_frame_t *request)
{
    dual_onboard_log_request_flush();
    vTaskDelay(pdMS_TO_TICKS(120));
    uint32_t offset = 0U;
    uint8_t max_bytes = 0U;
    if (!dual_log_read_request_decode(request->payload, request->payload_length,
                                      &offset, &max_bytes)) {
        ++s_rejected;
        return;
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
        ++s_rejected;
        return;
    }
    uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
    size_t serialized_length = 0;
    if (dual_frame_serialize(&response, serialized, sizeof(serialized),
                             &serialized_length) != ESP_OK) {
        ++s_rejected;
        return;
    }
    const size_t queued = tinyusb_cdcacm_write_queue(CDC_CONTROL_PORT, serialized,
                                                     serialized_length);
    if (queued != serialized_length ||
        tinyusb_cdcacm_write_flush(CDC_CONTROL_PORT, pdMS_TO_TICKS(200)) != ESP_OK) {
        ++s_rejected;
        return;
    }
    ++s_accepted;
}

static void on_control_frame(const dual_frame_t *frame, void *context)
{
    (void)context;
    ++s_received;
    const uint32_t timestamp = now_ms();
    if (frame->type == DUAL_MESSAGE_DEVICE_PROBE) {
        if (dual_cdc_session_accept(&s_session, frame, timestamp) == DUAL_CDC_ACTION_DEVICE_PROBE) {
            send_device_hello(frame);
            ++s_accepted;
        } else {
            ++s_rejected;
        }
        return;
    }
    /* 日志命令不占用输入会话：没建会话时也能取板载日志。 */
    if (frame->type == DUAL_MESSAGE_LOG_READ_REQUEST) {
        send_log_response(frame);
        return;
    }
    if (frame->type == DUAL_MESSAGE_LOG_DUMP_REQUEST &&
        frame->payload_length == DUAL_LOG_DUMP_REQUEST_LENGTH) {
        uint32_t offset = 0U;
        uint32_t budget = 0U;
        memcpy(&offset, &frame->payload[0], sizeof(offset));
        memcpy(&budget, &frame->payload[4], sizeof(budget));
        dual_onboard_log_request_flush();
        vTaskDelay(pdMS_TO_TICKS(120));
        uint8_t data[DUAL_LOG_CHUNK_MAX];
        uint32_t sent = 0U;
        while (sent < budget) {
            size_t want = budget - sent;
            if (want > DUAL_LOG_CHUNK_MAX) {
                want = DUAL_LOG_CHUNK_MAX;
            }
            const size_t read = dual_onboard_log_read(offset, data, want);
            if (read == 0U) {
                break;
            }
            dual_frame_t response = {
                .version = DUAL_PROXY_PROTOCOL_VERSION,
                .type = DUAL_MESSAGE_LOG_READ_RESPONSE,
                .sequence = frame->sequence,
            };
            if (!dual_log_read_response_encode(offset, dual_onboard_log_total_bytes(),
                                               data, read, response.payload,
                                               sizeof(response.payload),
                                               &response.payload_length)) {
                break;
            }
            uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
            size_t serialized_length = 0;
            if (dual_frame_serialize(&response, serialized, sizeof(serialized),
                                     &serialized_length) != ESP_OK) {
                break;
            }
            if (tinyusb_cdcacm_write_queue(CDC_CONTROL_PORT, serialized,
                                           serialized_length) != serialized_length ||
                tinyusb_cdcacm_write_flush(CDC_CONTROL_PORT, pdMS_TO_TICKS(500)) != ESP_OK) {
                break;
            }
            offset += (uint32_t)read;
            sent += (uint32_t)read;
        }
        ++s_accepted;
        return;
    }
    if (frame->type == DUAL_MESSAGE_LOG_CLEAR_REQUEST) {
        (void)dual_onboard_log_clear();
        dual_frame_t response = {
            .version = DUAL_PROXY_PROTOCOL_VERSION,
            .type = DUAL_MESSAGE_LOG_READ_RESPONSE,
            .sequence = frame->sequence,
        };
        if (dual_log_read_response_encode(0U, 0U, NULL, 0U, response.payload,
                                          sizeof(response.payload),
                                          &response.payload_length)) {
            uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
            size_t serialized_length = 0;
            if (dual_frame_serialize(&response, serialized, sizeof(serialized),
                                     &serialized_length) == ESP_OK &&
                tinyusb_cdcacm_write_queue(CDC_CONTROL_PORT, serialized,
                                           serialized_length) == serialized_length) {
                (void)tinyusb_cdcacm_write_flush(CDC_CONTROL_PORT, pdMS_TO_TICKS(200));
                ++s_accepted;
            }
        }
        ESP_LOGW(TAG, "板载日志已清空（CDC）");
        return;
    }

    const bool was_sequence_initialized = s_session.sequence_initialized;
    const uint16_t expected_sequence = s_session.expected_sequence;
    const dual_cdc_frame_action_t action = dual_cdc_session_accept(&s_session, frame, timestamp);
    if (was_sequence_initialized && expected_sequence != frame->sequence) {
        ++s_discontinuities;
        ESP_LOGW(TAG, "CDC帧序号不连续：期望=%u 实际=%u", expected_sequence, frame->sequence);
    }
    if (action == DUAL_CDC_ACTION_REJECT) {
        ++s_rejected;
        return;
    }
    ++s_accepted;
    if (action == DUAL_CDC_ACTION_SOFTWARE_RELEASE) {
        if (s_release_callback != NULL) {
            s_release_callback();
        }
        return;
    }
    if (action == DUAL_CDC_ACTION_MOUSE_REPORT) {
        ++s_mouse_reports;
        if (s_report_callback != NULL) {
            s_report_callback(frame);
        }
    }
}

static void cdc_rx_callback(int interface, cdcacm_event_t *event)
{
    (void)event;
    if (interface == CDC_CONTROL_PORT && s_control_task != NULL) {
        /* USB callback只通知；读取和协议解析全部在普通任务中完成。 */
        xTaskNotifyGive(s_control_task);
    }
}

static void cdc_line_state_callback(int interface, cdcacm_event_t *event)
{
    if (interface != CDC_CONTROL_PORT || event == NULL || s_control_task == NULL) {
        return;
    }
    s_line_dtr = event->line_state_changed_data.dtr;
    s_line_rts = event->line_state_changed_data.rts;
    s_line_event_pending = true;
    xTaskNotifyGive(s_control_task);
}

static void release_software_input(void)
{
    dual_cdc_session_expire(&s_session);
    if (s_release_callback != NULL) {
        s_release_callback();
    }
}

static void process_disconnect_events(void)
{
    if (s_detached) {
        s_detached = false;
        release_software_input();
        ESP_LOGI(TAG, "原生USB CDC断开，已释放软件输入");
    }
    if (s_line_event_pending) {
        s_line_event_pending = false;
        /* DTR/RTS只作为串口关闭提示，不影响USB角色，也不触发板复位。 */
        if (!s_line_dtr && !s_line_rts && s_session.active) {
            release_software_input();
            ESP_LOGI(TAG, "CDC串口关闭，已释放软件输入");
        }
    }
}

static void control_task(void *argument)
{
    (void)argument;
    dual_parser_t parser;
    dual_parser_init(&parser, on_control_frame, NULL);
    TickType_t last_statistics = xTaskGetTickCount();
    uint64_t last_statistics_mouse_reports = s_mouse_reports;
    while (!s_stop_requested) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        if (s_stop_requested) {
            break;
        }
        process_disconnect_events();

        while (tud_cdc_n_available(CDC_CONTROL_PORT) > 0) {
            uint8_t buffer[CDC_RX_BUFFER_SIZE];
            size_t received = 0;
            if (tinyusb_cdcacm_read(CDC_CONTROL_PORT, buffer, sizeof(buffer), &received) != ESP_OK ||
                received == 0) {
                break;
            }
            dual_parser_feed(&parser, buffer, received);
        }

        const uint32_t timestamp = now_ms();
        if (dual_cdc_session_lease_expired(&s_session, timestamp)) {
            release_software_input();
            ESP_LOGW(TAG, "CDC软件输入租约超时，已释放软件输入");
        }
        if (xTaskGetTickCount() - last_statistics >= pdMS_TO_TICKS(CDC_STATISTICS_PERIOD_MS)) {
            /*
             * 本周期没有鼠标报告就不打印（2026-09-28）：空闲时每秒一行没有信息量。
             * 计数器仍要无条件推进，否则窗口增量会越算越错。
             */
            const uint64_t mouse_reports_window =
                s_mouse_reports - last_statistics_mouse_reports;
            if (mouse_reports_window > 0U) {
                ESP_LOGI(TAG, "CDC协议统计：接收=%" PRIu64 " 接受=%" PRIu64
                         " MouseReport=%" PRIu64 " 拒绝=%" PRIu64 " 序号不连续=%" PRIu64,
                         s_received, s_accepted, s_mouse_reports, s_rejected, s_discontinuities);
            }
            last_statistics_mouse_reports = s_mouse_reports;
            last_statistics = xTaskGetTickCount();
        }
    }
    release_software_input();
    s_control_task = NULL;
    xSemaphoreGive(s_control_stopped);
    vTaskDelete(NULL);
}

esp_err_t dual_usb_cdc_control_start(
    dual_cdc_report_callback_t report_callback,
    dual_cdc_release_callback_t release_callback)
{
    if (s_control_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 重枚举后重新初始化 CDC 时允许传 NULL，继续沿用原角色回调。 */
    if (report_callback != NULL) {
        s_report_callback = report_callback;
    }
    if (release_callback != NULL) {
        s_release_callback = release_callback;
    }
    s_detached = false;
    s_line_event_pending = false;
    s_line_dtr = false;
    s_line_rts = false;
    s_received = 0;
    s_accepted = 0;
    s_rejected = 0;
    s_mouse_reports = 0;
    s_discontinuities = 0;
    s_stop_requested = false;
    if (s_control_stopped == NULL) {
        s_control_stopped = xSemaphoreCreateBinary();
        if (s_control_stopped == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    dual_cdc_session_init(&s_session);

    const tinyusb_config_cdcacm_t config = {
        .cdc_port = CDC_CONTROL_PORT,
        .callback_rx = cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = cdc_line_state_callback,
        .callback_line_coding_changed = NULL,
    };
    esp_err_t result = tinyusb_cdcacm_init(&config);
    if (result != ESP_OK) {
        return result;
    }
    if (xTaskCreate(control_task, "dual_usb_cdc", CDC_CONTROL_TASK_STACK, NULL,
                    CDC_CONTROL_TASK_PRIORITY, &s_control_task) != pdPASS) {
        (void)tinyusb_cdcacm_deinit(CDC_CONTROL_PORT);
        s_control_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "原生USB CDC控制已启动；仅CDC承载协议v2，UART0仅保留日志");
    return ESP_OK;
}

esp_err_t dual_usb_cdc_control_stop(void)
{
    if (s_control_task == NULL) {
        return ESP_OK;
    }
    s_stop_requested = true;
    xTaskNotifyGive(s_control_task);
    if (xSemaphoreTake(s_control_stopped, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return tinyusb_cdcacm_deinit(CDC_CONTROL_PORT);
}

void dual_usb_cdc_control_on_detached(void)
{
    if (s_control_task != NULL) {
        s_detached = true;
        xTaskNotifyGive(s_control_task);
    }
}
