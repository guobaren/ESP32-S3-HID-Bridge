#include "input_session.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "output_router.h"

static const char *TAG = "input_session";
static SemaphoreHandle_t s_mutex;
static bridge_input_source_t s_active_source;
static TickType_t s_last_activity;

static void lease_watchdog_task(void *context)
{
    (void)context;
    const TickType_t timeout = pdMS_TO_TICKS(CONFIG_HID_BRIDGE_LEASE_TIMEOUT_MS);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bool expired = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_active_source != BRIDGE_INPUT_NONE &&
            xTaskGetTickCount() - s_last_activity > timeout) {
            ESP_LOGW(TAG, "输入租约超时，来源=%d，释放全部输入", s_active_source);
            s_active_source = BRIDGE_INPUT_NONE;
            expired = true;
        }
        xSemaphoreGive(s_mutex);
        if (expired) {
            output_router_release_all();
        }
    }
}

esp_err_t input_session_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t created = xTaskCreate(lease_watchdog_task, "input_lease", 3072, NULL, 6, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void input_session_handle(bridge_input_source_t source, const bridge_frame_t *frame)
{
    if (source == BRIDGE_INPUT_NONE || frame == NULL) {
        return;
    }
    bool new_session = false;
    bool accepted = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (frame->type == BRIDGE_MESSAGE_SESSION_START) {
        new_session = true;
        accepted = true;
        s_active_source = source;
        s_last_activity = xTaskGetTickCount();
        ESP_LOGI(TAG, "新的输入会话，来源=%d", source);
    } else if (s_active_source == BRIDGE_INPUT_NONE && frame->type == BRIDGE_MESSAGE_PING) {
        accepted = true;
        s_active_source = source;
        s_last_activity = xTaskGetTickCount();
        ESP_LOGI(TAG, "空闲状态下恢复输入租约，来源=%d", source);
    } else if (s_active_source == source) {
        accepted = true;
        s_last_activity = xTaskGetTickCount();
    }
    xSemaphoreGive(s_mutex);

    if (!accepted) {
        return;
    }
    if (new_session) {
        output_router_release_all();
        return;
    }
    if (frame->type == BRIDGE_MESSAGE_PING) {
        return;
    }
    esp_err_t error = output_router_submit(frame);
    if (error != ESP_OK && error != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "丢弃输出帧，错误：%s", esp_err_to_name(error));
    }
}

void input_session_disconnected(bridge_input_source_t source)
{
    bool was_active = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_active_source == source) {
        s_active_source = BRIDGE_INPUT_NONE;
        was_active = true;
    }
    xSemaphoreGive(s_mutex);
    if (was_active) {
        ESP_LOGW(TAG, "活动输入通道断开，来源=%d", source);
        output_router_release_all();
    }
}

void input_session_release_all(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_active_source = BRIDGE_INPUT_NONE;
        xSemaphoreGive(s_mutex);
    }
    output_router_release_all();
}
