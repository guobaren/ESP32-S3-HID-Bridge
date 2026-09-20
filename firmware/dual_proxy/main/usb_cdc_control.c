#include "usb_cdc_control.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"

#include "usb_cdc_control_logic.h"

#define CDC_CONTROL_PORT TINYUSB_CDC_ACM_0
#define CDC_RX_BUFFER_SIZE 512
#define CDC_CONTROL_TASK_STACK 4096
#define CDC_CONTROL_TASK_PRIORITY 7
#define CDC_STATISTICS_PERIOD_MS 5000U

static const char *TAG = "dual_usb_cdc";
static TaskHandle_t s_control_task;
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
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
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
            ESP_LOGI(TAG, "CDC协议统计：接收=%" PRIu64 " 接受=%" PRIu64
                     " MouseReport=%" PRIu64 " 拒绝=%" PRIu64 " 序号不连续=%" PRIu64,
                     s_received, s_accepted, s_mouse_reports, s_rejected, s_discontinuities);
            last_statistics = xTaskGetTickCount();
        }
    }
}

esp_err_t dual_usb_cdc_control_start(
    dual_cdc_report_callback_t report_callback,
    dual_cdc_release_callback_t release_callback)
{
    if (s_control_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_report_callback = report_callback;
    s_release_callback = release_callback;
    s_detached = false;
    s_line_event_pending = false;
    s_line_dtr = false;
    s_line_rts = false;
    s_received = 0;
    s_accepted = 0;
    s_rejected = 0;
    s_mouse_reports = 0;
    s_discontinuities = 0;
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

void dual_usb_cdc_control_on_detached(void)
{
    if (s_control_task != NULL) {
        s_detached = true;
        xTaskNotifyGive(s_control_task);
    }
}
