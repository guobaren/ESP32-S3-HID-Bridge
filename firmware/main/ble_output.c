#include "ble_output.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_hid_gap.h"
#include "esp_timer.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "output_router.h"

#if CONFIG_HID_BRIDGE_BLE_ENABLE && !CONFIG_BT_NIMBLE_NVS_PERSIST
#error "BLE HID bonding requires CONFIG_BT_NIMBLE_NVS_PERSIST"
#endif

static const char *TAG = "ble_output";
#define FIXED_BATTERY_LEVEL 100

static esp_hidd_dev_t *s_device;
static volatile bool s_connected;
static uint64_t s_mouse_sent_reports;
static int64_t s_mouse_sent_x;
static int64_t s_mouse_sent_y;
static uint64_t s_mouse_send_failures;
static uint64_t s_mouse_send_attempts;
static uint32_t s_mouse_max_pending_x;
static uint32_t s_mouse_max_pending_y;
static uint32_t s_mouse_max_pending_reports;
static int64_t s_mouse_pending_x;
static int64_t s_mouse_pending_y;
static int64_t s_mouse_pending_wheel;
static int64_t s_mouse_pending_pan;
static uint8_t s_mouse_pending_buttons;
static uint8_t s_mouse_last_received_buttons;
static bool s_mouse_buttons_dirty;
static uint32_t s_mouse_pending_reports;
static int64_t s_mouse_last_send_us;
static uint32_t s_mouse_min_send_interval_us;
static uint32_t s_mouse_max_send_interval_us;
static uint64_t s_mouse_last_stats_sent_reports;
static int64_t s_mouse_last_stats_sent_x;
static int64_t s_mouse_last_stats_sent_y;
static uint32_t s_mouse_window_min_send_interval_us;
static uint32_t s_mouse_window_max_send_interval_us;
static SemaphoreHandle_t s_mouse_mutex;
static TaskHandle_t s_mouse_sender_task;

#define BLE_MOUSE_SEND_INTERVAL_MS 10
#define BLE_MOUSE_SEND_TASK_PRIORITY 10
#if CONFIG_FREERTOS_UNICORE
#define BLE_MOUSE_SEND_TASK_CORE tskNO_AFFINITY
#else
#define BLE_MOUSE_SEND_TASK_CORE 1
#endif
// X/Y 在 HID 报告中是有符号 16 位，取消过小的 16 单位人工上限。
// 这样一次 BLE 报告可以承载普通大范围移动，避免因拆成大量 16 单位报告而产生明显积压延迟。
#define BLE_MOUSE_MAX_MOTION_PER_REPORT INT16_MAX
#define BLE_MOUSE_MAX_WHEEL_PER_REPORT 8

static const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05,
    0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,

    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02,
    0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03,
    0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10,
    0x95, 0x02, 0x81, 0x06, 0x09, 0x38, 0x15, 0x81,
    0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25,
    0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0,
    0xC0,
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {.data = REPORT_MAP, .len = sizeof(REPORT_MAP)},
};

static esp_hid_device_config_t s_config = {
    .vendor_id = 0x303A,
    .product_id = 0x4002,
    .version = 0x0100,
    .device_name = CONFIG_HID_BRIDGE_BLE_DEVICE_NAME,
    .manufacturer_name = "HidBridge",
    .serial_number = "HIDBRIDGE",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static void ble_output_set_connected(bool connected, const char *reason)
{
    bool changed = s_connected != connected;
    s_connected = connected;
    output_router_set_connected(OUTPUT_MODE_BLE, connected);
    if (changed) {
        ESP_LOGI(TAG, "BLE HID %s（%s）", connected ? "已就绪" : "已断开", reason);
    }
}

void ble_hid_task_start_up(void)
{
    ble_output_set_connected(true, "加密完成");
}

void ble_hid_task_shut_down(void)
{
    ble_output_set_connected(false, "链路关闭");
    if (s_mouse_mutex != NULL && xSemaphoreTake(s_mouse_mutex, portMAX_DELAY) == pdTRUE) {
        s_mouse_pending_x = 0;
        s_mouse_pending_y = 0;
        s_mouse_pending_wheel = 0;
        s_mouse_pending_pan = 0;
        s_mouse_pending_buttons = 0;
        s_mouse_last_received_buttons = 0;
        s_mouse_buttons_dirty = false;
        s_mouse_pending_reports = 0;
        xSemaphoreGive(s_mouse_mutex);
    }
}

static void ble_host_task(void *context)
{
    (void)context;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool ble_mouse_has_pending_state_locked(void)
{
    return s_mouse_buttons_dirty || s_mouse_pending_x != 0 ||
           s_mouse_pending_y != 0 || s_mouse_pending_wheel != 0 ||
           s_mouse_pending_pan != 0;
}

static int64_t clamp_pending_delta(int64_t value, int64_t limit)
{
    if (value > limit) {
        return limit;
    }
    if (value < -limit) {
        return -limit;
    }
    return value;
}

static void ble_mouse_record_send_interval(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_mouse_last_send_us > 0) {
        uint32_t interval_us = (uint32_t)(now_us - s_mouse_last_send_us);
        if (s_mouse_min_send_interval_us == 0 || interval_us < s_mouse_min_send_interval_us) {
            s_mouse_min_send_interval_us = interval_us;
        }
        if (interval_us > s_mouse_max_send_interval_us) {
            s_mouse_max_send_interval_us = interval_us;
        }
        if (s_mouse_window_min_send_interval_us == 0 || interval_us < s_mouse_window_min_send_interval_us) {
            s_mouse_window_min_send_interval_us = interval_us;
        }
        if (interval_us > s_mouse_window_max_send_interval_us) {
            s_mouse_window_max_send_interval_us = interval_us;
        }
    }
    s_mouse_last_send_us = now_us;
}

static void ble_mouse_sender_task(void *context)
{
    (void)context;
    const TickType_t send_period = pdMS_TO_TICKS(BLE_MOUSE_SEND_INTERVAL_MS);
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&last_wake, send_period);

        uint8_t report[7] = {0};
        bool should_send = false;
        int16_t sent_x = 0;
        int16_t sent_y = 0;
        int8_t sent_wheel = 0;
        int8_t sent_pan = 0;

        if (s_mouse_mutex == NULL || xSemaphoreTake(s_mouse_mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_connected && ble_mouse_has_pending_state_locked()) {
            sent_x = (int16_t)clamp_pending_delta(s_mouse_pending_x, BLE_MOUSE_MAX_MOTION_PER_REPORT);
            sent_y = (int16_t)clamp_pending_delta(s_mouse_pending_y, BLE_MOUSE_MAX_MOTION_PER_REPORT);
            sent_wheel = (int8_t)clamp_pending_delta(s_mouse_pending_wheel, BLE_MOUSE_MAX_WHEEL_PER_REPORT);
            sent_pan = (int8_t)clamp_pending_delta(s_mouse_pending_pan, BLE_MOUSE_MAX_WHEEL_PER_REPORT);
            report[0] = s_mouse_pending_buttons;
            report[1] = (uint8_t)sent_x;
            report[2] = (uint8_t)(sent_x >> 8);
            report[3] = (uint8_t)sent_y;
            report[4] = (uint8_t)(sent_y >> 8);
            report[5] = (uint8_t)sent_wheel;
            report[6] = (uint8_t)sent_pan;
            s_mouse_pending_x -= sent_x;
            s_mouse_pending_y -= sent_y;
            s_mouse_pending_wheel -= sent_wheel;
            s_mouse_pending_pan -= sent_pan;
            s_mouse_buttons_dirty = false;
            s_mouse_pending_reports = ble_mouse_has_pending_state_locked() ? 1U : 0U;
            should_send = true;
        }
        if (!should_send) {
            xSemaphoreGive(s_mouse_mutex);
            continue;
        }

        // 发送调用也放在同一把锁内，避免 ReleaseAll/断线清理与旧报告并发交错。
        s_mouse_send_attempts++;
        esp_err_t result = esp_hidd_dev_input_set(s_device, 0, 2, report, sizeof(report));
        if (result == ESP_OK) {
            s_mouse_sent_reports++;
            s_mouse_sent_x += sent_x;
            s_mouse_sent_y += sent_y;
            ble_mouse_record_send_interval();
        } else {
            s_mouse_send_failures++;
            s_mouse_pending_x += sent_x;
            s_mouse_pending_y += sent_y;
            s_mouse_pending_wheel += sent_wheel;
            s_mouse_pending_pan += sent_pan;
            s_mouse_buttons_dirty = true;
            s_mouse_pending_reports = 1;
            ESP_LOGW(TAG, "BLE鼠标报告发送失败：err=%s，保留未发送位移=(%" PRId16 ",%" PRId16 ")",
                     esp_err_to_name(result), sent_x, sent_y);
        }
        xSemaphoreGive(s_mouse_mutex);
    }
}

static void ble_statistics_task(void *context)
{
    (void)context;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t pending_x = 0;
        int64_t pending_y = 0;
        uint32_t pending_reports = 0;
        uint64_t sent_reports = 0;
        uint64_t send_attempts = 0;
        uint64_t send_failures = 0;
        int64_t sent_x = 0;
        int64_t sent_y = 0;
        uint64_t window_sent_reports = 0;
        int64_t window_sent_x = 0;
        int64_t window_sent_y = 0;
        uint32_t window_min_interval_us = 0;
        uint32_t window_max_interval_us = 0;
        if (s_mouse_mutex != NULL && xSemaphoreTake(s_mouse_mutex, portMAX_DELAY) == pdTRUE) {
            pending_x = s_mouse_pending_x;
            pending_y = s_mouse_pending_y;
            pending_reports = s_mouse_pending_reports;
            sent_reports = s_mouse_sent_reports;
            send_attempts = s_mouse_send_attempts;
            send_failures = s_mouse_send_failures;
            sent_x = s_mouse_sent_x;
            sent_y = s_mouse_sent_y;
            window_sent_reports = sent_reports - s_mouse_last_stats_sent_reports;
            window_sent_x = sent_x - s_mouse_last_stats_sent_x;
            window_sent_y = sent_y - s_mouse_last_stats_sent_y;
            window_min_interval_us = s_mouse_window_min_send_interval_us;
            window_max_interval_us = s_mouse_window_max_send_interval_us;
            s_mouse_last_stats_sent_reports = sent_reports;
            s_mouse_last_stats_sent_x = sent_x;
            s_mouse_last_stats_sent_y = sent_y;
            s_mouse_window_min_send_interval_us = 0;
            s_mouse_window_max_send_interval_us = 0;
            xSemaphoreGive(s_mouse_mutex);
        }
        ESP_LOGI(
            TAG,
            "BLE鼠标统计：本秒发送=%" PRIu64 " 发送累计=%" PRIu64 " 尝试=%" PRIu64 " 失败=%" PRIu64
            " 本秒位移=(%" PRId64 ",%" PRId64 ") 累计位移=(%" PRId64 ",%" PRId64 ")"
            " 待发送=(%" PRId64 ",%" PRId64 ") 待发送状态=%u 最大积压=(%u,%u) 最大待发送状态=%u"
            " 本秒间隔us=(%u..%u) 累计间隔us=(%u..%u)",
            window_sent_reports,
            sent_reports,
            send_attempts,
            send_failures,
            window_sent_x,
            window_sent_y,
            sent_x,
            sent_y,
            pending_x,
            pending_y,
            pending_reports,
            s_mouse_max_pending_x,
            s_mouse_max_pending_y,
            s_mouse_max_pending_reports,
            window_min_interval_us,
            window_max_interval_us,
            s_mouse_min_send_interval_us,
            s_mouse_max_send_interval_us);
    }
}

void ble_store_config_init(void);

static void hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *data)
{
    (void)handler_args;
    (void)base;
    esp_hidd_event_data_t *event = (esp_hidd_event_data_t *)data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "BLE HID 已启动并开始广播");
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        if (event->connect.status == ESP_OK) {
            ESP_LOGI(TAG, "BLE HID 物理链路已建立，等待加密完成");
        } else {
            ble_output_set_connected(false, "物理连接失败");
        }
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ble_hid_task_shut_down();
        break;
    default:
        break;
    }
}

esp_err_t ble_output_init(void)
{
#if !CONFIG_HID_BRIDGE_BLE_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#elif !CONFIG_BT_NIMBLE_ENABLED
    ESP_LOGE(TAG, "BLE 输出已启用，但 NimBLE 未启用");
    return ESP_ERR_INVALID_STATE;
#else
    /*
     * NimBLE Info 会为每次 notify 生成串口文本。连续鼠标移动时，这些同步日志
     * 会与 UART 输入及 10 ms 发送任务争用 CPU/日志锁；连接与性能诊断由本项目
     * 的 ESP_HID_GAP、NIMBLE_HIDD 和 ble_output 周期统计保留。
     */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    ESP_RETURN_ON_ERROR(esp_hid_gap_init(HIDD_BLE_MODE), TAG, "初始化 BLE GAP 失败");
    ESP_RETURN_ON_ERROR(
        esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, s_config.device_name),
        TAG,
        "初始化 BLE 广播失败");
    ESP_RETURN_ON_ERROR(
        esp_hidd_dev_init(&s_config, ESP_HID_TRANSPORT_BLE, hidd_event_callback, &s_device),
        TAG,
        "初始化 BLE HID 失败");
    ESP_RETURN_ON_ERROR(
        esp_hidd_dev_battery_set(s_device, FIXED_BATTERY_LEVEL),
        TAG,
        "设置 BLE 固定电量失败");
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    s_mouse_mutex = xSemaphoreCreateMutex();
    if (s_mouse_mutex == NULL) {
        ESP_LOGE(TAG, "无法创建 BLE 鼠标发送锁");
        return ESP_ERR_NO_MEM;
    }
    nimble_port_freertos_init(ble_host_task);
    /*
     * UART 生产者优先级为 9；发送者若维持旧优先级 5，会在连续高频输入下被
     * 延迟数十到数百毫秒。固定到应用核并提高一级，保证每 10 ms 先消费一次
     * 合并位移，再让 UART 继续灌入新报告。
     */
    if (xTaskCreatePinnedToCore(
            ble_mouse_sender_task,
            "ble_mouse_tx",
            3072,
            NULL,
            BLE_MOUSE_SEND_TASK_PRIORITY,
            &s_mouse_sender_task,
            BLE_MOUSE_SEND_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "无法创建 BLE 鼠标发送任务");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "BLE 鼠标发送任务：周期=%d ms 优先级=%d Core=%d",
             BLE_MOUSE_SEND_INTERVAL_MS,
             BLE_MOUSE_SEND_TASK_PRIORITY,
             BLE_MOUSE_SEND_TASK_CORE);
    if (xTaskCreate(ble_statistics_task, "ble_stats", 3072, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "无法创建 BLE 鼠标统计任务");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

esp_err_t ble_output_submit(const bridge_frame_t *frame)
{
#if !CONFIG_HID_BRIDGE_BLE_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (frame == NULL || s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_connected) {
        return ESP_OK;
    }
    if (frame->type == BRIDGE_MESSAGE_KEYBOARD_REPORT && frame->payload_length == 8) {
        return esp_hidd_dev_input_set(s_device, 0, 1, (uint8_t *)frame->payload, 8);
    }
    if (frame->type == BRIDGE_MESSAGE_MOUSE_REPORT && frame->payload_length == 7) {
        if (s_mouse_mutex == NULL || xSemaphoreTake(s_mouse_mutex, portMAX_DELAY) != pdTRUE) {
            return ESP_ERR_INVALID_STATE;
        }
        int16_t x = (int16_t)((uint16_t)frame->payload[1] | ((uint16_t)frame->payload[2] << 8));
        int16_t y = (int16_t)((uint16_t)frame->payload[3] | ((uint16_t)frame->payload[4] << 8));
        int8_t wheel = (int8_t)frame->payload[5];
        int8_t pan = (int8_t)frame->payload[6];
        s_mouse_buttons_dirty = s_mouse_buttons_dirty ||
                                frame->payload[0] != s_mouse_last_received_buttons;
        s_mouse_pending_buttons = frame->payload[0];
        s_mouse_last_received_buttons = frame->payload[0];
        s_mouse_pending_x += x;
        s_mouse_pending_y += y;
        s_mouse_pending_wheel += wheel;
        s_mouse_pending_pan += pan;
        // 移动采用 latest-state：所有输入位移合并为当前唯一待发送状态，
        // 不再把每个上游输入帧计为需要逐帧补发的历史队列。
        s_mouse_pending_reports = ble_mouse_has_pending_state_locked() ? 1U : 0U;
        if ((uint32_t)llabs(s_mouse_pending_x) > s_mouse_max_pending_x) {
            s_mouse_max_pending_x = (uint32_t)llabs(s_mouse_pending_x);
        }
        if ((uint32_t)llabs(s_mouse_pending_y) > s_mouse_max_pending_y) {
            s_mouse_max_pending_y = (uint32_t)llabs(s_mouse_pending_y);
        }
        if (s_mouse_pending_reports > s_mouse_max_pending_reports) {
            s_mouse_max_pending_reports = s_mouse_pending_reports;
        }
        xSemaphoreGive(s_mouse_mutex);
        return ESP_OK;
    }
    if (frame->type == BRIDGE_MESSAGE_RELEASE_ALL) {
        uint8_t keyboard[8] = {0};
        uint8_t mouse[7] = {0};
        if (s_mouse_mutex != NULL && xSemaphoreTake(s_mouse_mutex, portMAX_DELAY) == pdTRUE) {
            s_mouse_pending_x = 0;
            s_mouse_pending_y = 0;
            s_mouse_pending_wheel = 0;
            s_mouse_pending_pan = 0;
            s_mouse_pending_buttons = 0;
            s_mouse_last_received_buttons = 0;
            s_mouse_buttons_dirty = false;
            s_mouse_pending_reports = 0;
            xSemaphoreGive(s_mouse_mutex);
        }
        esp_err_t keyboard_result = esp_hidd_dev_input_set(s_device, 0, 1, keyboard, sizeof(keyboard));
        esp_err_t mouse_result = esp_hidd_dev_input_set(s_device, 0, 2, mouse, sizeof(mouse));
        return keyboard_result != ESP_OK ? keyboard_result : mouse_result;
    }
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
