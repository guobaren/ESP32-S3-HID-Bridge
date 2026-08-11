#include "output_router.h"

#include "ble_output.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hid_output.h"
#include "status_led.h"
#include "wifi_target_output.h"

static const char *TAG = "output_router";
static SemaphoreHandle_t s_mode_mutex;
static output_mode_selector_t s_selector;
static bool s_release_pending;

static const char *mode_name(output_mode_t mode)
{
    switch (mode) {
    case OUTPUT_MODE_USB:
        return "USB HID";
    case OUTPUT_MODE_BLE:
        return "BLE HID";
    default:
        return "无";
    }
}

static esp_err_t submit_to_mode(output_mode_t mode, const bridge_frame_t *frame)
{
    if (mode == OUTPUT_MODE_USB) {
        return hid_output_submit(frame);
    }
    if (mode == OUTPUT_MODE_BLE) {
        return ble_output_submit(frame);
    }
    return ESP_OK;
}

esp_err_t output_router_init(void)
{
    s_mode_mutex = xSemaphoreCreateMutex();
    if (s_mode_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t usb_result = hid_output_init();
    if (usb_result != ESP_OK) {
        return usb_result;
    }
    esp_err_t ble_result = ble_output_init();
    if (ble_result != ESP_OK && ble_result != ESP_ERR_NOT_SUPPORTED) {
        return ble_result;
    }
#if HID_BRIDGE_WIFI_RUNTIME_ENABLED
    /* Target Agent 输出实现暂时保留，当前交付配置不创建其任务或网络连接。 */
    esp_err_t wifi_result = wifi_target_output_init();
    return wifi_result == ESP_ERR_NOT_SUPPORTED ? ESP_OK : wifi_result;
#else
    return ESP_OK;
#endif
}

esp_err_t output_router_submit(const bridge_frame_t *frame)
{
    if (frame == NULL || s_mode_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    output_mode_t active_mode;
    bool release_pending;
    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    active_mode = s_selector.active_mode;
    release_pending = s_release_pending;
    if (frame->type == BRIDGE_MESSAGE_RELEASE_ALL) {
        s_release_pending = false;
        release_pending = false;
    }
    xSemaphoreGive(s_mode_mutex);

    esp_err_t mode_result = ESP_OK;
    if (release_pending && active_mode != OUTPUT_MODE_NONE) {
        bridge_frame_t release_frame = {
            .version = BRIDGE_PROTOCOL_VERSION,
            .type = BRIDGE_MESSAGE_RELEASE_ALL,
            .payload_length = 0,
        };
        mode_result = submit_to_mode(active_mode, &release_frame);
        if (mode_result == ESP_OK) {
            xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
            if (s_selector.active_mode == active_mode) {
                s_release_pending = false;
            }
            xSemaphoreGive(s_mode_mutex);
        }
    }
    if (mode_result == ESP_OK) {
        mode_result = submit_to_mode(active_mode, frame);
    }

    if (mode_result != ESP_OK && mode_result != ESP_ERR_NOT_SUPPORTED) {
        return mode_result;
    }
#if HID_BRIDGE_WIFI_RUNTIME_ENABLED
    /* 保留镜像到 Target Agent 的代码路径，当前不向网络输出任何键鼠报告。 */
    esp_err_t wifi_result = wifi_target_output_submit(frame);
    return wifi_result == ESP_ERR_NOT_SUPPORTED ? ESP_OK : wifi_result;
#else
    return ESP_OK;
#endif
}

void output_router_set_connected(output_mode_t mode, bool connected)
{
    if (s_mode_mutex == NULL || mode == OUTPUT_MODE_NONE) {
        return;
    }

    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    output_mode_t previous_mode = s_selector.active_mode;
    output_mode_t active_mode = output_mode_selector_set_connected(&s_selector, mode, connected);
    if (active_mode != previous_mode) {
        /*
         * 当前活动链路断开后才切换。切换前先释放旧目标，避免旧目标留下
         * 卡键或按住的鼠标按钮；新目标收到下一帧前也会先 ReleaseAll。
         */
        if (previous_mode != OUTPUT_MODE_NONE) {
            bridge_frame_t release_frame = {
                .version = BRIDGE_PROTOCOL_VERSION,
                .type = BRIDGE_MESSAGE_RELEASE_ALL,
                .payload_length = 0,
            };
            esp_err_t release_result = submit_to_mode(previous_mode, &release_frame);
            if (release_result != ESP_OK && release_result != ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(
                    TAG,
                    "切换输出前释放 %s 失败：%s",
                    mode_name(previous_mode),
                    esp_err_to_name(release_result));
            }
        }
        s_release_pending = active_mode != OUTPUT_MODE_NONE;
    }
    xSemaphoreGive(s_mode_mutex);

    status_led_set_active_mode(active_mode);

    if (active_mode != previous_mode) {
        ESP_LOGI(
            TAG,
            "输出模式切换：%s -> %s（%s %s）",
            mode_name(previous_mode),
            mode_name(active_mode),
            mode_name(mode),
            connected ? "已连接" : "已断开");
    } else if (connected && active_mode != mode) {
        ESP_LOGI(TAG, "%s 已连接，但当前活动输出为 %s，不向该连接发送报告", mode_name(mode), mode_name(active_mode));
    }
}

esp_err_t output_router_release_all(void)
{
    bridge_frame_t frame = {
        .version = BRIDGE_PROTOCOL_VERSION,
        .type = BRIDGE_MESSAGE_RELEASE_ALL,
        .payload_length = 0,
    };
    return output_router_submit(&frame);
}
