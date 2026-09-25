#include "vendor_urb.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "vendor_urb";

/*
 * 等待上限。正常 HID 控制传输是毫秒级，**超过 1 秒就已经是"异常"而不是"慢"**
 * （用户指正：正常 USB 操作延迟不会大于 1 秒）。取 800 ms：
 *   - 远大于正常/略慢的情形，不误判；
 *   - 小于 1 秒红线，设备真的卡住时能尽快进入"重试 → 升级恢复"。
 * 每请求独立 URB + 孤儿回收保证：即使这里判超时，也不会锁死通道。
 */
#define VENDOR_URB_TIMEOUT_MS 800U
/*
 * 每次请求最多尝试次数：瞬时抖动靠第 2 次吃掉；两次都超时才认为设备不响应，
 * 以免无脑重发把 URB 堆成孤儿、并推迟真正有效的端口断电恢复。
 */
#define VENDOR_URB_ATTEMPTS 2U
#define VENDOR_URB_CLIENT_TASK_STACK 4096
#define VENDOR_URB_CLIENT_TASK_PRIORITY 4

typedef struct {
    SemaphoreHandle_t done;
    usb_transfer_t *transfer;
    volatile bool completed;
    /* 调用方已放弃等待：由回调负责释放 URB 与上下文（flush EP0 不被支持，
     * 只能等它自己完成；per-request URB 保证它不影响其它请求）。 */
    volatile bool orphan;
    volatile uint8_t status;
} urb_wait_t;

static usb_host_client_handle_t s_client;
static TaskHandle_t s_client_task;
static usb_device_handle_t s_device;
static uint8_t s_device_address;
static SemaphoreHandle_t s_device_mutex;

static volatile uint32_t s_submitted;
static volatile uint32_t s_completed;
static volatile uint32_t s_timeouts;
static volatile uint32_t s_aborted;
static volatile uint32_t s_device_reopens;
static volatile uint32_t s_orphans_reaped;
static volatile uint32_t s_retries;
/*
 * 控制传输耗时分布（区分"拥堵"与"设备拒答"）：
 *   拥堵（IO 被占满）→ 成功的那批也会明显变慢（耗时分布整体右移）；
 *   设备拒答         → 绝大多数仍是毫秒级，只是个别整笔失败。
 */
static volatile int64_t s_latency_max_us;
static volatile uint32_t s_latency_over_10ms;
static volatile uint32_t s_latency_over_100ms;

static void urb_transfer_callback(usb_transfer_t *transfer)
{
    urb_wait_t *wait = (urb_wait_t *)transfer->context;
    if (wait == NULL) {
        return;
    }
    wait->status = (uint8_t)transfer->status;
    wait->completed = true;
    if (wait->orphan) {
        /* 调用方已放弃等待：这里负责收尾，避免泄漏在飞的 URB。 */
        ++s_orphans_reaped;
        vSemaphoreDelete(wait->done);
        usb_host_transfer_free(transfer);
        free(wait);
        return;
    }
    xSemaphoreGive(wait->done);
}

static void urb_client_event_callback(const usb_host_client_event_msg_t *message,
                                      void *argument)
{
    (void)argument;
    if (message->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        /* 设备消失：关掉本客户端的句柄，下次请求时按新地址重开。 */
        if (s_device_mutex != NULL &&
            xSemaphoreTake(s_device_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (s_device != NULL) {
                (void)usb_host_device_close(s_client, s_device);
                s_device = NULL;
            }
            s_device_address = 0;
            xSemaphoreGive(s_device_mutex);
        }
        ESP_LOGW(TAG, "设备已移除：关闭直连控制通道的设备句柄");
    }
}

static void urb_client_task(void *argument)
{
    (void)argument;
    while (true) {
        (void)usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

esp_err_t dual_vendor_urb_start(void)
{
    if (s_client != NULL) {
        return ESP_OK;
    }
    if (s_device_mutex == NULL) {
        s_device_mutex = xSemaphoreCreateMutex();
        if (s_device_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    const usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = urb_client_event_callback,
            .callback_arg = NULL,
        },
    };
    esp_err_t result = usb_host_client_register(&config, &s_client);
    if (result != ESP_OK) {
        s_client = NULL;
        ESP_LOGE(TAG, "USB 客户端注册失败：%s", esp_err_to_name(result));
        return result;
    }
    if (xTaskCreate(urb_client_task, "vendor_urb_events", VENDOR_URB_CLIENT_TASK_STACK,
                    NULL, VENDOR_URB_CLIENT_TASK_PRIORITY, &s_client_task) != pdPASS) {
        (void)usb_host_client_deregister(s_client);
        s_client = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "直连控制通道已启动（每请求独立 URB，超时 %u ms）",
             (unsigned)VENDOR_URB_TIMEOUT_MS);
    return ESP_OK;
}

/* 取得（必要时打开）目标地址的设备句柄。调用方需持有 s_device_mutex。 */
static esp_err_t ensure_device_locked(uint8_t device_address)
{
    if (s_device != NULL && s_device_address == device_address) {
        return ESP_OK;
    }
    if (s_device != NULL) {
        (void)usb_host_device_close(s_client, s_device);
        s_device = NULL;
    }
    const esp_err_t result = usb_host_device_open(s_client, device_address, &s_device);
    if (result != ESP_OK) {
        s_device = NULL;
        s_device_address = 0;
        return result;
    }
    s_device_address = device_address;
    ++s_device_reopens;
    ESP_LOGI(TAG, "直连控制通道已打开设备：addr=%u（第 %u 次）",
             device_address, (unsigned)s_device_reopens);
    return ESP_OK;
}

/* 单次尝试：分配独立 URB → 提交 → 等待（超时则挂孤儿）。 */
static esp_err_t urb_set_report_once(
    uint8_t interface_number,
    uint8_t report_type,
    uint8_t report_id,
    const uint8_t *data,
    size_t length,
    uint32_t timeout_ms)
{
    if (length > (sizeof(((usb_transfer_t *)0)->data_buffer) - 8U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_device_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_device == NULL) {
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    usb_transfer_t *transfer = NULL;
    result = usb_host_transfer_alloc(8U + length, 0, &transfer);
    if (result != ESP_OK || transfer == NULL) {
        xSemaphoreGive(s_device_mutex);
        return result == ESP_OK ? ESP_ERR_NO_MEM : result;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)transfer->data_buffer;
    setup->bmRequestType = 0x21U;         /* class, interface, OUT */
    setup->bRequest = 0x09U;              /* SET_REPORT */
    setup->wValue = (uint16_t)(((uint16_t)report_type << 8) | report_id);
    setup->wIndex = interface_number;
    setup->wLength = (uint16_t)length;
    if (length != 0U && data != NULL) {
        memcpy(transfer->data_buffer + 8U, data, length);
    }

    urb_wait_t *wait = (urb_wait_t *)calloc(1, sizeof(urb_wait_t));
    if (wait == NULL) {
        usb_host_transfer_free(transfer);
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_NO_MEM;
    }
    wait->done = xSemaphoreCreateBinary();
    wait->transfer = transfer;
    if (wait->done == NULL) {
        free(wait);
        usb_host_transfer_free(transfer);
        xSemaphoreGive(s_device_mutex);
        return ESP_ERR_NO_MEM;
    }

    transfer->device_handle = s_device;
    transfer->bEndpointAddress = 0;
    transfer->callback = urb_transfer_callback;
    transfer->context = wait;
    transfer->num_bytes = (int)(8U + length);

    ++s_submitted;
    const int64_t transfer_start_us = esp_timer_get_time();
    result = usb_host_transfer_submit_control(s_client, transfer);
    if (result == ESP_OK) {
        if (xSemaphoreTake(wait->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            /*
             * 超时：US B 栈不实现逐传输超时，而 EP0 又不支持 flush
             * （usb_host_endpoint_flush(dev, 0) 返回 ESP_ERR_INVALID_ARG），
             * 所以把这笔挂成"孤儿"交给回调收尾：per-request URB 下它不会
             * 影响任何其它请求，通道保持可用。
             */
            ++s_timeouts;
            wait->orphan = true;
            if (wait->completed) {
                /* 回调刚刚跑完并已自行释放：这里不能再碰 wait。 */
                xSemaphoreGive(s_device_mutex);
                return ESP_ERR_TIMEOUT;
            }
            ESP_LOGW(TAG, "控制传输超时（%u ms）：挂为孤儿等待回收（已回收 %u 次）",
                     (unsigned)timeout_ms, (unsigned)s_orphans_reaped);
            xSemaphoreGive(s_device_mutex);
            return ESP_ERR_TIMEOUT;
        }
        ++s_completed;
        const int64_t elapsed_us = esp_timer_get_time() - transfer_start_us;
        if (elapsed_us > s_latency_max_us) {
            s_latency_max_us = elapsed_us;
        }
        if (elapsed_us > 10000) {
            ++s_latency_over_10ms;
        }
        if (elapsed_us > 100000) {
            ++s_latency_over_100ms;
        }
        result = (wait->status == 0U) ? ESP_OK : ESP_FAIL;
    } else {
        ++s_aborted;
    }

    vSemaphoreDelete(wait->done);
    usb_host_transfer_free(transfer);
    free(wait);
    xSemaphoreGive(s_device_mutex);
    return result;
}

esp_err_t dual_vendor_urb_set_report(
    uint8_t device_address,
    uint8_t interface_number,
    uint8_t report_type,
    uint8_t report_id,
    const uint8_t *data,
    size_t length,
    uint32_t timeout_ms)
{
    if (s_client == NULL || s_device_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 打开/复用设备句柄（按地址缓存）。 */
    if (xSemaphoreTake(s_device_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t open_result = ensure_device_locked(device_address);
    xSemaphoreGive(s_device_mutex);
    if (open_result != ESP_OK) {
        return open_result;
    }

    esp_err_t result = ESP_ERR_TIMEOUT;
    for (uint32_t attempt = 0; attempt < VENDOR_URB_ATTEMPTS; ++attempt) {
        result = urb_set_report_once(interface_number, report_type, report_id,
                                     data, length, timeout_ms);
        if (result != ESP_ERR_TIMEOUT) {
            break;
        }
        if (attempt + 1U < VENDOR_URB_ATTEMPTS) {
            ++s_retries;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return result;
}

void dual_vendor_urb_stats(
    uint32_t *submitted,
    uint32_t *completed,
    uint32_t *timeouts,
    uint32_t *aborted,
    uint32_t *device_reopens,
    uint32_t *retries,
    int64_t *latency_max_us,
    uint32_t *latency_over_10ms,
    uint32_t *latency_over_100ms)
{
    if (retries != NULL) {
        *retries = s_retries;
    }
    if (latency_max_us != NULL) {
        *latency_max_us = s_latency_max_us;
    }
    if (latency_over_10ms != NULL) {
        *latency_over_10ms = s_latency_over_10ms;
    }
    if (latency_over_100ms != NULL) {
        *latency_over_100ms = s_latency_over_100ms;
    }
    if (submitted != NULL) {
        *submitted = s_submitted;
    }
    if (completed != NULL) {
        *completed = s_completed;
    }
    if (timeouts != NULL) {
        *timeouts = s_timeouts;
    }
    if (aborted != NULL) {
        *aborted = s_aborted;
    }
    if (device_reopens != NULL) {
        *device_reopens = s_device_reopens;
    }
}
