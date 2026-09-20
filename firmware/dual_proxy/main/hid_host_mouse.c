#include "hid_host_mouse.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#include "dual_status_led.h"

#define HID_EVENT_QUEUE_LENGTH 16
#define HID_REPORT_QUEUE_LENGTH 64
#define HID_RAW_REPORT_MAX 64

static const char *TAG = "dual_hid_host";

typedef enum {
    HID_EVENT_CONNECTED,
    HID_EVENT_DISCONNECTED,
    HID_EVENT_TRANSFER_ERROR,
} hid_event_type_t;

typedef struct {
    hid_host_device_handle_t handle;
    hid_event_type_t type;
} hid_event_t;

typedef struct {
    uint8_t interface_number;
    uint8_t length;
    uint8_t data[HID_RAW_REPORT_MAX];
} raw_report_event_t;

static QueueHandle_t s_hid_event_queue;
static QueueHandle_t s_report_queue;
static dual_physical_mouse_callback_t s_report_callback;
static dual_physical_release_callback_t s_release_callback;
static volatile uint32_t s_reports;
static volatile uint32_t s_errors;

#define HID_STATS_PERIOD_MS 5000

static int8_t read_i8(const uint8_t *value)
{
    return (int8_t)*value;
}

static void raw_report_task(void *argument)
{
    (void)argument;
    raw_report_event_t event;
    while (true) {
        if (xQueueReceive(s_report_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* Boot mouse: buttons, signed X/Y, optional wheel and pan. */
        if (event.length < 3) {
            ++s_errors;
            continue;
        }
        const uint8_t buttons = event.data[0] & 0x1FU;
        const int16_t x = read_i8(&event.data[1]);
        const int16_t y = read_i8(&event.data[2]);
        const int8_t wheel = event.length >= 4 ? read_i8(&event.data[3]) : 0;
        const int8_t pan = event.length >= 5 ? read_i8(&event.data[4]) : 0;
        ++s_reports;
        if (s_report_callback != NULL) {
            s_report_callback(event.interface_number, 0, buttons, x, y, wheel, pan);
        }
    }
}

static void hid_stats_task(void *argument)
{
    (void)argument;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(HID_STATS_PERIOD_MS));
        ESP_LOGI(TAG, "Host HID统计：reports=%" PRIu32 " errors=%" PRIu32,
                 s_reports, s_errors);
    }
}

static void hid_interface_callback(
    hid_host_device_handle_t handle,
    hid_host_interface_event_t event,
    void *argument)
{
    (void)argument;
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        ++s_errors;
        return;
    }
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t data[HID_RAW_REPORT_MAX];
        size_t length = 0;
        if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &length) != ESP_OK ||
            length > sizeof(data) || length > UINT8_MAX) {
            ++s_errors;
            return;
        }
        raw_report_event_t queued = {
            .interface_number = params.iface_num,
            .length = (uint8_t)length,
        };
        memcpy(queued.data, data, length);
        if (xQueueSend(s_report_queue, &queued, 0) != pdTRUE) {
            ++s_errors;
            if (s_release_callback != NULL) {
                s_release_callback();
            }
        }
        return;
    }
    if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "标准鼠标接口断开：interface=%u", params.iface_num);
        dual_status_led_set_host_mouse_ready(false);
        if (s_release_callback != NULL) {
            s_release_callback();
        }
        (void)hid_host_device_close(handle);
        return;
    }
    if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        ++s_errors;
        ESP_LOGW(TAG, "标准鼠标接口传输错误：interface=%u", params.iface_num);
        if (s_release_callback != NULL) {
            s_release_callback();
        }
    }
}

static void hid_driver_callback(
    hid_host_device_handle_t handle,
    hid_host_driver_event_t event,
    void *argument)
{
    (void)argument;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED || s_hid_event_queue == NULL) {
        return;
    }
    const hid_event_t queued = {.handle = handle, .type = HID_EVENT_CONNECTED};
    if (xQueueSend(s_hid_event_queue, &queued, 0) != pdTRUE) {
        ++s_errors;
        if (s_release_callback != NULL) {
            s_release_callback();
        }
    }
}

static void hid_event_task(void *argument)
{
    (void)argument;
    hid_event_t event;
    while (true) {
        if (xQueueReceive(s_hid_event_queue, &event, portMAX_DELAY) != pdTRUE ||
            event.type != HID_EVENT_CONNECTED) {
            continue;
        }
        hid_host_dev_params_t params;
        if (hid_host_device_get_params(event.handle, &params) != ESP_OK) {
            ++s_errors;
            continue;
        }
        hid_host_dev_info_t info = {0};
        if (hid_host_get_device_info(event.handle, &info) == ESP_OK) {
            ESP_LOGI(TAG, "HID接口发现：addr=%u interface=%u subclass=%u protocol=%u VID:PID=%04X:%04X",
                     params.addr, params.iface_num, params.sub_class, params.proto, info.VID, info.PID);
        }
        if (params.proto != HID_PROTOCOL_MOUSE) {
            ESP_LOGI(TAG, "隔离非标准鼠标接口：interface=%u protocol=%u", params.iface_num, params.proto);
            continue;
        }
        if (params.sub_class != HID_SUBCLASS_BOOT_INTERFACE) {
            ESP_LOGW(TAG, "拒绝非Boot标准鼠标接口：interface=%u subclass=%u",
                     params.iface_num, params.sub_class);
            continue;
        }
        const hid_host_device_config_t config = {
            .callback = hid_interface_callback,
            .callback_arg = NULL,
        };
        esp_err_t result = hid_host_device_open(event.handle, &config);
        if (result == ESP_OK) {
            size_t descriptor_length = 0;
            uint8_t *descriptor = hid_host_get_report_descriptor(event.handle, &descriptor_length);
            if (descriptor == NULL || descriptor_length == 0) {
                ESP_LOGW(TAG, "标准鼠标接口没有可读报告描述符");
            } else {
                ESP_LOGI(TAG, "标准鼠标报告描述符长度=%u；使用Boot报告字段", (unsigned)descriptor_length);
            }
        }
        if (result == ESP_OK) {
            result = hid_class_request_set_protocol(event.handle, HID_REPORT_PROTOCOL_BOOT);
        }
        if (result == ESP_OK) {
            result = hid_host_device_start(event.handle);
        }
        if (result != ESP_OK) {
            ++s_errors;
            ESP_LOGE(TAG, "标准鼠标接口启动失败：%s", esp_err_to_name(result));
            (void)hid_host_device_close(event.handle);
            if (s_release_callback != NULL) {
                s_release_callback();
            }
        } else {
            dual_status_led_set_host_mouse_ready(true);
            ESP_LOGI(TAG, "标准鼠标输入已启动");
        }
    }
}

static void usb_host_library_task(void *argument)
{
    TaskHandle_t owner = (TaskHandle_t)argument;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t result = usb_host_install(&config);
    xTaskNotify(owner, (uint32_t)result, eSetValueWithOverwrite);
    if (result != ESP_OK) {
        vTaskDelete(NULL);
    }
    while (true) {
        uint32_t flags = 0;
        result = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (result != ESP_OK) {
            ++s_errors;
            ESP_LOGE(TAG, "USB Host事件处理失败：%s", esp_err_to_name(result));
        }
    }
}

esp_err_t dual_hid_host_start(
    dual_physical_mouse_callback_t report_callback,
    dual_physical_release_callback_t release_callback)
{
    s_report_callback = report_callback;
    s_release_callback = release_callback;
    s_hid_event_queue = xQueueCreate(HID_EVENT_QUEUE_LENGTH, sizeof(hid_event_t));
    s_report_queue = xQueueCreate(HID_REPORT_QUEUE_LENGTH, sizeof(raw_report_event_t));
    if (s_hid_event_queue == NULL || s_report_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(usb_host_library_task, "usb_host_events", 4096,
                   xTaskGetCurrentTaskHandle(), 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t notification = ESP_FAIL;
    if (xTaskNotifyWait(0, UINT32_MAX, &notification, pdMS_TO_TICKS(2000)) != pdTRUE ||
        (esp_err_t)notification != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    const hid_host_driver_config_t config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_driver_callback,
        .callback_arg = NULL,
    };
    esp_err_t result = hid_host_install(&config);
    if (result != ESP_OK) {
        return result;
    }
    if (xTaskCreate(hid_event_task, "hid_host_events", 4096, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(raw_report_task, "hid_raw_reports", 3072, NULL, 6, NULL) != pdPASS ||
        xTaskCreate(hid_stats_task, "hid_host_stats", 2048, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB角色锁定：MOUSE_HOST；仅接受protocol=2标准鼠标接口");
    return ESP_OK;
}
