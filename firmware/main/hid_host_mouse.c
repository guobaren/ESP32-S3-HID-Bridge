#include "hid_host_mouse.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#include "dual_status_led.h"
#include "diag_stream.h"
#include "hid_device_profile.h"
#include "hid_report_layout.h"
#include "link_recovery_logic.h"
#include "vendor_urb.h"
#include "usb_stall_recovery_logic.h"
#include "dual_proxy_runtime_config.h"
#include "uart0_control.h"

/*
 * 厂商控制传输走直连通道（每请求独立 URB + 自定超时 + 孤儿等待回调回收）还是走
 * usb_host_hid 组件的单例 ctrl_xfer。置 1 用直连通道；置 0 可一键回退对比。
 */
#define VENDOR_USE_DIRECT_URB 1
#include "uart1_link.h"

#define HID_EVENT_QUEUE_LENGTH 128
#define HID_REPORT_QUEUE_LENGTH 128
#define HID_RAW_REPORT_MAX 64
#define HID_PROFILE_DEBOUNCE_MS 200
#define HID_INTERFACE_SLOT_COUNT HID_PROFILE_MAX_REPORT_DESCRIPTORS
#define HID_CONTROL_QUEUE_LENGTH 128
#define HID_CONTROL_TASK_STACK 4096
#define HID_HOST_STOP_TIMEOUT_MS 2000
#define HID_HOST_STOP_POLL_MS 10

#define TASK_EXIT_HOST_LIBRARY (1U << 0)
#define TASK_EXIT_DESCRIPTOR (1U << 1)
#define TASK_EXIT_PROFILE (1U << 2)
#define TASK_EXIT_HID_EVENT (1U << 3)
#define TASK_EXIT_REPORT (1U << 4)
#define TASK_EXIT_CONTROL (1U << 5)
#define TASK_EXIT_STATS (1U << 6)
#define TASK_EVENT_ALL_FREE (1U << 7)
#define TASK_EVENT_HOST_INSTALL_DONE (1U << 8)
#define TASK_EXIT_STALL_WATCH (1U << 9)
#define TASK_EXIT_WORKERS (TASK_EXIT_DESCRIPTOR | TASK_EXIT_PROFILE | \
                           TASK_EXIT_HID_EVENT | TASK_EXIT_REPORT | \
                           TASK_EXIT_CONTROL | TASK_EXIT_STATS | TASK_EXIT_STALL_WATCH)

static const char *TAG = "dual_hid_host";

typedef struct {
    volatile uint32_t received;
    volatile uint32_t rejected;
    volatile uint32_t dropped;
    volatile uint32_t peak;
} queue_metrics_t;

static queue_metrics_t s_hid_event_queue_metrics;
static queue_metrics_t s_report_queue_metrics;
static queue_metrics_t s_control_queue_metrics;

static void queue_metric_increment(volatile uint32_t *counter)
{
    __atomic_fetch_add(counter, 1U, __ATOMIC_RELAXED);
}

static void queue_metric_add(volatile uint32_t *counter, uint32_t amount)
{
    if (amount != 0U) {
        __atomic_fetch_add(counter, amount, __ATOMIC_RELAXED);
    }
}

static void queue_metric_observe_depth(queue_metrics_t *metrics,
                                       QueueHandle_t queue)
{
    if (queue == NULL) {
        return;
    }
    uint32_t peak = __atomic_load_n(&metrics->peak, __ATOMIC_RELAXED);
    const uint32_t depth = (uint32_t)uxQueueMessagesWaiting(queue);
    while (depth > peak && !__atomic_compare_exchange_n(
               &metrics->peak, &peak, depth, true,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void queue_reset_count_dropped(QueueHandle_t queue,
                                      queue_metrics_t *metrics)
{
    if (queue != NULL) {
        queue_metric_add(&metrics->dropped,
                         (uint32_t)uxQueueMessagesWaiting(queue));
        xQueueReset(queue);
    }
}

static void log_queue_metrics(const char *name, const queue_metrics_t *metrics)
{
    ESP_LOGI(TAG, "QUEUE name=%s received=%" PRIu32 " rejected=%" PRIu32
             " dropped=%" PRIu32 " peak=%" PRIu32,
             name,
             __atomic_load_n(&metrics->received, __ATOMIC_RELAXED),
             __atomic_load_n(&metrics->rejected, __ATOMIC_RELAXED),
             __atomic_load_n(&metrics->dropped, __ATOMIC_RELAXED),
             __atomic_load_n(&metrics->peak, __ATOMIC_RELAXED));
}

typedef enum {
    HID_EVENT_CONNECTED,
    HID_EVENT_DISCONNECTED,
    HID_EVENT_TRANSFER_ERROR,
    HID_EVENT_STOP,
} hid_event_type_t;

typedef struct {
    hid_host_device_handle_t handle;
    hid_event_type_t type;
} hid_event_t;

typedef struct {
    uint8_t interface_number;
    uint8_t report_id;
    uint8_t length;
    bool mouse_report;
    bool injected;
    bool has_report_id;
    uint8_t mouse_report_id;
    hid_mouse_report_layout_t mouse_layout;
    bool stop;
    uint8_t data[HID_RAW_REPORT_MAX];
} raw_report_event_t;

typedef struct {
    bool active;
    bool started;
    bool mouse_interface;
    bool has_report_id;
    uint8_t mouse_report_id;
    uint8_t interface_number;
    hid_host_device_handle_t handle;
    /* 鼠标接口的字段布局（枚举时保存）：RX 回调里用它把报告解析成位移，
     * 供"收到的位移"统计使用（2026-09-27）。 */
    hid_mouse_report_layout_t mouse_layout;
} hid_interface_slot_t;

typedef struct {
    bool get_report;
    uint32_t vendor_session_epoch;
    uint32_t vendor_session_peer_generation;
    uint16_t transaction_id;
    uint8_t interface_number;
    uint8_t report_id;
    uint8_t report_type;
    uint8_t requested_length;
    uint8_t length;
    bool stop;
    /*
     * 设备级 Vendor 控制请求（2026-09-27）：为 true 时上面三个 HID 字段无意义，
     * 改由 bm_request_type/b_request/w_value/w_index 描述一笔任意 EP0 控制传输，
     * 走 dual_vendor_urb_control() 的通用通道；data/length 承载 OUT 数据。
     */
    bool vendor_control;
    uint8_t bm_request_type;
    uint8_t b_request;
    uint16_t w_value;
    uint16_t w_index;
    uint16_t w_length;
    uint8_t data[DUAL_HID_CONTROL_MAX_DATA];
} hid_control_event_t;

static QueueHandle_t s_hid_event_queue;
static QueueHandle_t s_report_queue;
static dual_physical_mouse_callback_t s_report_callback;
/* 最近一次厂商控制事务的时刻：供移动让路判断厂商是否在途。 */
static volatile int64_t s_vendor_control_activity_us;
/* “鼠标消失”的防抖窗口与挂起时刻：瞬断不上报 DEVICE_GONE，避免拆装克隆。 */
#define MOUSE_GONE_DEBOUNCE_US 800000LL
static volatile int64_t s_mouse_gone_pending_us;
/*
 * 只对“密集突发”让路（G HUB 初始化：请求间隔微秒级；正常轮询：几十毫秒）。
 * 早期实现按“最后一次请求后 400 ms”一律让路，正常轮询会把窗口不断续上，
 * 导致移动被持续丢弃、指针延迟巨大（实测 motion_skipped=8675）。
 */
#define HOST_VENDOR_BURST_GAP_US 5000LL
#define HOST_VENDOR_BURST_MIN_COUNT 4U
#define HOST_VENDOR_BURST_HOLD_US 50000LL
static volatile int64_t s_vendor_last_request_us;
static volatile uint32_t s_vendor_burst_count;
static volatile int64_t s_vendor_burst_last_us;

/* 到达版突发状态（供让路判据使用）。 */
static volatile int64_t s_vendor_arrival_last_us;
static volatile uint32_t s_vendor_arrival_count;
static volatile int64_t s_vendor_arrival_burst_us;
/* 观测：厂商请求到达间隔的最小值，用来判断"究竟有没有密集突发"。 */
static volatile int64_t s_vendor_arrival_min_gap_us;

static void host_vendor_note_arrival(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t previous = s_vendor_arrival_last_us;
    s_vendor_arrival_last_us = now;
    if (previous != 0) {
        const int64_t gap = now - previous;
        if (s_vendor_arrival_min_gap_us == 0 || gap < s_vendor_arrival_min_gap_us) {
            s_vendor_arrival_min_gap_us = gap;
        }
    }
    if (previous != 0 && now - previous < HOST_VENDOR_BURST_GAP_US) {
        if (s_vendor_arrival_count < 100000U) {
            ++s_vendor_arrival_count;
        }
    } else {
        s_vendor_arrival_count = 1U;
    }
    if (s_vendor_arrival_count >= 3U) {
        s_vendor_arrival_burst_us = now;
    }
}

static void host_vendor_note_request(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t previous = s_vendor_last_request_us;
    s_vendor_last_request_us = now;
    if (previous != 0 && now - previous < HOST_VENDOR_BURST_GAP_US) {
        if (s_vendor_burst_count < 100000U) {
            ++s_vendor_burst_count;
        }
    } else {
        s_vendor_burst_count = 1U;
    }
    if (s_vendor_burst_count >= HOST_VENDOR_BURST_MIN_COUNT) {
        s_vendor_burst_last_us = now;
    }
}
static dual_physical_release_callback_t s_release_callback;
static volatile uint32_t s_reports;
static volatile uint32_t s_errors;
static uint8_t s_logged_device_addr = UINT8_MAX;
static hid_device_profile_t s_profile_build;
static hid_device_profile_t s_profile_snapshot;
static uint8_t s_profile_serialized_blob[HID_PROFILE_MAX_BLOB];
static uint8_t s_profile_serialized_staging[HID_PROFILE_MAX_BLOB];
static size_t s_profile_serialized_length;
static uint8_t s_profile_device_addr = UINT8_MAX;
static volatile uint32_t s_profile_revision;
static volatile bool s_profile_refresh_requested;
static TaskHandle_t s_profile_task;
static SemaphoreHandle_t s_profile_mutex;
static SemaphoreHandle_t s_vendor_session_state_mutex;
static usb_host_client_handle_t s_descriptor_client;
static TaskHandle_t s_usb_host_task;
static TaskHandle_t s_descriptor_task;
static TaskHandle_t s_hid_event_task;
static TaskHandle_t s_report_task;
static hid_interface_slot_t s_interface_slots[HID_INTERFACE_SLOT_COUNT];
static QueueHandle_t s_control_queue;
static TaskHandle_t s_control_task;
static TaskHandle_t s_stats_task;
static TaskHandle_t s_stall_watch_task;
static EventGroupHandle_t s_task_events;
static EventBits_t s_worker_task_mask;
static portMUX_TYPE s_interface_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_host_task_created;
static bool s_usb_host_installed;
static bool s_hid_host_installed;
static bool s_descriptor_client_registered;
static bool s_root_port_powered;
static bool s_free_all_requested;
static volatile bool s_stopping;
static volatile bool s_host_lib_stop_requested;
static volatile bool s_lifecycle_busy;
static volatile uint32_t s_control_api_users;
static volatile uint32_t s_vendor_reports;
static volatile uint32_t s_vendor_input_failures;
static volatile uint32_t s_vendor_control_requests;
static volatile uint32_t s_vendor_control_failures;
static volatile bool s_device_present;
static volatile bool s_mouse_present;

#define HID_STATS_PERIOD_MS 5000

static bool stopping_requested(void)
{
    return __atomic_load_n(&s_stopping, __ATOMIC_ACQUIRE);
}

static void finish_owned_task(EventBits_t stopped_bit)
{
    if (s_task_events != NULL) {
        (void)xEventGroupSetBits(s_task_events, stopped_bit);
    }
    vTaskDelete(NULL);
}

static bool try_lifecycle_lock(void)
{
    return !__atomic_exchange_n(&s_lifecycle_busy, true, __ATOMIC_ACQUIRE);
}

static void release_lifecycle_lock(void)
{
    __atomic_store_n(&s_lifecycle_busy, false, __ATOMIC_RELEASE);
}

static void wide_ascii_copy(char output[HID_STR_DESC_MAX_LENGTH], const wchar_t *input)
{
    size_t index = 0;
    if (input != NULL) {
        while (index + 1 < HID_STR_DESC_MAX_LENGTH && input[index] != 0) {
            const wchar_t value = input[index];
            output[index] = value >= 0x20 && value <= 0x7e ? (char)value : '?';
            ++index;
        }
    }
    output[index] = '\0';
}

static size_t wide_utf8_copy(char *output, size_t capacity,
                             const wchar_t *input, bool *lossy)
{
    size_t written = 0;
    if (lossy != NULL) {
        *lossy = false;
    }
    if (output == NULL || capacity == 0) {
        return 0;
    }
    for (size_t index = 0; input != NULL && input[index] != 0; ++index) {
        uint32_t codepoint = (uint32_t)input[index];
        if (codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            codepoint = '?';
            if (lossy != NULL) {
                *lossy = true;
            }
        }
        const size_t needed = codepoint <= 0x7FU ? 1U :
            codepoint <= 0x7FFU ? 2U : codepoint <= 0xFFFFU ? 3U : 4U;
        if (written + needed >= capacity) {
            if (lossy != NULL) {
                *lossy = true;
            }
            break;
        }
        if (needed == 1U) {
            output[written++] = (char)codepoint;
        } else if (needed == 2U) {
            output[written++] = (char)(0xC0U | (codepoint >> 6));
            output[written++] = (char)(0x80U | (codepoint & 0x3FU));
        } else if (needed == 3U) {
            output[written++] = (char)(0xE0U | (codepoint >> 12));
            output[written++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            output[written++] = (char)(0x80U | (codepoint & 0x3FU));
        } else {
            output[written++] = (char)(0xF0U | (codepoint >> 18));
            output[written++] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
            output[written++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            output[written++] = (char)(0x80U | (codepoint & 0x3FU));
        }
    }
    output[written] = '\0';
    return written;
}

static hid_interface_slot_t *find_interface_slot(uint8_t interface_number)
{
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        if (s_interface_slots[index].active &&
            s_interface_slots[index].interface_number == interface_number) {
            return &s_interface_slots[index];
        }
    }
    return NULL;
}

static hid_interface_slot_t *find_interface_slot_by_handle(
    hid_host_device_handle_t handle)
{
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        if (s_interface_slots[index].active &&
            s_interface_slots[index].handle == handle) {
            return &s_interface_slots[index];
        }
    }
    return NULL;
}

static hid_interface_slot_t *allocate_interface_slot(uint8_t interface_number)
{
    hid_interface_slot_t *slot = find_interface_slot(interface_number);
    if (slot != NULL) {
        return NULL;
    }
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        if (!s_interface_slots[index].active) {
            memset(&s_interface_slots[index], 0, sizeof(s_interface_slots[index]));
            s_interface_slots[index].active = true;
            s_interface_slots[index].interface_number = interface_number;
            return &s_interface_slots[index];
        }
    }
    return NULL;
}

static void clear_interface_slot(uint8_t interface_number)
{
    hid_interface_slot_t *slot = find_interface_slot(interface_number);
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

/* These slot helpers are called with s_interface_state_mux held. */
static bool any_interface_active_locked(void)
{
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        if (s_interface_slots[index].active) {
            return true;
        }
    }
    return false;
}

static bool any_started_mouse_interface_locked(void)
{
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        if (s_interface_slots[index].active &&
            s_interface_slots[index].started &&
            s_interface_slots[index].mouse_interface) {
            return true;
        }
    }
    return false;
}

static void refresh_mouse_present_locked(void)
{
    __atomic_store_n(&s_mouse_present, any_started_mouse_interface_locked(),
                     __ATOMIC_RELEASE);
}

static bool report_descriptor_has_report_id(const uint8_t *descriptor, size_t length)
{
    size_t offset = 0;
    while (descriptor != NULL && offset < length) {
        const uint8_t prefix = descriptor[offset++];
        if (prefix == 0xFEU) {
            if (length - offset < 2U) {
                return false;
            }
            const size_t long_length = descriptor[offset];
            offset += 2U;
            if (long_length > length - offset) {
                return false;
            }
            offset += long_length;
            continue;
        }
        const uint8_t size_code = prefix & 0x03U;
        const size_t item_length = size_code == 3U ? 4U : size_code;
        const uint8_t item_type = (prefix >> 2) & 0x03U;
        const uint8_t item_tag = prefix & 0xFCU;
        if (item_type == 1U && item_tag == 0x84U) {
            return true;
        }
        if (item_length > length - offset) {
            return false;
        }
        offset += item_length;
    }
    return false;
}

static void profile_reset_collector(void)
{
    if (s_profile_mutex != NULL) {
        xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
    }
    s_profile_device_addr = UINT8_MAX;
    s_profile_serialized_length = 0U;
    ++s_profile_revision;
    hid_device_profile_init(&s_profile_build);
    memset(s_interface_slots, 0, sizeof(s_interface_slots));
    if (s_control_queue != NULL) {
        queue_reset_count_dropped(s_control_queue, &s_control_queue_metrics);
    }
    if (s_profile_mutex != NULL) {
        xSemaphoreGive(s_profile_mutex);
    }
}

static void profile_collect_identity(const hid_host_dev_params_t *params,
                                     const hid_host_dev_info_t *info)
{
    if (params == NULL) {
        return;
    }
    xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
    if (s_profile_device_addr != params->addr) {
        s_profile_device_addr = UINT8_MAX;
        ++s_profile_revision;
        hid_device_profile_init(&s_profile_build);
        s_profile_device_addr = params->addr;
        s_profile_build.flags = HID_PROFILE_FLAG_PARTIAL;
    }
    if (info == NULL) {
        s_profile_build.flags |= HID_PROFILE_FLAG_PARTIAL;
        xSemaphoreGive(s_profile_mutex);
        return;
    }
    if (s_profile_build.device_descriptor.length == 0 &&
        !hid_device_profile_set_synthetic_identity(&s_profile_build, info->VID, info->PID)) {
        s_profile_build.flags |= HID_PROFILE_FLAG_PARTIAL;
    }
    char text[HID_PROFILE_MAX_STRING_BYTES + 1U];
    bool lossy = false;
    bool any_lossy = false;
    size_t length = wide_utf8_copy(text, sizeof(text), info->iManufacturer, &lossy);
    any_lossy |= lossy;
    if (!hid_device_profile_set_string(&s_profile_build.manufacturer, text, length)) {
        s_profile_build.flags |= HID_PROFILE_FLAG_PARTIAL;
    }
    length = wide_utf8_copy(text, sizeof(text), info->iProduct, &lossy);
    any_lossy |= lossy;
    if (!hid_device_profile_set_string(&s_profile_build.product, text, length)) {
        s_profile_build.flags |= HID_PROFILE_FLAG_PARTIAL;
    }
    length = wide_utf8_copy(text, sizeof(text), info->iSerialNumber, &lossy);
    any_lossy |= lossy;
    if (!hid_device_profile_set_string(&s_profile_build.serial, text, length)) {
        s_profile_build.flags |= HID_PROFILE_FLAG_PARTIAL;
    }
    if (any_lossy) {
        s_profile_build.flags |= HID_PROFILE_FLAG_PARTIAL;
    }
    xSemaphoreGive(s_profile_mutex);
}

static bool profile_collect_raw_usb_descriptors(uint8_t device_addr)
{
    if (s_descriptor_client == NULL) {
        return false;
    }
    bool collected = false;
    usb_device_handle_t device = NULL;
    const usb_device_desc_t *device_descriptor = NULL;
    const usb_config_desc_t *config_descriptor = NULL;
    esp_err_t result = usb_host_device_open(
        s_descriptor_client, device_addr, &device);
    if (result == ESP_OK) {
        result = usb_host_get_device_descriptor(device, &device_descriptor);
    }
    if (result == ESP_OK) {
        result = usb_host_get_active_config_descriptor(device, &config_descriptor);
    }
    if (result == ESP_OK && device_descriptor != NULL && config_descriptor != NULL &&
        config_descriptor->wTotalLength >= sizeof(*config_descriptor) &&
        config_descriptor->wTotalLength <= HID_PROFILE_MAX_CONFIG_DESCRIPTOR) {
        xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
        const bool device_ok = hid_device_profile_set_device_descriptor(
            &s_profile_build, (const uint8_t *)device_descriptor,
            sizeof(*device_descriptor));
        const bool config_ok = hid_device_profile_set_config_descriptor(
            &s_profile_build, (const uint8_t *)config_descriptor,
            config_descriptor->wTotalLength);
        if (device_ok && config_ok) {
            collected = true;
            s_profile_build.flags &= (uint8_t)~(
                HID_PROFILE_FLAG_SYNTHETIC_DEVICE_DESCRIPTOR |
                HID_PROFILE_FLAG_SYNTHETIC_CONFIG_DESCRIPTOR);
            ESP_LOGI(TAG, "已采集原始USB描述符：device=%u config=%u interfaces=%u",
                     (unsigned)sizeof(*device_descriptor),
                     (unsigned)config_descriptor->wTotalLength,
                     config_descriptor->bNumInterfaces);
        } else {
            s_profile_build.flags |= HID_PROFILE_FLAG_PARTIAL;
        }
        xSemaphoreGive(s_profile_mutex);
    } else {
        ++s_errors;
        ESP_LOGW(TAG, "原始USB描述符采集失败：addr=%u error=%s",
                 device_addr, esp_err_to_name(result));
    }
    if (device != NULL) {
        const esp_err_t close_result = usb_host_device_close(
            s_descriptor_client, device);
        if (close_result != ESP_OK) {
            ++s_errors;
            ESP_LOGW(TAG, "描述符客户端关闭设备失败：%s",
                     esp_err_to_name(close_result));
        }
    }
    if (!collected) {
        dual_status_led_set_flow_error(true);
    }
    return collected;
}

static void profile_collect_interface(const hid_host_dev_params_t *params,
                                      const hid_host_dev_info_t *info,
                                      const uint8_t *descriptor,
                                      size_t descriptor_length)
{
    if (params == NULL) {
        return;
    }
    profile_collect_identity(params, info);
    xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
    const bool needs_raw_descriptors =
        s_profile_build.config_descriptor.length == 0;
    xSemaphoreGive(s_profile_mutex);
    if (needs_raw_descriptors) {
        (void)profile_collect_raw_usb_descriptors(params->addr);
    }
    xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
    if (descriptor == NULL || descriptor_length == 0 ||
        descriptor_length > HID_PROFILE_MAX_REPORT_DESCRIPTOR ||
        !hid_device_profile_add_report_descriptor(
            &s_profile_build, params->iface_num, params->sub_class, params->proto,
            descriptor, descriptor_length)) {
        s_profile_build.flags |= HID_PROFILE_FLAG_PARTIAL;
        ++s_errors;
        ESP_LOGW(TAG, "Profile接口描述符未采集：interface=%u length=%u",
                 params->iface_num, (unsigned)descriptor_length);
        xSemaphoreGive(s_profile_mutex);
        return;
    }
    ++s_profile_revision;
    xSemaphoreGive(s_profile_mutex);
    if (s_profile_task != NULL) {
        xTaskNotifyGive(s_profile_task);
    }
}

static void profile_publish_task(void *argument)
{
    (void)argument;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (stopping_requested()) {
            break;
        }
        if (__atomic_exchange_n(&s_profile_refresh_requested, false,
                                __ATOMIC_ACQ_REL)) {
            xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
            const uint8_t refresh_addr = s_profile_device_addr;
            xSemaphoreGive(s_profile_mutex);
            if (refresh_addr != UINT8_MAX) {
                if (!profile_collect_raw_usb_descriptors(refresh_addr)) {
                    ESP_LOGE(TAG, "按电脑侧申请重新读取原始 USB 信息失败");
                    continue;
                }
            } else {
                dual_status_led_set_flow_error(true);
                continue;
            }
        }
        xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
        const uint32_t revision = s_profile_revision;
        xSemaphoreGive(s_profile_mutex);
        vTaskDelay(pdMS_TO_TICKS(HID_PROFILE_DEBOUNCE_MS));
        if (stopping_requested()) {
            break;
        }
        xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
        if (revision == 0 || revision != s_profile_revision ||
            s_profile_device_addr == UINT8_MAX) {
            xSemaphoreGive(s_profile_mutex);
            continue;
        }
        s_profile_snapshot = s_profile_build;
        const uint8_t device_addr = s_profile_device_addr;
        xSemaphoreGive(s_profile_mutex);
        size_t blob_length = 0;
        if (!hid_device_profile_serialize(
                &s_profile_snapshot, s_profile_serialized_staging,
                sizeof(s_profile_serialized_staging), &blob_length)) {
            ++s_errors;
            dual_status_led_set_flow_error(true);
            ESP_LOGW(TAG, "Profile序列化失败：interfaces=%u partial=%s",
                     s_profile_snapshot.report_descriptor_count,
                     (s_profile_snapshot.flags & HID_PROFILE_FLAG_PARTIAL) != 0 ? "yes" : "no");
            continue;
        }
        const uint32_t crc32 = hid_profile_crc32(s_profile_serialized_staging, blob_length);
        if (stopping_requested()) {
            break;
        }
        xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
        if (revision != s_profile_revision) {
            xSemaphoreGive(s_profile_mutex);
            continue;
        }
        memcpy(s_profile_serialized_blob, s_profile_serialized_staging, blob_length);
        s_profile_serialized_length = blob_length;
        const esp_err_t queued = dual_uart1_queue_profile(
            s_profile_serialized_blob, blob_length, crc32);
        xSemaphoreGive(s_profile_mutex);
        if (queued != ESP_OK) {
            ++s_errors;
            dual_status_led_set_flow_error(true);
            ESP_LOGW(TAG, "Profile排队发送失败：length=%u", (unsigned)blob_length);
        } else {
            dual_status_led_set_flow_error(false);
            /* 保持 WAITING，直到电脑侧安装克隆并回传匹配 ACK。 */
            ESP_LOGI(TAG, "Profile观察快照已排队：addr=%u interfaces=%u length=%u partial=%s",
                     device_addr, s_profile_snapshot.report_descriptor_count,
                     (unsigned)blob_length,
                     (s_profile_snapshot.flags & HID_PROFILE_FLAG_PARTIAL) != 0 ? "yes" : "no");
        }
    }
    finish_owned_task(TASK_EXIT_PROFILE);
}

esp_err_t dual_hid_host_request_profile_refresh(void)
{
    if (s_profile_mutex == NULL || s_profile_task == NULL ||
        !__atomic_load_n(&s_device_present, __ATOMIC_ACQUIRE)) {
        dual_status_led_set_flow_error(true);
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
    const uint8_t device_addr = s_profile_device_addr;
    const bool available = device_addr != UINT8_MAX &&
        s_profile_build.report_descriptor_count != 0U;
    xSemaphoreGive(s_profile_mutex);
    if (!available) {
        dual_status_led_set_flow_error(true);
        return ESP_ERR_INVALID_STATE;
    }

    /* 发布任务中重新读取原始 USB 描述符，避免 UART RX 回调被 USB API
     * 阻塞；仍在线的接口提供 Report Descriptor 快照。 */
    __atomic_store_n(&s_profile_refresh_requested, true, __ATOMIC_RELEASE);
    xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
    ++s_profile_revision;
    xSemaphoreGive(s_profile_mutex);
    dual_status_led_set_flow_error(false);
    xTaskNotifyGive(s_profile_task);
    return ESP_OK;
}

static void log_device_identity_once(
    uint8_t address,
    const hid_host_dev_info_t *info)
{
    if (info == NULL || s_logged_device_addr == address) {
        return;
    }
    char manufacturer[HID_STR_DESC_MAX_LENGTH];
    char product[HID_STR_DESC_MAX_LENGTH];
    wide_ascii_copy(manufacturer, info->iManufacturer);
    wide_ascii_copy(product, info->iProduct);
    ESP_LOGI(TAG,
             "物理HID身份：addr=%u VID:PID=%04X:%04X manufacturer='%s' product='%s' serial_present=%s",
             address, info->VID, info->PID, manufacturer, product,
             info->iSerialNumber[0] != 0 ? "yes" : "no");
    s_logged_device_addr = address;
}

static void log_report_descriptor(
    hid_host_device_handle_t handle,
    uint8_t interface_number)
{
    size_t descriptor_length = 0;
    uint8_t *descriptor = hid_host_get_report_descriptor(handle, &descriptor_length);
    if (descriptor == NULL || descriptor_length == 0) {
        ++s_errors;
        ESP_LOGW(TAG, "HID接口%u没有可读报告描述符", interface_number);
        return;
    }
    ESP_LOGI(TAG, "HID接口%u报告描述符 length=%u hex如下",
             interface_number, (unsigned)descriptor_length);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, descriptor, descriptor_length, ESP_LOG_INFO);
}

/* 上一次鼠标报文首字节（按键位）：仅用于按键边沿打点。 */
static uint8_t s_last_button_byte;
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API || DUAL_PROXY_ENABLE_MAKCU_V4_API
static volatile uint8_t s_makcu_physical_buttons;

uint8_t dual_hid_host_makcu_physical_buttons(void)
{
    return __atomic_load_n(&s_makcu_physical_buttons, __ATOMIC_ACQUIRE);
}
#endif

static void notify_release_callback(bool device_gone)
{
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API || DUAL_PROXY_ENABLE_MAKCU_V4_API
    __atomic_store_n(&s_makcu_physical_buttons, 0U, __ATOMIC_RELEASE);
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
    dual_uart0_control_v4_physical_buttons(0U);
    dual_uart0_control_v4_cancel_input();
#endif
#endif
    if (s_release_callback != NULL) {
        s_release_callback(device_gone);
    }
}
/* 滚轮字节偏移（相对报表正文，不含 report ID）；0xFF 表示未知。 */
static volatile uint8_t s_wheel_byte_offset = 0xFFU;
static volatile uint32_t s_wheel_reports;
/* 控制传输重试与异常记录。 */
#define VENDOR_CONTROL_RETRIES 2U
#define VENDOR_CONTROL_RETRY_DELAY_MS 3U
#define HID_GET_REPORT_TIMEOUT_MS 100U /* 两次尝试合计不超过 P 侧 250 ms 等待 */
#define HID_RECOVER_FAIL_STREAK 5U
static volatile uint32_t s_vendor_control_retries;
static volatile uint32_t s_ctrl_fail_streak;
/* 直连通道连续失败计数（与组件路径分开统计）。 */
static volatile uint32_t s_vendor_urb_fail_streak;
/* 自动恢复只记录故障并释放卡住的按钮；不重挂接口或切断物理鼠标供电。 */
#define USB_HOST_AUTOMATIC_DESTRUCTIVE_RECOVERY 0
/* 心跳失败数供诊断；恢复判据使用未完成心跳 URB，不用失败次数代替。 */
static volatile uint32_t s_probe_fail_streak;
/*
 * USB 心跳探测：约每 100 ms 主动向设备发一笔最轻量的标准请求，
 * 成败直接喂给 s_last_ctrl_ok_us / s_last_ctrl_fail_us，于是上面这套"数据冻结判据"
 * 不必再等外部软件碰巧发控制请求。
 * 起因：现场实测 13:58 那次冻结期间**没有任何控制请求发出**，判据白等了 42 秒才成立。
 */
#define USB_HEARTBEAT_PERIOD_US    (100 * 1000LL)        /* 探测周期 */
#define USB_HEARTBEAT_TIMEOUT_MS   (200U)                /* 单次探测超时（正常控制传输 ≤1.7 ms，200 ms 仍留百倍余量） */
static volatile int64_t s_last_heartbeat_us;
static volatile uint32_t s_heartbeat_ok_count;
static volatile uint32_t s_heartbeat_fail_count;
static volatile int64_t s_last_report_us;      /* 最近一次收到物理鼠标接口报告 */
/*
 * 「M 收到的位移」统计（2026-09-27）：把物理鼠标报告里的相对位移累加起来，
 * 与 P 侧「输出的位移」对比即可看出桥接层有没有吞掉/放大位移（帧计数看不出来）。
 */
static volatile int64_t s_motion_rx_dx;
static volatile int64_t s_motion_rx_dy;
/* 解析诊断（2026-09-27）：定位"两侧位移不一致"究竟来自解析失败还是真的丢位移。 */
static volatile uint32_t s_motion_rx_ok;
static volatile uint32_t s_motion_rx_bad_length;
static volatile uint32_t s_motion_rx_bad_parse;
/*
 * 诊断注入缓冲（2026-09-27）：把一段原始鼠标报告"伪装"成刚从 USB 收到的样子，
 * 让 RX 回调（位移统计 → 入队 → 转发）走与物理报告**完全相同**的路径。
 * 用途：用完全已知的位移做受控实验，判断 M/P 两侧位移统计究竟哪一侧不准。
 */
static uint8_t s_injected_report[HID_RAW_REPORT_MAX];
static size_t s_injected_report_length;
static volatile bool s_injected_report_pending;
static volatile int64_t s_last_ctrl_ok_us;     /* 最近一次控制传输成功 */
static volatile int64_t s_last_ctrl_fail_us;   /* 最近一次控制传输失败 */
static volatile int64_t s_stall_recovery_us;   /* 最近一次因数据冻结触发的恢复 */
/* 鼠标接口就绪时刻：用于判定枚举后的预热期。 */
static volatile int64_t s_mouse_ready_us;
static volatile uint32_t s_vendor_session_epoch;
static volatile uint32_t s_vendor_session_peer_generation;
static volatile int64_t s_vendor_session_first_request_us;
#define VENDOR_URB_RECOVER_STREAK 3U
/*
 * 枚举后预热期：实测同一场景下超时次数随等待时间单调下降（600 ms→8~13 次、
 * 800 ms→18 次、2500 ms→3 次），说明设备刚枚举完时首批 HID++ 应答本身就慢，
 * 不是卡死。因此预热期内放宽等待，正常期维持 800 ms 的严格判据。
 */
#define VENDOR_URB_WARMUP_US 10000000LL
#define VENDOR_URB_WARMUP_TIMEOUT_MS 2500U
#define VENDOR_URB_TIMEOUT_MS 800U

static bool vendor_control_event_session_is_current(const hid_control_event_t *event)
{
    return event != NULL && event->vendor_session_epoch != 0U &&
        event->vendor_session_epoch == __atomic_load_n(
            &s_vendor_session_epoch, __ATOMIC_ACQUIRE) &&
        event->vendor_session_peer_generation == __atomic_load_n(
            &s_vendor_session_peer_generation, __ATOMIC_ACQUIRE);
}

/* 在 URB 结束后短暂持锁校验并入队，BEGIN不能夹在校验与旧响应入队之间。 */
static bool vendor_control_event_lock_if_current(const hid_control_event_t *event)
{
    if (s_vendor_session_state_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_vendor_session_state_mutex, portMAX_DELAY);
    if (!vendor_control_event_session_is_current(event)) {
        xSemaphoreGive(s_vendor_session_state_mutex);
        return false;
    }
    return true;
}

static int64_t vendor_session_note_first_request(uint32_t epoch)
{
    if (epoch == 0U || s_vendor_session_state_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_vendor_session_state_mutex, portMAX_DELAY);
    if (epoch != __atomic_load_n(&s_vendor_session_epoch, __ATOMIC_ACQUIRE)) {
        xSemaphoreGive(s_vendor_session_state_mutex);
        return 0;
    }
    int64_t first = __atomic_load_n(&s_vendor_session_first_request_us, __ATOMIC_RELAXED);
    if (first == 0) {
        const int64_t now = esp_timer_get_time();
        first = now;
        __atomic_store_n(&s_vendor_session_first_request_us, first, __ATOMIC_RELEASE);
    }
    xSemaphoreGive(s_vendor_session_state_mutex);
    return first;
}

static bool vendor_session_request_is_warmup(uint32_t epoch, int64_t now_us)
{
    const int64_t first = vendor_session_note_first_request(epoch);
    return first != 0 && now_us >= first && now_us - first < VENDOR_URB_WARMUP_US;
}

static volatile uint32_t s_hid_recoveries;
static volatile bool s_hid_recover_requested;
/* 二级恢复：根端口断电再上电（等价于拔插一次 USB）。一级重挂若被"在飞传输"挡住
 * （start 返回 ESP_ERR_NOT_FINISHED），设备会保持沉默，这时只有端口级功率循环
 * 或整体重启主机栈才能恢复——实测手动拔插鼠标与复位 M 都属这一类。 */
#if USB_HOST_AUTOMATIC_DESTRUCTIVE_RECOVERY
static volatile bool s_root_port_cycle_requested;
#endif
static volatile uint32_t s_cycle_requests, s_cycle_ok, s_cycle_failed;
static volatile uint32_t s_cycle_off_fail, s_cycle_on_fail, s_cycle_suppressed;
static volatile uint32_t s_cycle_input_restored, s_cycle_input_missing, s_cycle_slow;
static volatile int64_t s_cycle_delay_max_us;
static int64_t s_cycle_input_wait_us;
static volatile uint32_t s_root_port_cycles;
static const char *s_hid_recover_reason;

static void raw_report_task(void *argument)
{
    (void)argument;
    raw_report_event_t event;
    while (true) {
        if (xQueueReceive(s_report_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.stop) {
            break;
        }
        if (stopping_requested()) {
            continue;
        }
        dual_diag_stream_record(DUAL_DIAG_SOURCE_M_USB,
                                DUAL_DIAG_KIND_M_RAW_HID_INPUT,
                                event.data, event.length);
        if (event.mouse_report) {
            const uint8_t *axis_report = event.data;
            size_t axis_length = event.length;
            uint8_t axis_report_id = 0U;
            if (event.has_report_id && axis_length > 0U) {
                axis_report_id = event.data[0];
                axis_report = &event.data[1];
                --axis_length;
            }
            if (!event.has_report_id || axis_report_id == event.mouse_report_id) {
                int32_t axis_x = 0, axis_y = 0, axis_wheel = 0, axis_pan = 0;
                if (axis_length != event.mouse_layout.report_bytes) {
                    __atomic_add_fetch(&s_motion_rx_bad_length, 1U, __ATOMIC_RELAXED);
                } else {
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API || DUAL_PROXY_ENABLE_MAKCU_V4_API
                    uint8_t physical_buttons = 0U;
                    if (hid_mouse_report_read_buttons(
                            axis_report, axis_length, &event.mouse_layout,
                            &physical_buttons)) {
                        __atomic_store_n(&s_makcu_physical_buttons,
                                         physical_buttons, __ATOMIC_RELEASE);
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
                        if (!event.injected) {
                            dual_uart0_control_v4_physical_buttons(physical_buttons);
                        }
#endif
                    }
#endif
                    if (hid_mouse_report_read_axes(axis_report, axis_length,
                                                   &event.mouse_layout, &axis_x, &axis_y,
                                                   &axis_wheel, &axis_pan)) {
                        __atomic_add_fetch(&s_motion_rx_dx, axis_x, __ATOMIC_RELAXED);
                        __atomic_add_fetch(&s_motion_rx_dy, axis_y, __ATOMIC_RELAXED);
                        const uint32_t sample_index = __atomic_add_fetch(
                            &s_motion_rx_ok, 1U, __ATOMIC_RELAXED);
                        if (sample_index <= 10U || (sample_index % 1000U) == 0U) {
                            ESP_LOGI(TAG,
                                     "M样本#%u 原始=%u字节 剥离后=%u字节 布局=%u字节 dx=%d dy=%d",
                                     (unsigned)sample_index, (unsigned)event.length,
                                     (unsigned)axis_length,
                                     (unsigned)event.mouse_layout.report_bytes,
                                     (int)axis_x, (int)axis_y);
                            ESP_LOG_BUFFER_HEX_LEVEL(TAG, axis_report, axis_length, ESP_LOG_INFO);
                        }
                    } else {
                        __atomic_add_fetch(&s_motion_rx_bad_parse, 1U, __ATOMIC_RELAXED);
                    }
                }
            }
        }
        if (event.has_report_id) {
            if (event.length == 0U) {
                ++s_errors;
                queue_metric_increment(&s_report_queue_metrics.rejected);
                continue;
            }
            event.report_id = event.data[0];
            --event.length;
            memmove(event.data, &event.data[1], event.length);
        }
        event.mouse_report = event.mouse_report &&
            event.report_id == event.mouse_report_id;
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
        if (event.mouse_report) {
            uint8_t button_mask = 0U;
            uint8_t move_mask = 0U;
            uint8_t wheel_mask = 0U;
            dual_uart0_control_v4_get_physical_masks(
                &button_mask, &move_mask, &wheel_mask);
            if (!hid_mouse_report_apply_physical_masks(
                    &event.mouse_layout, event.data, event.length,
                    button_mask, move_mask, wheel_mask)) {
                ++s_errors;
                ++s_vendor_input_failures;
                continue;
            }
        }
#endif
        if (event.length > DUAL_HID_RAW_INPUT_MAX_DATA) {
            ++s_errors;
            ++s_vendor_input_failures;
            queue_metric_increment(&s_report_queue_metrics.rejected);
            continue;
        }
        /*
         * 按键边沿打点（点击延迟测量用）：物理鼠标的移动/按键走的是原始透传路径
         * （不在 on_mouse_report 里，那条回调在本版本根本不会被调用），所以打点
         * 必须放在这里。只在按键字节变化时打印，人工点击频率下不会造成日志洪峰。
         */
        if (event.mouse_report && s_wheel_byte_offset != 0xFFU &&
            s_wheel_byte_offset < event.length && event.data[s_wheel_byte_offset] != 0U) {
            /* 滚轮报文计数：用于"超时期间设备输入通道是否还活着"的观测。 */
            ++s_wheel_reports;
        }
        if (event.mouse_report && event.length > 0U) {
            if (event.data[0] != s_last_button_byte) {
                s_last_button_byte = event.data[0];
                ESP_LOGI(TAG, "按键边沿：buttons=0x%02X（点击延迟测量打点）", event.data[0]);
            }
        }
        /*
         * 2026-09-27：按用户要求**移除全部纯移动抑制**。原先这里有两支让路：
         *   (1) 克隆完成前（`!dual_uart1_clone_ready()`）不转发纯移动；
         *   (2) 厂商事务密集突发期间（`dual_hid_host_vendor_busy()`，30 ms 窗）让路，
         *       用于保护 HID++ 控制传输（历史实测"不动时 168/168 全过，一动就超时"）。
         * 两支均已删除：任何时刻的纯移动都照常转发；带按键的报文一如既往不丢。
         * 通道上移动走 `s_vendor_tx_queue`（每 4 轮取一次），Profile 分片走 `s_tx_queue`
         * 且每轮优先取用（`uart1_link.c:1866-1889`），不存在"移动挤占克隆建立"的竞争。
         * 注意：本模块的 `on_mouse_report` 回调在物理路径上不会被调用，这里的判定才是
         * 生效路径——历史教训是"只改那边等于没生效"。
         */
        const esp_err_t forward_result = event.length == 0U
            ? ESP_ERR_INVALID_SIZE
            : dual_uart1_send_raw_hid_input(
                  event.interface_number, event.report_id, event.data,
                  event.length);
        if (forward_result != ESP_OK) {
            ++s_errors;
            ++s_vendor_input_failures;
        } else if (event.mouse_report) {
#if DUAL_PROXY_ENABLE_MAKCU_V4_API
            int32_t x = 0, y = 0, wheel = 0, pan = 0;
            if (hid_mouse_report_read_axes(
                    event.data, event.length, &event.mouse_layout,
                    &x, &y, &wheel, &pan)) {
                dual_uart0_control_v4_track_physical_move(x, y);
            }
#endif
            ++s_reports;
        } else {
            ++s_vendor_reports;
        }
    }
    finish_owned_task(TASK_EXIT_REPORT);
}

/* 连续控制失败交给事件任务记录；默认不重挂仍在工作的输入接口。 */
static void request_hid_device_recovery(const char *reason)
{
    ++s_ctrl_fail_streak;
    if (s_ctrl_fail_streak < HID_RECOVER_FAIL_STREAK || s_hid_recover_requested) {
        if (s_ctrl_fail_streak == HID_RECOVER_FAIL_STREAK) {
            ESP_LOGW(TAG, "控制传输连续失败 %u 次（%s）：请求重挂 HID 设备",
                     (unsigned)s_ctrl_fail_streak, reason != NULL ? reason : "");
        }
        return;
    }
    s_hid_recover_reason = reason;
    s_hid_recover_requested = true;
}

static void hid_control_task(void *argument)
{
    (void)argument;
    hid_control_event_t request;
    uint8_t response[DUAL_HID_CONTROL_MAX_DATA];
    while (true) {
        if (xQueueReceive(s_control_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (request.stop) {
            break;
        }
        if (stopping_requested()) {
            continue;
        }
        if (!vendor_control_event_session_is_current(&request)) {
            /* BEGIN 已使旧请求失效；旧响应不得进入新的 PC Vendor 会话。 */
            continue;
        }
        (void)vendor_session_note_first_request(request.vendor_session_epoch);
        /* G HUB 的初始化是成组 SET/GET，期间移动报文让路。 */
        host_vendor_note_request();
        if (request.vendor_control) {
            /*
             * 设备级 Vendor 控制请求：与具体 HID 接口无关（wIndex 由主机任意指定），
             * 所以不能用 slot 过滤——先取出任一活动接口的 handle，再用它的设备地址
             * 走通用 EP0 通道，最后把状态与（IN 方向的）数据回给 P。
             * 注意：组件查询不能放在临界区内，这里只取 handle。
             */
            hid_host_device_handle_t vendor_handle = NULL;
            portENTER_CRITICAL(&s_interface_state_mux);
            for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
                const hid_interface_slot_t *candidate = &s_interface_slots[index];
                if (candidate->active && candidate->handle != NULL) {
                    vendor_handle = candidate->handle;
                    break;
                }
            }
            portEXIT_CRITICAL(&s_interface_state_mux);
            hid_host_dev_params_t vendor_params;
            if (vendor_handle == NULL ||
                hid_host_device_get_params(vendor_handle, &vendor_params) != ESP_OK) {
                ++s_vendor_control_failures;
                if (vendor_control_event_lock_if_current(&request)) {
                    (void)dual_uart1_send_vendor_control_response(
                        request.transaction_id, DUAL_HID_REPORT_STATUS_INVALID, NULL, 0);
                    xSemaphoreGive(s_vendor_session_state_mutex);
                }
                continue;
            }
            uint8_t vendor_response[DUAL_VENDOR_CONTROL_MAX_DATA];
            size_t vendor_response_length = 0;
            const int64_t vendor_now_us = esp_timer_get_time();
            const bool vendor_warmup = vendor_session_request_is_warmup(
                request.vendor_session_epoch, vendor_now_us);
            const bool vendor_is_in = (request.bm_request_type & 0x80U) != 0U;
            const esp_err_t vendor_result = dual_vendor_urb_control(
                vendor_params.addr, request.bm_request_type, request.b_request,
                request.w_value, request.w_index,
                /* IN 不带数据，长度取"期望读取长度"；OUT 传数据与其长度。 */
                vendor_is_in ? NULL : request.data,
                vendor_is_in ? request.w_length : request.length,
                vendor_warmup ? VENDOR_URB_WARMUP_TIMEOUT_MS : VENDOR_URB_TIMEOUT_MS,
                vendor_response, sizeof(vendor_response), &vendor_response_length);
            const uint8_t vendor_status = vendor_result == ESP_OK
                ? DUAL_HID_REPORT_STATUS_OK
                : (vendor_result == ESP_ERR_TIMEOUT ? DUAL_HID_REPORT_STATUS_TIMEOUT
                                                    : DUAL_HID_REPORT_STATUS_UNSUPPORTED);
            if (vendor_result == ESP_OK) {
                s_vendor_urb_fail_streak = 0;
                __atomic_store_n(&s_last_ctrl_ok_us, esp_timer_get_time(), __ATOMIC_RELEASE);
            } else {
                /* 与 SET_REPORT 同源：失败时刻供"数据冻结"判据使用。 */
                ++s_vendor_control_failures;
                __atomic_store_n(&s_last_ctrl_fail_us, esp_timer_get_time(), __ATOMIC_RELEASE);
            }
            ESP_LOGI(TAG,
                     "Vendor 控制请求：bm=%02X req=%02X value=%04X index=%04X len=%u result=%s",
                     request.bm_request_type, request.b_request, request.w_value,
                     request.w_index, request.length, esp_err_to_name(vendor_result));
            if (vendor_control_event_lock_if_current(&request)) {
                (void)dual_uart1_send_vendor_control_response(
                    request.transaction_id, vendor_status,
                    vendor_status == DUAL_HID_REPORT_STATUS_OK ? vendor_response : NULL,
                    vendor_status == DUAL_HID_REPORT_STATUS_OK ? vendor_response_length : 0U);
                xSemaphoreGive(s_vendor_session_state_mutex);
            }
            continue;
        }
        hid_interface_slot_t *slot = find_interface_slot(request.interface_number);
        if (slot == NULL || !slot->active || slot->handle == NULL) {
            ++s_vendor_control_failures;
            if (request.get_report && vendor_control_event_lock_if_current(&request)) {
                (void)dual_uart1_send_hid_get_response(
                    request.transaction_id, DUAL_HID_REPORT_STATUS_INVALID,
                    request.interface_number, request.report_id, NULL, 0);
                xSemaphoreGive(s_vendor_session_state_mutex);
            }
            continue;
        }

        if (request.get_report) {
            /*
             * TinyUSB 在非零 report ID 的 GET_REPORT 回调外层自动预留并补上
             * report ID；物理设备端的控制传输则仍可能把 report ID 放在首字节。
             * 多请求一个字节并在回传前去重，保证电脑最终只收到一个 ID。
             */
            size_t response_length = request.requested_length;
            if (request.report_id != 0U &&
                response_length < DUAL_HID_CONTROL_MAX_DATA) {
                ++response_length;
            }
            esp_err_t result;
#if VENDOR_USE_DIRECT_URB
            hid_host_dev_params_t get_params;
            size_t received_length = 0U;
            if (hid_host_device_get_params(slot->handle, &get_params) == ESP_OK) {
                result = dual_vendor_urb_control(
                    get_params.addr, 0xA1U, 0x01U,
                    (uint16_t)(((uint16_t)request.report_type << 8) | request.report_id),
                    request.interface_number, NULL, response_length,
                    HID_GET_REPORT_TIMEOUT_MS, response, sizeof(response), &received_length);
                response_length = received_length;
            } else {
                result = ESP_ERR_INVALID_STATE;
                response_length = 0U;
            }
#else
            result = hid_class_request_get_report(
                slot->handle, request.report_type, request.report_id,
                response, &response_length);
#endif
            const uint8_t status = result == ESP_OK
                ? DUAL_HID_REPORT_STATUS_OK : DUAL_HID_REPORT_STATUS_UNSUPPORTED;
            if (result == ESP_OK) {
                s_ctrl_fail_streak = 0;
                __atomic_store_n(&s_last_ctrl_ok_us, esp_timer_get_time(), __ATOMIC_RELEASE);
            } else {
                ++s_vendor_control_failures;
                __atomic_store_n(&s_last_ctrl_fail_us, esp_timer_get_time(), __ATOMIC_RELEASE);
                request_hid_device_recovery("GET_REPORT 失败");
            }
            ESP_LOGI(TAG,
                     "物理 GET_REPORT: interface=%u id=%02X type=%u requested=%u "
                     "result=%s length=%u",
                     request.interface_number, request.report_id,
                     request.report_type, request.requested_length,
                     esp_err_to_name(result), (unsigned)response_length);
            if (result == ESP_OK && response_length != 0U) {
                ESP_LOG_BUFFER_HEX_LEVEL(
                    TAG, response, response_length, ESP_LOG_INFO);
            }
            const uint8_t *response_data = response;
            if (status == DUAL_HID_REPORT_STATUS_OK &&
                request.report_id != 0U && response_length != 0U &&
                response[0] == request.report_id) {
                ++response_data;
                --response_length;
            }
            if (vendor_control_event_lock_if_current(&request)) {
                if (dual_uart1_send_hid_get_response(
                        request.transaction_id, status, request.interface_number,
                        request.report_id,
                        status == DUAL_HID_REPORT_STATUS_OK ? response_data : NULL,
                        status == DUAL_HID_REPORT_STATUS_OK ? response_length : 0) != ESP_OK) {
                    ++s_vendor_control_failures;
                }
                xSemaphoreGive(s_vendor_session_state_mutex);
            }
        } else {
            uint8_t mutable_data[DUAL_HID_CONTROL_MAX_DATA];
            size_t mutable_length = request.length;
            /*
             * TinyUSB 会在回调前剥离控制传输数据阶段开头的 report ID。
             * Logitech C092 对缺少该首字节的 SET_REPORT 会在 EP0 STALL，
             * 因此转发到物理设备前必须恢复原始 wire payload。
             */
            const bool restore_report_id = request.report_id != 0U &&
                request.length < sizeof(mutable_data);
            if (restore_report_id) {
                mutable_data[0] = request.report_id;
                memcpy(&mutable_data[1], request.data, request.length);
                ++mutable_length;
            } else {
                memcpy(mutable_data, request.data, request.length);
            }
            /*
             * 重试：设备偶尔会 NAK 掉一笔控制传输。实测现场（含控制台 1 Hz 统计
             * 佐证）表明"5 秒超时"期间开发板本身完全正常，是设备未完成该传输；
             * 而一次失败若不复位就会把后续所有提交都挡在"在飞对象"之外、
             * 导致整条厂商通道永久瘫痪。这里先就地重试，仍失败才升级为设备重挂。
             */
            esp_err_t result;
#if VENDOR_USE_DIRECT_URB
            /*
             * 直连通道：每个请求独立 URB，超时挂孤儿，不会像组件那样被一笔"在飞"对象
             * 锁死整条通道；等待时间区分预热期（枚举后 10 s 内 2500 ms）与正常期（800 ms）。
             */
            hid_host_dev_params_t direct_params;
            const int64_t now_us = esp_timer_get_time();
            const bool warmup = vendor_session_request_is_warmup(
                request.vendor_session_epoch, now_us);
            if (hid_host_device_get_params(slot->handle, &direct_params) == ESP_OK) {
                result = dual_vendor_urb_set_report(
                    direct_params.addr, request.interface_number, request.report_type,
                    request.report_id, mutable_data, mutable_length,
                    warmup ? VENDOR_URB_WARMUP_TIMEOUT_MS : VENDOR_URB_TIMEOUT_MS);
            } else {
                result = ESP_ERR_INVALID_STATE;
            }
            if (result == ESP_OK) {
                s_vendor_urb_fail_streak = 0;
                __atomic_store_n(&s_last_ctrl_ok_us, esp_timer_get_time(), __ATOMIC_RELEASE);
            } else if (warmup) {
                /*
                 * 预热期（枚举后 10 s 内）不升级恢复：实测此时的"超时"是设备初始化
                 * 期间应答变慢，而端口断电会**重新枚举**、让它再次进入慢阶段，
                 * 形成"恢复风暴"（实测 port_cycle=2 且超时 12 次）。
                 * 这里只重试+计数，让设备自己缓过来；也**不**计入"控制失败"时刻，
                 * 免得预热期的慢响应被数据冻结判据误判成挂死。
                 */
                s_vendor_urb_fail_streak = 0;
            } else {
                /* 每次失败都记下时刻：数据冻结判据靠它判断"设备不回"，
                 * 不必像下面这样凑满 3 批超时才动手。 */
                __atomic_store_n(&s_last_ctrl_fail_us, esp_timer_get_time(), __ATOMIC_RELEASE);
                if (++s_vendor_urb_fail_streak >= VENDOR_URB_RECOVER_STREAK) {
                    s_vendor_urb_fail_streak = 0;
#if USB_HOST_AUTOMATIC_DESTRUCTIVE_RECOVERY
                    /* 历史回退模式才由此安排断电；自动冻结恢复由独立检查任务处理。 */
                    s_root_port_cycle_requested = true;
#else
                    ESP_LOGW(TAG, "SET_REPORT 连续失败：保留 USB 供电，继续接受后续独立 URB 请求");
#endif
                }
            }
#else
            result = hid_class_request_set_report(
                slot->handle, request.report_type, request.report_id,
                mutable_data, mutable_length);
#endif
            /* 直连路径已在每请求独立 URB 上重试；不能再退回组件的共享 EP0 URB。 */
#if !VENDOR_USE_DIRECT_URB
            for (uint32_t attempt = 0; result != ESP_OK && attempt < VENDOR_CONTROL_RETRIES;
                 ++attempt) {
                ++s_vendor_control_retries;
                vTaskDelay(pdMS_TO_TICKS(VENDOR_CONTROL_RETRY_DELAY_MS));
                result = hid_class_request_set_report(
                    slot->handle, request.report_type, request.report_id,
                    mutable_data, mutable_length);
            }
#endif
#if !VENDOR_USE_DIRECT_URB
            /* 仅组件路径需要：单例 URB 被在飞对象锁死时必须重挂设备。
             * 直连路径每请求独立 URB，通道不会被锁死，不需要这套重手段。 */
            if (result != ESP_OK) {
                request_hid_device_recovery("控制传输连续失败");
            }
#endif
            ESP_LOGI(TAG,
                     "物理 SET_REPORT: interface=%u id=%02X type=%u length=%u "
                     "wire_length=%u result=%s",
                     request.interface_number, request.report_id,
                     request.report_type, request.length,
                     (unsigned)mutable_length, esp_err_to_name(result));
            if (mutable_length != 0U) {
                ESP_LOG_BUFFER_HEX_LEVEL(
                    TAG, mutable_data, mutable_length, ESP_LOG_INFO);
            }
            if (result == ESP_OK) {
                s_ctrl_fail_streak = 0;
            } else {
                ++s_vendor_control_failures;
            }
        }
    }
    finish_owned_task(TASK_EXIT_CONTROL);
}

static void hid_stats_task(void *argument)
{
    (void)argument;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HID_STATS_PERIOD_MS));
        if (stopping_requested()) {
            break;
        }
        if (!DUAL_PROXY_ENABLE_PERIODIC_STATS_LOG) {
            continue;
        }
        uint32_t urb_submitted = 0;
        uint32_t urb_completed = 0;
        uint32_t urb_timeouts = 0;
        uint32_t urb_aborted = 0;
        uint32_t urb_reopens = 0;
        uint32_t urb_retries = 0;
        int64_t urb_latency_max_us = 0;
        uint32_t urb_over_10ms = 0;
        uint32_t urb_over_100ms = 0;
        dual_vendor_urb_stats(&urb_submitted, &urb_completed, &urb_timeouts,
                              &urb_aborted, &urb_reopens, &urb_retries,
                              &urb_latency_max_us, &urb_over_10ms, &urb_over_100ms);
        ESP_LOGI(TAG, "Host HID统计：reports=%" PRIu32 " vendor_reports=%" PRIu32
                 " input_fail=%" PRIu32 " control=%" PRIu32
                 " control_fail=%" PRIu32 " ctrl_retry=%" PRIu32 " urb_sub=%" PRIu32 " urb_ok=%" PRIu32 " urb_to=%" PRIu32 " urb_retry=%" PRIu32 " recover=%" PRIu32 " port_cycle=%" PRIu32 " wheel=%" PRIu32 " ctrl_lat_max_us=%lld slow10=%" PRIu32 " slow100=%" PRIu32 " vmin_gap_us=%lld"
                 " errors=%" PRIu32 " motion_rx_dx=%lld motion_rx_dy=%lld"
                 " motion_rx_ok=%" PRIu32 " motion_rx_badlen=%" PRIu32
                 " motion_rx_badparse=%" PRIu32
                 " hb_ok=%" PRIu32 " hb_fail=%" PRIu32,
                 s_reports, s_vendor_reports, s_vendor_input_failures,
                 s_vendor_control_requests, s_vendor_control_failures, s_vendor_control_retries,
                 urb_submitted, urb_completed, urb_timeouts, urb_retries,
                 s_hid_recoveries, s_root_port_cycles,
                 s_wheel_reports,
                 urb_latency_max_us, urb_over_10ms, urb_over_100ms,
                 (long long)s_vendor_arrival_min_gap_us, s_errors,
                 (long long)__atomic_load_n(&s_motion_rx_dx, __ATOMIC_RELAXED),
                 (long long)__atomic_load_n(&s_motion_rx_dy, __ATOMIC_RELAXED),
                 (uint32_t)__atomic_load_n(&s_motion_rx_ok, __ATOMIC_RELAXED),
                 (uint32_t)__atomic_load_n(&s_motion_rx_bad_length, __ATOMIC_RELAXED),
                 (uint32_t)__atomic_load_n(&s_motion_rx_bad_parse, __ATOMIC_RELAXED),
                 (uint32_t)__atomic_load_n(&s_heartbeat_ok_count, __ATOMIC_RELAXED),
                 (uint32_t)__atomic_load_n(&s_heartbeat_fail_count, __ATOMIC_RELAXED));
        ESP_LOGI(TAG, "USB诊断：cycle_req=%" PRIu32 " cycle_attempt=%" PRIu32
                 " cycle_ok=%" PRIu32 " cycle_fail=%" PRIu32 " off_fail=%" PRIu32
                 " on_fail=%" PRIu32 " suppressed=%" PRIu32 " input_restored=%" PRIu32
                 " input_missing=%" PRIu32 " late500=%" PRIu32 " detect_max_us=%lld",
                 s_cycle_requests, s_root_port_cycles, s_cycle_ok, s_cycle_failed,
                 s_cycle_off_fail, s_cycle_on_fail, s_cycle_suppressed,
                 s_cycle_input_restored, s_cycle_input_missing, s_cycle_slow,
                 (long long)__atomic_load_n(&s_cycle_delay_max_us, __ATOMIC_ACQUIRE));
        log_queue_metrics("host_hid_event", &s_hid_event_queue_metrics);
        log_queue_metrics("host_hid_report", &s_report_queue_metrics);
        log_queue_metrics("host_hid_control", &s_control_queue_metrics);
    }
    finish_owned_task(TASK_EXIT_STATS);
}

void dual_hid_host_collect_stats(dual_stats_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
#define ADD_HOST_STAT(id, field) \
    dual_stats_snapshot_add_counter(snapshot, (uint8_t)(id), \
        (uint64_t)__atomic_load_n(&(field), __ATOMIC_RELAXED))
    ADD_HOST_STAT(DUAL_STAT_REPORTS, s_reports);
    ADD_HOST_STAT(DUAL_STAT_VENDOR_REPORTS, s_vendor_reports);
    ADD_HOST_STAT(DUAL_STAT_INPUT_FAIL, s_vendor_input_failures);
    ADD_HOST_STAT(DUAL_STAT_CONTROL, s_vendor_control_requests);
    ADD_HOST_STAT(DUAL_STAT_CONTROL_FAIL, s_vendor_control_failures);
    ADD_HOST_STAT(DUAL_STAT_CTRL_RETRY, s_vendor_control_retries);
    ADD_HOST_STAT(DUAL_STAT_RECOVER, s_hid_recoveries);
    ADD_HOST_STAT(DUAL_STAT_PORT_CYCLE, s_root_port_cycles);
    ADD_HOST_STAT(DUAL_STAT_WHEEL, s_wheel_reports);
    ADD_HOST_STAT(DUAL_STAT_ERRORS, s_errors);
    dual_stats_snapshot_add_signed_counter(snapshot, DUAL_STAT_MOTION_RX_DX,
        __atomic_load_n(&s_motion_rx_dx, __ATOMIC_RELAXED));
    dual_stats_snapshot_add_signed_counter(snapshot, DUAL_STAT_MOTION_RX_DY,
        __atomic_load_n(&s_motion_rx_dy, __ATOMIC_RELAXED));
    ADD_HOST_STAT(DUAL_STAT_MOTION_RX_OK, s_motion_rx_ok);
    ADD_HOST_STAT(DUAL_STAT_MOTION_RX_BADLEN, s_motion_rx_bad_length);
    ADD_HOST_STAT(DUAL_STAT_MOTION_RX_BADPARSE, s_motion_rx_bad_parse);
    ADD_HOST_STAT(DUAL_STAT_HB_OK, s_heartbeat_ok_count);
    ADD_HOST_STAT(DUAL_STAT_HB_FAIL, s_heartbeat_fail_count);
    ADD_HOST_STAT(DUAL_STAT_CYCLE_REQ, s_cycle_requests);
    ADD_HOST_STAT(DUAL_STAT_CYCLE_ATTEMPT, s_root_port_cycles);
    ADD_HOST_STAT(DUAL_STAT_CYCLE_OK, s_cycle_ok);
    ADD_HOST_STAT(DUAL_STAT_CYCLE_FAIL, s_cycle_failed);
    ADD_HOST_STAT(DUAL_STAT_CYCLE_OFF_FAIL, s_cycle_off_fail);
    ADD_HOST_STAT(DUAL_STAT_CYCLE_ON_FAIL, s_cycle_on_fail);
    ADD_HOST_STAT(DUAL_STAT_CYCLE_SUPPRESSED, s_cycle_suppressed);
    ADD_HOST_STAT(DUAL_STAT_INPUT_RESTORED, s_cycle_input_restored);
    ADD_HOST_STAT(DUAL_STAT_INPUT_MISSING, s_cycle_input_missing);
    ADD_HOST_STAT(DUAL_STAT_LATE500, s_cycle_slow);
#undef ADD_HOST_STAT
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_VMIN_GAP_US,
        (uint64_t)__atomic_load_n(&s_vendor_arrival_min_gap_us, __ATOMIC_RELAXED));
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_DETECT_MAX_US,
        (uint64_t)__atomic_load_n(&s_cycle_delay_max_us, __ATOMIC_ACQUIRE));
    uint32_t urb_submitted = 0U;
    uint32_t urb_completed = 0U;
    uint32_t urb_timeouts = 0U;
    uint32_t urb_aborted = 0U;
    uint32_t urb_reopens = 0U;
    uint32_t urb_retries = 0U;
    int64_t urb_latency_max_us = 0;
    uint32_t urb_over_10ms = 0U;
    uint32_t urb_over_100ms = 0U;
    dual_vendor_urb_stats(&urb_submitted, &urb_completed, &urb_timeouts,
                          &urb_aborted, &urb_reopens, &urb_retries,
                          &urb_latency_max_us, &urb_over_10ms, &urb_over_100ms);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_URB_SUB, urb_submitted);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_URB_OK, urb_completed);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_URB_TO, urb_timeouts);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_URB_RETRY, urb_retries);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_SLOW10, urb_over_10ms);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_SLOW100, urb_over_100ms);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_CTRL_LAT_MAX_US,
                                     (uint64_t)urb_latency_max_us);
    uint64_t vendor_session_state = 0U;
    uint32_t vendor_epoch = 0U;
    uint32_t vendor_peer_generation = 0U;
    int64_t first_vendor_request_us = 0;
    if (s_vendor_session_state_mutex != NULL) {
        xSemaphoreTake(s_vendor_session_state_mutex, portMAX_DELAY);
        vendor_epoch = __atomic_load_n(&s_vendor_session_epoch, __ATOMIC_RELAXED);
        vendor_peer_generation = __atomic_load_n(
            &s_vendor_session_peer_generation, __ATOMIC_RELAXED);
        first_vendor_request_us = __atomic_load_n(
            &s_vendor_session_first_request_us, __ATOMIC_RELAXED);
        xSemaphoreGive(s_vendor_session_state_mutex);
    }
    if (vendor_epoch != 0U && vendor_peer_generation != 0U) {
        vendor_session_state |= DUAL_M_VENDOR_SESSION_ACTIVE;
    }
    if (first_vendor_request_us != 0) {
        vendor_session_state |= DUAL_M_VENDOR_SESSION_FIRST_REQUEST;
    }
    if (vendor_peer_generation != 0U &&
        vendor_peer_generation == dual_uart1_peer_generation()) {
        vendor_session_state |= DUAL_M_VENDOR_SESSION_PEER_CURRENT;
    }
    if (dual_uart1_profile_waiting_host()) {
        vendor_session_state |= DUAL_M_VENDOR_SESSION_WAITING_HOST;
    }
    vendor_session_state |= (uint64_t)vendor_epoch <<
        DUAL_M_VENDOR_SESSION_EPOCH_SHIFT;
    dual_stats_snapshot_add_counter(snapshot,
        DUAL_STAT_M_VENDOR_SESSION_STATE, vendor_session_state);
    const QueueHandle_t queues[] = { s_hid_event_queue, s_report_queue, s_control_queue };
    const queue_metrics_t *metrics[] = {
        &s_hid_event_queue_metrics, &s_report_queue_metrics, &s_control_queue_metrics
    };
    const uint16_t capacities[] = {
        HID_EVENT_QUEUE_LENGTH, HID_REPORT_QUEUE_LENGTH, HID_CONTROL_QUEUE_LENGTH
    };
    const uint8_t ids[] = {
        DUAL_STATS_QUEUE_HOST_HID_EVENT, DUAL_STATS_QUEUE_HOST_HID_REPORT,
        DUAL_STATS_QUEUE_HOST_HID_CONTROL
    };
    for (size_t index = 0; index < sizeof(queues) / sizeof(queues[0]); ++index) {
        const uint16_t depth = queues[index] == NULL ? DUAL_STATS_UNKNOWN_U16 :
            (uint16_t)uxQueueMessagesWaiting(queues[index]);
        const uint16_t peak = (uint16_t)__atomic_load_n(&metrics[index]->peak,
                                                        __ATOMIC_RELAXED);
        dual_stats_snapshot_add_queue(snapshot, ids[index], capacities[index], depth, peak,
            __atomic_load_n(&metrics[index]->received, __ATOMIC_RELAXED),
            __atomic_load_n(&metrics[index]->rejected, __ATOMIC_RELAXED),
            __atomic_load_n(&metrics[index]->dropped, __ATOMIC_RELAXED));
    }
}

static void hid_interface_callback(
    hid_host_device_handle_t handle,
    hid_host_interface_event_t event,
    void *argument)
{
    (void)argument;
    /*
     * 输入报告按 1 kHz 到达，本分支刻意**不调用任何组件查询 API**：直接用本地槽位表
     * 按 handle 定位（槽位在 CONNECTED 时登记了 handle 与 interface_number）。
     * 依据：ESP-IDF issue #14996「USB Host sometimes freezes randomly (IDFGH-14198)」
     * 里官方测试者验证过的缓解方向，就是"让 RX 回调尽可能快地返回"；本分支其余部分
     * 本来就只是取数据 + 入队，这里再去掉一次 hid_host_device_get_params()。
     */
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        queue_metric_increment(&s_report_queue_metrics.received);
        hid_interface_slot_t *slot = find_interface_slot_by_handle(handle);
        if (slot == NULL || !slot->active) {
            ++s_errors;
            queue_metric_increment(&s_report_queue_metrics.rejected);
            return;
        }
        if (stopping_requested()) {
            queue_metric_increment(&s_report_queue_metrics.rejected);
            return;
        }
        uint8_t data[HID_RAW_REPORT_MAX];
        size_t length = 0;
        const bool injected = __atomic_load_n(&s_injected_report_pending, __ATOMIC_ACQUIRE);
        if (injected) {
            /* 诊断注入：用注入字节顶替真实报告；后续统计/入队/转发与物理报告完全同路。 */
            __atomic_store_n(&s_injected_report_pending, false, __ATOMIC_RELEASE);
            length = s_injected_report_length;
            if (length > sizeof(data)) {
                length = sizeof(data);
            }
            memcpy(data, s_injected_report, length);
        } else if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &length) != ESP_OK ||
                   length > sizeof(data) || length > UINT8_MAX) {
            ++s_errors;
            queue_metric_increment(&s_report_queue_metrics.rejected);
            return;
        }
        const size_t id_bytes = slot->has_report_id ? 1U : 0U;
        if (!injected && slot->mouse_interface &&
            length == (size_t)slot->mouse_layout.report_bytes + id_bytes &&
            (!slot->has_report_id || data[0] == slot->mouse_report_id)) {
            __atomic_store_n(&s_last_report_us, esp_timer_get_time(), __ATOMIC_RELEASE);
        }
        raw_report_event_t queued = {
            .interface_number = slot->interface_number,
            .length = (uint8_t)length,
            .mouse_report = slot->mouse_interface,
            .injected = injected,
            .has_report_id = slot->has_report_id,
            .mouse_report_id = slot->mouse_report_id,
            .mouse_layout = slot->mouse_layout,
        };
        memcpy(queued.data, data, length);
        if (s_report_queue == NULL) {
            queue_metric_increment(&s_report_queue_metrics.rejected);
            ++s_errors;
            notify_release_callback(false);
        } else if (xQueueSend(s_report_queue, &queued, 0) != pdTRUE) {
            queue_metric_increment(&s_report_queue_metrics.dropped);
            ++s_errors;
            notify_release_callback(false);
        } else {
            queue_metric_observe_depth(&s_report_queue_metrics, s_report_queue);
        }
        return;
    }
    /* 其余事件（接口断开 / 传输错误）都是低频路径，保留组件查询取 interface_number。 */
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        ++s_errors;
        return;
    }
    if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        bool mouse_interface = false;
        bool device_gone = false;
        portENTER_CRITICAL(&s_interface_state_mux);
        hid_interface_slot_t *slot = find_interface_slot(params.iface_num);
        mouse_interface = slot != NULL && slot->started && slot->mouse_interface;
        clear_interface_slot(params.iface_num);
        refresh_mouse_present_locked();
        if (__atomic_load_n(&s_device_present, __ATOMIC_ACQUIRE) &&
            !any_interface_active_locked()) {
            __atomic_store_n(&s_device_present, false, __ATOMIC_RELEASE);
            __atomic_store_n(&s_mouse_present, false, __ATOMIC_RELEASE);
            device_gone = true;
        }
        portEXIT_CRITICAL(&s_interface_state_mux);

        if (mouse_interface) {
            ESP_LOGW(TAG, "鼠标 HID 接口断开：interface=%u", params.iface_num);
            dual_status_led_set_host_mouse_ready(false);
        } else {
            ESP_LOGI(TAG, "vendor HID接口断开：interface=%u", params.iface_num);
            queue_reset_count_dropped(s_control_queue, &s_control_queue_metrics);
        }
        if (device_gone) {
            /*
             * 防抖：H 在 Host 口上注册/注销时的抖动（现场实测：P 侧每个十几秒就
             * 发生一次拆卸，日志里写明"M 报告物理鼠标拔出"）会被立刻升级成
             * "拆克隆 + 重装"，代价是 1~2 秒输入中断。这里先挂起，等
             * MOUSE_GONE_DEBOUNCE_US 后仍不在设备列表里，才真的上报 DEVICE_GONE；
             * 期间设备若回来则取消挂起——真拔出依旧走完整的卸载+重装流程。
             */
            if (s_mouse_gone_pending_us == 0) {
                s_mouse_gone_pending_us = esp_timer_get_time();
            }
            ESP_LOGW(TAG, "物理USB HID设备已拔出（进入 %u ms 防抖，暂不上报 DEVICE_GONE）",
                     (unsigned)(MOUSE_GONE_DEBOUNCE_US / 1000));
        } else {
            profile_reset_collector();
        }
        (void)hid_host_device_close(handle);
        return;
    }
    if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        ++s_errors;
        ESP_LOGW(TAG, "标准鼠标接口传输错误：interface=%u", params.iface_num);
        notify_release_callback(false);
        queue_metric_increment(&s_hid_event_queue_metrics.received);
        const hid_event_t retry = {.handle = handle, .type = HID_EVENT_TRANSFER_ERROR};
        if (s_hid_event_queue == NULL ||
            xQueueSend(s_hid_event_queue, &retry, 0) != pdTRUE) {
            queue_metric_increment(&s_hid_event_queue_metrics.dropped);
        } else {
            queue_metric_observe_depth(&s_hid_event_queue_metrics, s_hid_event_queue);
        }
    }
}

static void hid_driver_callback(
    hid_host_device_handle_t handle,
    hid_host_driver_event_t event,
    void *argument)
{
    (void)argument;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }
    queue_metric_increment(&s_hid_event_queue_metrics.received);
    if (s_hid_event_queue == NULL || stopping_requested()) {
        queue_metric_increment(&s_hid_event_queue_metrics.rejected);
        return;
    }
    const hid_event_t queued = {.handle = handle, .type = HID_EVENT_CONNECTED};
    if (xQueueSend(s_hid_event_queue, &queued, 0) != pdTRUE) {
        queue_metric_increment(&s_hid_event_queue_metrics.dropped);
        ++s_errors;
        notify_release_callback(false);
    } else {
        queue_metric_observe_depth(&s_hid_event_queue_metrics, s_hid_event_queue);
    }
}

/*
 * 防抖到期检查：由 hid_event_task 以 100 ms 周期调用。设备仍然不在（且没有任何
 * 活动接口）时，才真的上报“物理鼠标消失”，让 P 走完整的卸载 + 重装流程。
 */
static void maybe_report_mouse_gone(void)
{
    const int64_t pending = s_mouse_gone_pending_us;
    if (pending == 0) {
        return;
    }
    if (esp_timer_get_time() - pending < MOUSE_GONE_DEBOUNCE_US) {
        return;
    }
    s_mouse_gone_pending_us = 0;
    if (__atomic_load_n(&s_device_present, __ATOMIC_ACQUIRE)) {
        /* 防抖期间设备已回来：当作一次瞬断，不通知对端。 */
        ESP_LOGW(TAG, "防抖期间物理鼠标已恢复：本次瞬断不上报 DEVICE_GONE");
        return;
    }
    ESP_LOGW(TAG, "确认真拔出：防抖 %u ms 后仍无设备，上报 DEVICE_GONE",
             (unsigned)(MOUSE_GONE_DEBOUNCE_US / 1000));
    profile_reset_collector();
    notify_release_callback(true);
}

/* 历史重挂路径由显式开关保留；默认只报告控制异常。 */
static void maybe_run_hid_recovery(void)
{
    if (!s_hid_recover_requested) {
        return;
    }
    s_hid_recover_requested = false;
    const char *reason = s_hid_recover_reason;
#if !USB_HOST_AUTOMATIC_DESTRUCTIVE_RECOVERY
    ESP_LOGW(TAG, "控制通道异常，保持物理输入与克隆会话（原因：%s）",
             reason != NULL ? reason : "");
    s_ctrl_fail_streak = 0;
    return;
#else
    hid_host_device_handle_t handles[HID_INTERFACE_SLOT_COUNT];
    size_t count = 0;
    portENTER_CRITICAL(&s_interface_state_mux);
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        hid_interface_slot_t *slot = &s_interface_slots[index];
        if (slot->active && slot->handle != NULL) {
            handles[count++] = slot->handle;
        }
    }
    portEXIT_CRITICAL(&s_interface_state_mux);
    ++s_hid_recoveries;
    ESP_LOGW(TAG, "自动恢复第 %u 次：重挂 %u 个 HID 接口（原因：%s）",
             (unsigned)s_hid_recoveries, (unsigned)count,
             reason != NULL ? reason : "");
    for (size_t index = 0; index < count; ++index) {
        const esp_err_t stop_result = hid_host_device_stop(handles[index]);
        vTaskDelay(pdMS_TO_TICKS(20));
        const esp_err_t start_result = hid_host_device_start(handles[index]);
        ESP_LOGW(TAG, "  interface 重挂：stop=%s start=%s",
                 esp_err_to_name(stop_result), esp_err_to_name(start_result));
        if (start_result != ESP_OK) {
            /* 接口没能真正重开（在飞传输仍占着）→ 升级为端口级功率循环。 */
            s_root_port_cycle_requested = true;
        }
    }
    s_ctrl_fail_streak = 0;
#endif
}

/* 每约 100 ms 主动发 GET_DESCRIPTOR(Device)；其失败只代表 EP0 未完成。 */
static void maybe_run_usb_heartbeat(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t last = __atomic_load_n(&s_last_heartbeat_us, __ATOMIC_ACQUIRE);
    if (last != 0 && now - last < USB_HEARTBEAT_PERIOD_US) {
        return;
    }
    __atomic_store_n(&s_last_heartbeat_us, now, __ATOMIC_RELEASE);

    /* 锁内只取 handle；参数查询可能阻塞，必须放到锁外。 */
    hid_host_device_handle_t handle = NULL;
    portENTER_CRITICAL(&s_interface_state_mux);
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        const hid_interface_slot_t *slot = &s_interface_slots[index];
        if (slot->active && slot->handle != NULL && slot->mouse_interface) {
            handle = slot->handle;
            break;
        }
    }
    portEXIT_CRITICAL(&s_interface_state_mux);
    if (handle == NULL) {
        return;   /* 没有活动鼠标接口，探测没有意义 */
    }

    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        return;
    }

    uint8_t descriptor[18];
    size_t out_length = 0;
    const esp_err_t result = dual_vendor_urb_heartbeat_control(
        params.addr, 0x80U, 0x06U, (uint16_t)((1U << 8) | 0U), 0U,
        (uint16_t)sizeof(descriptor), USB_HEARTBEAT_TIMEOUT_MS,
        descriptor, sizeof(descriptor), &out_length);
    const int64_t done = esp_timer_get_time();
    if (result == ESP_OK) {
        __atomic_store_n(&s_last_ctrl_ok_us, done, __ATOMIC_RELEASE);
        __atomic_store_n(&s_probe_fail_streak, 0U, __ATOMIC_RELEASE);
        __atomic_add_fetch(&s_heartbeat_ok_count, 1U, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&s_last_ctrl_fail_us, done, __ATOMIC_RELEASE);
        __atomic_add_fetch(&s_probe_fail_streak, 1U, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_heartbeat_fail_count, 1U, __ATOMIC_RELAXED);
    }
}

/* 独立检查任务：冻结判定只看标准心跳 URB，不等待普通控制请求超时。 */
static void run_stall_port_cycle(int64_t oldest_heartbeat)
{
    ++s_root_port_cycles;
    notify_release_callback(false);
    ESP_LOGW(TAG, "USB诊断：根端口循环开始 attempt=%" PRIu32, s_root_port_cycles);
    /* 包含释放按钮和日志耗时，测量到实际调用关端口前；不是硬件故障时间。 */
    const int64_t observed_delay_us = usb_stall_observed_delay(
        esp_timer_get_time(), oldest_heartbeat);
    if (observed_delay_us > s_cycle_delay_max_us) {
        __atomic_store_n(&s_cycle_delay_max_us, observed_delay_us, __ATOMIC_RELEASE);
    }
    if (observed_delay_us > USB_STALL_TARGET_US) { ++s_cycle_slow; }
    const esp_err_t off = usb_host_lib_set_root_port_power(false);
    if (off != ESP_OK) {
        ++s_cycle_off_fail;
        ++s_cycle_failed;
        ESP_LOGE(TAG, "USB诊断：关闭根端口失败 %s", esp_err_to_name(off));
        return;
    }
    __atomic_store_n(&s_root_port_powered, false, __ATOMIC_RELEASE);
    esp_err_t on = ESP_FAIL;
    /* 设备句柄失效后仍执行有界开端口补救，不依赖mouse_present。 */
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        on = usb_host_lib_set_root_port_power(true);
        if (on == ESP_OK) { break; }
        ++s_cycle_on_fail;
        ESP_LOGW(TAG, "USB诊断：开启根端口失败 try=%u result=%s",
                 attempt + 1U, esp_err_to_name(on));
        if (attempt < 2U) { vTaskDelay(pdMS_TO_TICKS(100)); }
    }
    if (on == ESP_OK) {
        __atomic_store_n(&s_root_port_powered, true, __ATOMIC_RELEASE);
        ++s_cycle_ok;
        s_cycle_input_wait_us = esp_timer_get_time();
        ESP_LOGW(TAG, "USB诊断：端口开关完成，等待重新枚举及真实输入");
    } else {
        ++s_cycle_failed;
        ESP_LOGE(TAG, "USB诊断：开启根端口补救耗尽，不执行整板复位");
    }
}

static void stall_watch_task(void *argument)
{
    (void)argument;
    bool suppressed_latched = false;
    while (!stopping_requested()) {
        const int64_t now = esp_timer_get_time();
        const int64_t input = __atomic_load_n(&s_last_report_us, __ATOMIC_ACQUIRE);
        if (s_cycle_input_wait_us != 0) {
            if (input > s_cycle_input_wait_us) {
                ++s_cycle_input_restored;
                ESP_LOGW(TAG, "USB诊断：根端口循环后真实输入恢复 elapsed_us=%lld",
                         (long long)(input - s_cycle_input_wait_us));
                s_cycle_input_wait_us = 0;
            } else if (now - s_cycle_input_wait_us >= 15000000LL) {
                ++s_cycle_input_missing;
                ESP_LOGW(TAG, "USB诊断：循环后15秒未观察到真实输入（静止也可能无报告）");
                s_cycle_input_wait_us = 0;
            }
        }
        int64_t oldest_heartbeat = 0;
        (void)dual_vendor_urb_heartbeat_pending_snapshot(&oldest_heartbeat, NULL);
        const int64_t ready = __atomic_load_n(&s_mouse_ready_us, __ATOMIC_ACQUIRE);
        const bool evidence = usb_stall_evidence_ready(
            now, ready, oldest_heartbeat,
            __atomic_load_n(&s_mouse_present, __ATOMIC_ACQUIRE), stopping_requested());
        if (evidence) {
            if (!usb_stall_cooldown_ready(now, s_stall_recovery_us) ||
                !try_lifecycle_lock()) {
                if (!suppressed_latched) { ++s_cycle_suppressed; }
                suppressed_latched = true;
            } else {
                /* 获取生命周期锁后重新采样，避免依据已经恢复的旧快照切端口。 */
                (void)dual_vendor_urb_heartbeat_pending_snapshot(
                    &oldest_heartbeat, NULL);
                const int64_t check_now = esp_timer_get_time();
                if (usb_stall_evidence_ready(check_now,
                        __atomic_load_n(&s_mouse_ready_us, __ATOMIC_ACQUIRE),
                        oldest_heartbeat,
                        __atomic_load_n(&s_mouse_present, __ATOMIC_ACQUIRE), stopping_requested())) {
                    ++s_cycle_requests;
                    s_stall_recovery_us = check_now;
                    run_stall_port_cycle(oldest_heartbeat);
                }
                release_lifecycle_lock();
                suppressed_latched = false;
            }
        } else {
            suppressed_latched = false;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(USB_STALL_POLL_MS));
    }
    finish_owned_task(TASK_EXIT_STALL_WATCH);
}

static void hid_event_task(void *argument)
{
    (void)argument;
    hid_event_t event;
    while (true) {
        /*
         * 主动心跳探测：空闲时约每 100 ms 一次；独立检查任务观察在途请求。
         * 放在循环顶部而不是超时分支里，否则报告事件密集时它会被饿住，
         * 而这恰恰是"设备刚开始冻结、还在冒报告"的那一小段关键时间。
         */
        maybe_run_usb_heartbeat();
        if (xQueueReceive(s_hid_event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
            maybe_report_mouse_gone();
            /* 独立检查任务负责冻结判定；历史接口重挂路径仍默认关闭。 */
            maybe_run_hid_recovery();
            continue;
        }
        if (event.type == HID_EVENT_STOP) {
            break;
        }
        if (event.type == HID_EVENT_TRANSFER_ERROR) {
            /* 只对组件已经归还的 IN transfer 重提交；不重挂接口或切断供电。 */
            for (uint32_t attempt = 0; attempt < 3U; ++attempt) {
                const esp_err_t rearm = hid_host_device_rearm_input(event.handle);
                if (rearm == ESP_OK) {
                    break;
                }
                ESP_LOGW(TAG, "输入端点重提交第 %u 次失败：%s",
                         (unsigned)(attempt + 1U), esp_err_to_name(rearm));
                if (attempt < 2U) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
            continue;
        }
        if (event.type == HID_EVENT_CONNECTED) {
            /* 设备回来了：取消待定的“消失”，避免一次瞬断引发拆卸+重装。 */
            s_mouse_gone_pending_us = 0;
        }
        if (stopping_requested() || event.type != HID_EVENT_CONNECTED) {
            continue;
        }
        hid_host_dev_params_t params;
        if (hid_host_device_get_params(event.handle, &params) != ESP_OK) {
            ++s_errors;
            continue;
        }
        hid_host_dev_info_t info = {0};
        const bool info_valid = hid_host_get_device_info(event.handle, &info) == ESP_OK;
        if (info_valid) {
            log_device_identity_once(params.addr, &info);
            ESP_LOGI(TAG, "HID接口发现：addr=%u interface=%u subclass=%u protocol=%u VID:PID=%04X:%04X",
                     params.addr, params.iface_num, params.sub_class, params.proto, info.VID, info.PID);
        }
        const hid_host_device_config_t config = {
            .callback = hid_interface_callback,
            .callback_arg = NULL,
        };
        esp_err_t result = hid_host_device_open(event.handle, &config);
        if (result != ESP_OK) {
            ++s_errors;
            ESP_LOGE(TAG, "HID接口打开失败：interface=%u error=%s",
                     params.iface_num, esp_err_to_name(result));
            continue;
        }
        if (stopping_requested()) {
            (void)hid_host_device_close(event.handle);
            continue;
        }
        log_report_descriptor(event.handle, params.iface_num);
        size_t descriptor_length = 0;
        uint8_t *descriptor = hid_host_get_report_descriptor(
            event.handle, &descriptor_length);
        profile_collect_interface(
            &params, info_valid ? &info : NULL, descriptor, descriptor_length);
        hid_mouse_report_layout_t mouse_layout;
        memset(&mouse_layout, 0, sizeof(mouse_layout));
        const bool mouse_interface = hid_report_find_mouse_layout(
            descriptor, descriptor_length, &mouse_layout);
        const uint8_t mouse_report_id = mouse_interface
            ? mouse_layout.report_id : 0U;
        const bool has_report_id = report_descriptor_has_report_id(
            descriptor, descriptor_length);
        if (mouse_interface &&
            mouse_layout.wheel.bit_offset != HID_MOUSE_FIELD_INVALID_OFFSET) {
            s_wheel_byte_offset = (uint8_t)(mouse_layout.wheel.bit_offset / 8U);
        }

        portENTER_CRITICAL(&s_interface_state_mux);
        hid_interface_slot_t *slot = allocate_interface_slot(params.iface_num);
        if (slot != NULL) {
            slot->handle = event.handle;
            slot->mouse_interface = mouse_interface;
            slot->mouse_report_id = mouse_report_id;
            slot->has_report_id = has_report_id;
            if (mouse_interface) {
                /* 保存布局：RX 回调要靠它把报告解析成位移（统计"收到的位移"）。 */
                slot->mouse_layout = mouse_layout;
            }
            if (mouse_interface) {
                /* 枚举时刻打点：紧随其后的若干秒是设备应答最慢的预热期。 */
                __atomic_store_n(&s_mouse_ready_us, esp_timer_get_time(), __ATOMIC_RELEASE);
            }
        }
        portEXIT_CRITICAL(&s_interface_state_mux);
        if (slot == NULL) {
            ++s_errors;
            ESP_LOGW(TAG, "HID接口槽位耗尽，拒绝接口=%u", params.iface_num);
            (void)hid_host_device_close(event.handle);
            continue;
        }
        if (!mouse_interface) {
            ESP_LOGI(TAG,
                      "已打开并启动vendor HID接口：interface=%u subclass=%u protocol=%u report_id=%s",
                      params.iface_num, params.sub_class, params.proto,
                      has_report_id ? "yes" : "no");
        }
        /* 保持设备上电后的 Report Protocol，与 Windows 直连枚举一致。
         * 强制 Boot Protocol 可能改变厂商接口的 HID++ 工作状态。 */
        if (result == ESP_OK && !stopping_requested()) {
            const esp_err_t idle_result = hid_class_request_set_idle(
                event.handle, 0, 0);
            ESP_LOGI(TAG, "HID SET_IDLE: interface=%u result=%s",
                     params.iface_num, esp_err_to_name(idle_result));
        }
        if (result == ESP_OK && !stopping_requested()) {
            result = hid_host_device_start(event.handle);
        } else if (result == ESP_OK) {
            result = ESP_ERR_INVALID_STATE;
        }
        if (result != ESP_OK) {
            ++s_errors;
            ESP_LOGE(TAG, "HID接口启动失败：interface=%u error=%s",
                     params.iface_num, esp_err_to_name(result));
            portENTER_CRITICAL(&s_interface_state_mux);
            hid_interface_slot_t *current_slot = find_interface_slot(params.iface_num);
            if (current_slot == slot && current_slot->handle == event.handle) {
                clear_interface_slot(params.iface_num);
            }
            refresh_mouse_present_locked();
            if (!any_interface_active_locked()) {
                __atomic_store_n(&s_device_present, false, __ATOMIC_RELEASE);
            }
            portEXIT_CRITICAL(&s_interface_state_mux);
            (void)hid_host_device_close(event.handle);
            notify_release_callback(false);
        } else {
            bool slot_started = false;
            bool started_mouse_interface = false;
            portENTER_CRITICAL(&s_interface_state_mux);
            hid_interface_slot_t *current_slot = find_interface_slot(params.iface_num);
            if (current_slot == slot && current_slot->handle == event.handle) {
                current_slot->started = true;
                __atomic_store_n(&s_device_present, true, __ATOMIC_RELEASE);
                refresh_mouse_present_locked();
                started_mouse_interface = current_slot->mouse_interface;
                slot_started = true;
            }
            portEXIT_CRITICAL(&s_interface_state_mux);
            if (!slot_started) {
                /* Disconnect may have cleared this slot while start() was in flight. */
                (void)hid_host_device_close(event.handle);
                continue;
            }
            if (stopping_requested()) {
                (void)hid_host_device_close(event.handle);
                continue;
            }
            if (started_mouse_interface) {
                dual_status_led_set_host_mouse_ready(true);
                ESP_LOGI(TAG,
                         "动态鼠标输入已启动：interface=%u report_id=%u bytes=%u",
                         params.iface_num, mouse_layout.report_id,
                         mouse_layout.report_bytes);
            }
        }
    }
    finish_owned_task(TASK_EXIT_HID_EVENT);
}

static void usb_host_library_task(void *argument)
{
    TaskHandle_t owner = (TaskHandle_t)argument;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t result = usb_host_install(&config);
    if (result == ESP_OK) {
        __atomic_store_n(&s_usb_host_installed, true, __ATOMIC_RELEASE);
        __atomic_store_n(&s_root_port_powered, true, __ATOMIC_RELEASE);
    }
    if (s_task_events != NULL) {
        (void)xEventGroupSetBits(s_task_events, TASK_EVENT_HOST_INSTALL_DONE);
    }
    xTaskNotify(owner, (uint32_t)result, eSetValueWithOverwrite);
    if (result != ESP_OK) {
        finish_owned_task(TASK_EXIT_HOST_LIBRARY);
        return;
    }
    while (true) {
        uint32_t flags = 0;
        result = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0U &&
            s_task_events != NULL) {
            (void)xEventGroupSetBits(s_task_events, TASK_EVENT_ALL_FREE);
        }
        if (__atomic_load_n(&s_host_lib_stop_requested, __ATOMIC_ACQUIRE) &&
            s_task_events != NULL &&
            (xEventGroupGetBits(s_task_events) & TASK_EVENT_ALL_FREE) != 0U) {
            break;
        }
        if (result != ESP_OK) {
            ++s_errors;
            ESP_LOGE(TAG, "USB Host事件处理失败：%s", esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(HID_HOST_STOP_POLL_MS));
        }
    }
    finish_owned_task(TASK_EXIT_HOST_LIBRARY);
    return;
}

static void descriptor_client_event_callback(
    const usb_host_client_event_msg_t *event,
    void *argument)
{
    (void)event;
    (void)argument;
}

static void descriptor_client_task(void *argument)
{
    (void)argument;
    while (true) {
        const esp_err_t result = usb_host_client_handle_events(
            s_descriptor_client, pdMS_TO_TICKS(100));
        if (stopping_requested()) {
            break;
        }
        if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
            ++s_errors;
            ESP_LOGW(TAG, "描述符客户端事件处理失败：%s", esp_err_to_name(result));
        }
    }
    finish_owned_task(TASK_EXIT_DESCRIPTOR);
}

static esp_err_t wait_for_no_connected_devices(uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)timeout_ms * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        usb_host_lib_info_t info;
        const esp_err_t result = usb_host_lib_info(&info);
        if (result != ESP_OK) {
            return result;
        }
        if (info.num_devices == 0 &&
            !__atomic_load_n(&s_device_present, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&s_mouse_present, __ATOMIC_ACQUIRE)) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(HID_HOST_STOP_POLL_MS));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_for_control_api_quiescence(uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)timeout_ms * 1000LL;
    while (__atomic_load_n(&s_control_api_users, __ATOMIC_ACQUIRE) != 0U) {
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(HID_HOST_STOP_POLL_MS));
    }
    return ESP_OK;
}

static esp_err_t wait_for_host_install_result(uint32_t timeout_ms)
{
    if (!s_host_task_created || s_task_events == NULL) {
        return ESP_OK;
    }
    if ((xEventGroupGetBits(s_task_events) & TASK_EVENT_HOST_INSTALL_DONE) != 0U) {
        return ESP_OK;
    }
    const EventBits_t installed = xEventGroupWaitBits(
        s_task_events, TASK_EVENT_HOST_INSTALL_DONE, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    return (installed & TASK_EVENT_HOST_INSTALL_DONE) != 0U
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t wait_for_hid_host_uninstall(uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)timeout_ms * 1000LL;
    while (true) {
        const esp_err_t result = hid_host_uninstall();
        if (result == ESP_OK) {
            s_hid_host_installed = false;
            return ESP_OK;
        }
        if (result != ESP_ERR_INVALID_STATE || esp_timer_get_time() >= deadline_us) {
            return result == ESP_ERR_INVALID_STATE ? ESP_ERR_TIMEOUT : result;
        }
        vTaskDelay(pdMS_TO_TICKS(HID_HOST_STOP_POLL_MS));
    }
}

static esp_err_t resume_host_after_early_stop_failure(esp_err_t cause)
{
    if (!__atomic_load_n(&s_usb_host_installed, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_stopping, false, __ATOMIC_RELEASE);
        return cause;
    }
    if (!__atomic_load_n(&s_root_port_powered, __ATOMIC_ACQUIRE)) {
        const esp_err_t power_result = usb_host_lib_set_root_port_power(true);
        if (power_result != ESP_OK) {
            ESP_LOGE(TAG, "Host停止失败且恢复root port供电失败：cause=%s power=%s",
                     esp_err_to_name(cause), esp_err_to_name(power_result));
            return power_result;
        }
        __atomic_store_n(&s_root_port_powered, true, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s_stopping, false, __ATOMIC_RELEASE);
    return cause;
}

static esp_err_t stop_owned_workers(void)
{
    const EventBits_t mask = s_worker_task_mask;
    if (mask == 0U) {
        return ESP_OK;
    }
    if (s_task_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const EventBits_t completed = xEventGroupGetBits(s_task_events);
    if ((mask & TASK_EXIT_DESCRIPTOR) != 0U &&
        (completed & TASK_EXIT_DESCRIPTOR) == 0U &&
        s_descriptor_client_registered && s_descriptor_client != NULL) {
        (void)usb_host_client_unblock(s_descriptor_client);
    }
    if ((mask & TASK_EXIT_PROFILE) != 0U &&
        (completed & TASK_EXIT_PROFILE) == 0U && s_profile_task != NULL) {
        xTaskNotifyGive(s_profile_task);
    }
    if ((mask & TASK_EXIT_HID_EVENT) != 0U &&
        (completed & TASK_EXIT_HID_EVENT) == 0U && s_hid_event_queue != NULL) {
        const hid_event_t stop_event = {.handle = NULL, .type = HID_EVENT_STOP};
        queue_reset_count_dropped(s_hid_event_queue, &s_hid_event_queue_metrics);
        queue_metric_increment(&s_hid_event_queue_metrics.received);
        if (xQueueSend(s_hid_event_queue, &stop_event, 0) == pdTRUE) {
            queue_metric_observe_depth(&s_hid_event_queue_metrics, s_hid_event_queue);
        } else {
            queue_metric_increment(&s_hid_event_queue_metrics.dropped);
        }
    }
    if ((mask & TASK_EXIT_REPORT) != 0U &&
        (completed & TASK_EXIT_REPORT) == 0U && s_report_queue != NULL) {
        const raw_report_event_t stop_event = {.stop = true};
        queue_reset_count_dropped(s_report_queue, &s_report_queue_metrics);
        queue_metric_increment(&s_report_queue_metrics.received);
        if (xQueueSend(s_report_queue, &stop_event, 0) == pdTRUE) {
            queue_metric_observe_depth(&s_report_queue_metrics, s_report_queue);
        } else {
            queue_metric_increment(&s_report_queue_metrics.dropped);
        }
    }
    if ((mask & TASK_EXIT_CONTROL) != 0U &&
        (completed & TASK_EXIT_CONTROL) == 0U && s_control_queue != NULL) {
        const hid_control_event_t stop_event = {.stop = true};
        queue_reset_count_dropped(s_control_queue, &s_control_queue_metrics);
        queue_metric_increment(&s_control_queue_metrics.received);
        if (xQueueSend(s_control_queue, &stop_event, 0) == pdTRUE) {
            queue_metric_observe_depth(&s_control_queue_metrics, s_control_queue);
        } else {
            queue_metric_increment(&s_control_queue_metrics.dropped);
        }
    }
    if ((mask & TASK_EXIT_STATS) != 0U &&
        (completed & TASK_EXIT_STATS) == 0U && s_stats_task != NULL) {
        xTaskNotifyGive(s_stats_task);
    }

    if ((mask & TASK_EXIT_STALL_WATCH) != 0U && s_stall_watch_task != NULL) {
        xTaskNotifyGive(s_stall_watch_task);
    }
    const EventBits_t stopped = xEventGroupWaitBits(
        s_task_events, mask, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(HID_HOST_STOP_TIMEOUT_MS));
    if ((stopped & mask) != mask) {
        return ESP_ERR_TIMEOUT;
    }
    (void)xEventGroupClearBits(s_task_events, mask);
    s_worker_task_mask = 0;
    s_descriptor_task = NULL;
    s_profile_task = NULL;
    s_hid_event_task = NULL;
    s_report_task = NULL;
    s_control_task = NULL;
    s_stats_task = NULL;
    s_stall_watch_task = NULL;
    return ESP_OK;
}

static esp_err_t wait_for_host_library_task_exit(void)
{
    if (!s_host_task_created) {
        return ESP_OK;
    }
    if (s_task_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t stopped = xEventGroupGetBits(s_task_events);
    if ((stopped & TASK_EXIT_HOST_LIBRARY) == 0U) {
        stopped = xEventGroupWaitBits(
            s_task_events, TASK_EXIT_HOST_LIBRARY, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(HID_HOST_STOP_TIMEOUT_MS));
    }
    if ((stopped & TASK_EXIT_HOST_LIBRARY) == 0U) {
        return ESP_ERR_TIMEOUT;
    }
    (void)xEventGroupClearBits(s_task_events, TASK_EXIT_HOST_LIBRARY);
    s_host_task_created = false;
    s_usb_host_task = NULL;
    return ESP_OK;
}

static void delete_runtime_storage(void)
{
    if (s_hid_event_queue != NULL) {
        vQueueDelete(s_hid_event_queue);
        s_hid_event_queue = NULL;
    }
    if (s_report_queue != NULL) {
        vQueueDelete(s_report_queue);
        s_report_queue = NULL;
    }
    if (s_control_queue != NULL) {
        vQueueDelete(s_control_queue);
        s_control_queue = NULL;
    }
    if (s_profile_mutex != NULL) {
        vSemaphoreDelete(s_profile_mutex);
        s_profile_mutex = NULL;
    }
    if (s_vendor_session_state_mutex != NULL) {
        vSemaphoreDelete(s_vendor_session_state_mutex);
        s_vendor_session_state_mutex = NULL;
    }
    if (s_task_events != NULL) {
        vEventGroupDelete(s_task_events);
        s_task_events = NULL;
    }
    memset(s_interface_slots, 0, sizeof(s_interface_slots));
    memset(&s_profile_build, 0, sizeof(s_profile_build));
    memset(&s_profile_snapshot, 0, sizeof(s_profile_snapshot));
    memset(s_profile_serialized_blob, 0, sizeof(s_profile_serialized_blob));
    s_profile_device_addr = UINT8_MAX;
    s_report_callback = NULL;
    s_release_callback = NULL;
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API
    __atomic_store_n(&s_makcu_physical_buttons, 0U, __ATOMIC_RELEASE);
#endif
    s_profile_task = NULL;
    s_usb_host_task = NULL;
    s_descriptor_task = NULL;
    s_hid_event_task = NULL;
    s_report_task = NULL;
    s_control_task = NULL;
    s_stats_task = NULL;
    s_stall_watch_task = NULL;
    s_worker_task_mask = 0;
    s_host_task_created = false;
    __atomic_store_n(&s_usb_host_installed, false, __ATOMIC_RELEASE);
    s_hid_host_installed = false;
    s_descriptor_client = NULL;
    s_descriptor_client_registered = false;
    __atomic_store_n(&s_root_port_powered, false, __ATOMIC_RELEASE);
    s_free_all_requested = false;
    s_reports = 0;
    s_errors = 0;
    s_profile_revision = 0;
    s_profile_refresh_requested = false;
    s_vendor_reports = 0;
    s_vendor_input_failures = 0;
    s_vendor_control_requests = 0;
    s_vendor_control_failures = 0;
    memset(&s_hid_event_queue_metrics, 0, sizeof(s_hid_event_queue_metrics));
    memset(&s_report_queue_metrics, 0, sizeof(s_report_queue_metrics));
    memset(&s_control_queue_metrics, 0, sizeof(s_control_queue_metrics));
    s_logged_device_addr = UINT8_MAX;
    __atomic_store_n(&s_device_present, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_mouse_present, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_stopping, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_host_lib_stop_requested, false, __ATOMIC_RELEASE);
}

static esp_err_t dual_hid_host_stop_internal(void)
{
    __atomic_store_n(&s_stopping, true, __ATOMIC_RELEASE);

    esp_err_t result = wait_for_control_api_quiescence(HID_HOST_STOP_TIMEOUT_MS);
    if (result != ESP_OK) {
        return result;
    }

    result = wait_for_host_install_result(HID_HOST_STOP_TIMEOUT_MS);
    if (result != ESP_OK) {
        return result;
    }

    if (__atomic_load_n(&s_usb_host_installed, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&s_root_port_powered, __ATOMIC_ACQUIRE)) {
        const esp_err_t power_result = usb_host_lib_set_root_port_power(false);
        if (power_result != ESP_OK) {
            return resume_host_after_early_stop_failure(power_result);
        }
        __atomic_store_n(&s_root_port_powered, false, __ATOMIC_RELEASE);
    }

    if (__atomic_load_n(&s_usb_host_installed, __ATOMIC_ACQUIRE)) {
        result = wait_for_no_connected_devices(HID_HOST_STOP_TIMEOUT_MS);
        if (result != ESP_OK) {
            return resume_host_after_early_stop_failure(result);
        }
    }

    result = stop_owned_workers();
    if (result != ESP_OK) {
        return result;
    }

    if (s_hid_host_installed) {
        result = wait_for_hid_host_uninstall(HID_HOST_STOP_TIMEOUT_MS);
        if (result != ESP_OK) {
            /* Workers have already exited. Preserve the powered-off, stopping state
             * so a later stop() call can safely retry HID teardown. */
            return result;
        }
    }

    if (s_descriptor_client_registered && s_descriptor_client != NULL) {
        result = usb_host_client_deregister(s_descriptor_client);
        if (result != ESP_OK) {
            return result;
        }
        s_descriptor_client = NULL;
        s_descriptor_client_registered = false;
    }

    if (__atomic_load_n(&s_usb_host_installed, __ATOMIC_ACQUIRE)) {
        if (!s_free_all_requested) {
            (void)xEventGroupClearBits(s_task_events, TASK_EVENT_ALL_FREE);
            result = usb_host_device_free_all();
            if (result == ESP_OK) {
                s_free_all_requested = true;
                (void)xEventGroupSetBits(s_task_events, TASK_EVENT_ALL_FREE);
            } else if (result == ESP_ERR_NOT_FINISHED) {
                s_free_all_requested = true;
            } else {
                return result;
            }
        }
        if ((xEventGroupGetBits(s_task_events) & TASK_EVENT_ALL_FREE) == 0U) {
            const EventBits_t all_free = xEventGroupWaitBits(
                s_task_events, TASK_EVENT_ALL_FREE, pdFALSE, pdTRUE,
                pdMS_TO_TICKS(HID_HOST_STOP_TIMEOUT_MS));
            if ((all_free & TASK_EVENT_ALL_FREE) == 0U) {
                return ESP_ERR_TIMEOUT;
            }
        }

        if (s_host_task_created) {
            __atomic_store_n(&s_host_lib_stop_requested, true, __ATOMIC_RELEASE);
            (void)usb_host_lib_unblock();
            result = wait_for_host_library_task_exit();
            if (result != ESP_OK) {
                return result;
            }
        }

        result = usb_host_uninstall();
        if (result != ESP_OK) {
            return result;
        }
        __atomic_store_n(&s_usb_host_installed, false, __ATOMIC_RELEASE);
        __atomic_store_n(&s_root_port_powered, false, __ATOMIC_RELEASE);
    } else if (s_host_task_created) {
        result = wait_for_host_library_task_exit();
        if (result != ESP_OK) {
            return result;
        }
    }

    delete_runtime_storage();
    ESP_LOGI(TAG, "USB Host已合作式停止并卸载，可安全切换USB角色");
    return ESP_OK;
}

esp_err_t dual_hid_host_start(
    dual_physical_mouse_callback_t report_callback,
    dual_physical_release_callback_t release_callback)
{
    esp_err_t result = ESP_OK;
    if (!try_lifecycle_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_events != NULL ||
        __atomic_load_n(&s_usb_host_installed, __ATOMIC_ACQUIRE) || s_hid_host_installed ||
        s_host_task_created || s_descriptor_client_registered ||
        s_worker_task_mask != 0U) {
        release_lifecycle_lock();
        return ESP_ERR_INVALID_STATE;
    }
    s_task_events = xEventGroupCreate();
    if (s_task_events == NULL) {
        release_lifecycle_lock();
        return ESP_ERR_NO_MEM;
    }
    __atomic_store_n(&s_stopping, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_host_lib_stop_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_usb_host_installed, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_root_port_powered, false, __ATOMIC_RELEASE);
    s_free_all_requested = false;
    s_worker_task_mask = 0;
    s_stall_recovery_us = 0;
    s_cycle_input_wait_us = 0;
    __atomic_store_n(&s_last_report_us, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_mouse_ready_us, 0, __ATOMIC_RELEASE);
    s_report_callback = report_callback;
    s_release_callback = release_callback;
    s_usb_host_task = NULL;
    s_descriptor_task = NULL;
    s_profile_task = NULL;
    s_hid_event_task = NULL;
    s_report_task = NULL;
    s_control_task = NULL;
    s_stats_task = NULL;
    s_stall_watch_task = NULL;
    s_host_task_created = false;
    s_hid_host_installed = false;
    s_descriptor_client = NULL;
    s_descriptor_client_registered = false;
    s_reports = 0;
    s_errors = 0;
    s_logged_device_addr = UINT8_MAX;
    s_profile_mutex = xSemaphoreCreateMutex();
    s_vendor_session_state_mutex = xSemaphoreCreateMutex();
    if (s_profile_mutex == NULL || s_vendor_session_state_mutex == NULL) {
        goto no_memory;
    }
    __atomic_store_n(&s_vendor_session_epoch, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_vendor_session_peer_generation, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_vendor_session_first_request_us, 0, __ATOMIC_RELEASE);
    s_profile_revision = 0;
    s_profile_refresh_requested = false;
    s_vendor_reports = 0;
    s_vendor_input_failures = 0;
    s_vendor_control_requests = 0;
    s_vendor_control_failures = 0;
    __atomic_store_n(&s_device_present, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_mouse_present, false, __ATOMIC_RELEASE);
    memset(s_interface_slots, 0, sizeof(s_interface_slots));
    profile_reset_collector();
    s_hid_event_queue = xQueueCreate(HID_EVENT_QUEUE_LENGTH, sizeof(hid_event_t));
    s_report_queue = xQueueCreate(HID_REPORT_QUEUE_LENGTH, sizeof(raw_report_event_t));
    s_control_queue = xQueueCreate(HID_CONTROL_QUEUE_LENGTH, sizeof(hid_control_event_t));
    if (s_hid_event_queue == NULL || s_report_queue == NULL || s_control_queue == NULL) {
        goto no_memory;
    }
    if (xTaskCreate(usb_host_library_task, "usb_host_events", 4096,
                   xTaskGetCurrentTaskHandle(), 4, &s_usb_host_task) != pdPASS) {
        goto no_memory;
    }
    s_host_task_created = true;
    uint32_t notification = ESP_FAIL;
    const BaseType_t notified = xTaskNotifyWait(
        0, UINT32_MAX, &notification, pdMS_TO_TICKS(2000));
    if (notified != pdTRUE) {
        result = ESP_ERR_TIMEOUT;
        goto startup_failed_with_error;
    }
    if ((esp_err_t)notification != ESP_OK) {
        result = (esp_err_t)notification;
        goto startup_failed_with_error;
    }
    const usb_host_client_config_t descriptor_client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 4,
        .flags = {
            .notify_dev_removed = 0,
        },
        .async = {
            .client_event_callback = descriptor_client_event_callback,
            .callback_arg = NULL,
        },
    };
    result = usb_host_client_register(
        &descriptor_client_config, &s_descriptor_client);
    if (result != ESP_OK) {
        goto startup_failed_with_error;
    }
    s_descriptor_client_registered = true;
    if (xTaskCreate(descriptor_client_task, "usb_desc_client", 3072,
                    NULL, 6, &s_descriptor_task) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto startup_failed_with_error;
    }
    s_worker_task_mask |= TASK_EXIT_DESCRIPTOR;
    const hid_host_driver_config_t config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_driver_callback,
        .callback_arg = NULL,
    };
    result = hid_host_install(&config);
    if (result != ESP_OK) {
        goto startup_failed_with_error;
    }
    s_hid_host_installed = true;
    /* 创建发布任务后再消费枚举事件，避免首个设备的采集通知在句柄建立前丢失。 */
    if (xTaskCreate(profile_publish_task, "hid_profile_publish", 4096, NULL, 6,
                    &s_profile_task) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto startup_failed_with_error;
    }
    s_worker_task_mask |= TASK_EXIT_PROFILE;
    /* 直连控制通道：绕开组件单例 ctrl_xfer 的"在飞即锁死"问题。 */
    (void)dual_vendor_urb_start();
    if (xTaskCreate(hid_event_task, "hid_host_events", 4096, NULL, 5,
                    &s_hid_event_task) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto startup_failed_with_error;
    }
    s_worker_task_mask |= TASK_EXIT_HID_EVENT;
    /* 低于 HID 组件任务(5)：入队唤醒消费者后，先让组件重提交下一次 IN。 */
    if (xTaskCreate(raw_report_task, "hid_raw_reports", 3072, NULL, 4,
                    &s_report_task) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto startup_failed_with_error;
    }
    s_worker_task_mask |= TASK_EXIT_REPORT;
    if (xTaskCreate(hid_control_task, "hid_control_worker", HID_CONTROL_TASK_STACK,
                    NULL, 7, &s_control_task) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto startup_failed_with_error;
    }
    s_worker_task_mask |= TASK_EXIT_CONTROL;
    if (xTaskCreate(hid_stats_task, "hid_host_stats", 3072, NULL, 2,
                    &s_stats_task) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto startup_failed_with_error;
    }
    s_worker_task_mask |= TASK_EXIT_STATS;
    if (xTaskCreate(stall_watch_task, "usb_stall_watch", 3072, NULL, 6,
                    &s_stall_watch_task) != pdPASS) {
        result = ESP_ERR_NO_MEM;
        goto startup_failed_with_error;
    }
    s_worker_task_mask |= TASK_EXIT_STALL_WATCH;
    ESP_LOGI(TAG, "USB Host探测栈已启动；支持protocol=2标准鼠标接口，等待应用确认角色");
    release_lifecycle_lock();
    return ESP_OK;

no_memory:
    result = ESP_ERR_NO_MEM;
startup_failed_with_error:
    {
        const esp_err_t cleanup_result = dual_hid_host_stop_internal();
        if (cleanup_result != ESP_OK) {
            ESP_LOGE(TAG, "USB Host启动失败后资源回滚失败：start=%s cleanup=%s；保留现场供stop重试",
                     esp_err_to_name(result), esp_err_to_name(cleanup_result));
            result = cleanup_result;
        }
    }
    release_lifecycle_lock();
    return result;
}

esp_err_t dual_hid_host_stop(void)
{
    if (!try_lifecycle_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dual_hid_host_mouse_present() && !stopping_requested()) {
        release_lifecycle_lock();
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = dual_hid_host_stop_internal();
    release_lifecycle_lock();
    return result;
}

bool dual_hid_host_device_present(void)
{
    return __atomic_load_n(&s_device_present, __ATOMIC_ACQUIRE);
}

bool dual_hid_host_mouse_present(void)
{
    return __atomic_load_n(&s_mouse_present, __ATOMIC_ACQUIRE);
}

size_t dual_hid_host_copy_profile_blob(uint32_t offset, uint8_t *output, size_t capacity,
                                       uint32_t *total_length)
{
    if (s_profile_mutex == NULL || total_length == NULL ||
        (output == NULL && capacity != 0U)) {
        return 0U;
    }
    xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
    const size_t total = s_profile_serialized_length;
    *total_length = (uint32_t)total;
    size_t copied = 0U;
    if (offset < total) {
        copied = total - offset;
        if (copied > capacity) {
            copied = capacity;
        }
        memcpy(output, &s_profile_serialized_blob[offset], copied);
    }
    xSemaphoreGive(s_profile_mutex);
    return copied;
}

void dual_hid_host_handle_control_frame(const dual_frame_t *frame)
{
    if (frame != NULL && frame->type == DUAL_MESSAGE_VENDOR_SESSION_BEGIN) {
        uint32_t p_generation = 0U;
        uint32_t m_generation = 0U;
        uint32_t epoch = 0U;
        if (!dual_vendor_session_decode(frame->payload, frame->payload_length,
                                        &p_generation, &m_generation, &epoch) ||
            !link_vendor_session_identity_matches(
                p_generation, m_generation, dual_uart1_peer_generation(),
                dual_uart1_generation(), epoch) ||
            s_vendor_session_state_mutex == NULL) {
            ++s_vendor_control_failures;
            return;
        }

        bool accepted = false;
        xSemaphoreTake(s_vendor_session_state_mutex, portMAX_DELAY);
        const uint32_t current_peer = __atomic_load_n(
            &s_vendor_session_peer_generation, __ATOMIC_ACQUIRE);
        const uint32_t current_epoch = __atomic_load_n(
            &s_vendor_session_epoch, __ATOMIC_ACQUIRE);
        const bool duplicate = current_peer == p_generation && current_epoch == epoch;
        const bool newer = current_peer != p_generation ||
            link_vendor_session_epoch_is_newer(epoch, current_epoch);
        if (duplicate || newer) {
            if (!duplicate) {
                __atomic_store_n(&s_vendor_session_peer_generation,
                                 p_generation, __ATOMIC_RELEASE);
                __atomic_store_n(&s_vendor_session_epoch, epoch, __ATOMIC_RELEASE);
                __atomic_store_n(&s_vendor_session_first_request_us, 0,
                                 __ATOMIC_RELEASE);
                if (s_control_queue != NULL) {
                    queue_reset_count_dropped(s_control_queue,
                                              &s_control_queue_metrics);
                }
                /* 只使旧控制帧失效；队列中的物理输入/button edge仍按到达顺序发送。 */
                dual_uart1_cancel_vendor_control_session();
            }
            accepted = true;
        }
        xSemaphoreGive(s_vendor_session_state_mutex);
        if (accepted) {
            (void)dual_uart1_send_vendor_session_ack(
                p_generation, m_generation, epoch);
        } else {
            ESP_LOGW(TAG, "丢弃过期Vendor会话BEGIN：peer=%" PRIu32
                     " epoch=%" PRIu32 " current=%" PRIu32,
                     p_generation, epoch, current_epoch);
        }
        return;
    }

    __atomic_add_fetch(&s_control_api_users, 1U, __ATOMIC_ACQUIRE);
    const bool queue_candidate = frame != NULL &&
        (frame->type == DUAL_MESSAGE_HID_SET_REPORT ||
         frame->type == DUAL_MESSAGE_HID_GET_REPORT_REQUEST ||
         frame->type == DUAL_MESSAGE_VENDOR_CONTROL_REQUEST);
    if (queue_candidate) {
        queue_metric_increment(&s_control_queue_metrics.received);
    }
    /*
     * 到达节奏打点：用"请求到达时刻"判密集突发，而不是"控制任务处理时刻"。
     * 实测教训：原先用处理时刻，超时期间每次处理要等 800 ms → 节奏永远稀疏 →
     * 突发判据永不成立 → 让路一次都没触发（计数 vendor_motion=0）。
     * Vendor 控制请求同属厂商事务，一并纳入打点。
     */
    if (frame != NULL &&
        (frame->type == DUAL_MESSAGE_HID_SET_REPORT ||
         frame->type == DUAL_MESSAGE_HID_GET_REPORT_REQUEST ||
         frame->type == DUAL_MESSAGE_VENDOR_CONTROL_REQUEST)) {
        host_vendor_note_arrival();
    }
    if (frame == NULL || stopping_requested() || s_control_queue == NULL) {
        if (queue_candidate) {
            queue_metric_increment(&s_control_queue_metrics.rejected);
        }
        __atomic_sub_fetch(&s_control_api_users, 1U, __ATOMIC_RELEASE);
        return;
    }
    hid_control_event_t request = {0};
    bool decoded = false;
    if (frame->type == DUAL_MESSAGE_HID_SET_REPORT) {
        const uint8_t *data = NULL;
        size_t data_length = 0;
        decoded = dual_hid_set_report_decode(
            frame->payload, frame->payload_length, &request.transaction_id,
            &request.interface_number, &request.report_id, &request.report_type,
            &data, &data_length);
        if (decoded) {
            request.length = (uint8_t)data_length;
            if (data_length != 0U) {
                memcpy(request.data, data, data_length);
            }
        }
    } else if (frame->type == DUAL_MESSAGE_HID_GET_REPORT_REQUEST) {
        decoded = dual_hid_get_request_decode(
            frame->payload, frame->payload_length, &request.transaction_id,
            &request.interface_number, &request.report_id, &request.report_type,
            &request.requested_length);
        request.get_report = decoded;
    } else if (frame->type == DUAL_MESSAGE_VENDOR_CONTROL_REQUEST) {
        const uint8_t *data = NULL;
        size_t data_length = 0;
        decoded = dual_vendor_control_request_decode(
            frame->payload, frame->payload_length, &request.transaction_id,
            &request.bm_request_type, &request.b_request, &request.w_value,
            &request.w_index, &request.w_length, &data, &data_length);
        if (decoded) {
            request.vendor_control = true;
            request.length = (uint8_t)data_length;
            if (data_length != 0U) {
                memcpy(request.data, data, data_length);
            }
        }
    }
    if (!decoded) {
        ++s_vendor_control_failures;
        if (queue_candidate) {
            queue_metric_increment(&s_control_queue_metrics.rejected);
        }
        __atomic_sub_fetch(&s_control_api_users, 1U, __ATOMIC_RELEASE);
        return;
    }
    request.vendor_session_epoch = __atomic_load_n(
        &s_vendor_session_epoch, __ATOMIC_ACQUIRE);
    request.vendor_session_peer_generation = __atomic_load_n(
        &s_vendor_session_peer_generation, __ATOMIC_ACQUIRE);
    (void)vendor_session_note_first_request(request.vendor_session_epoch);
    ++s_vendor_control_requests;
    if (xQueueSend(s_control_queue, &request, 0) != pdTRUE) {
        queue_metric_increment(&s_control_queue_metrics.dropped);
        ++s_vendor_control_failures;
        if (request.get_report) {
            (void)dual_uart1_send_hid_get_response(
                request.transaction_id, DUAL_HID_REPORT_STATUS_TIMEOUT,
                request.interface_number, request.report_id, NULL, 0);
        }
    } else {
        queue_metric_observe_depth(&s_control_queue_metrics, s_control_queue);
    }
    __atomic_sub_fetch(&s_control_api_users, 1U, __ATOMIC_RELEASE);
}

void dual_hid_host_clear_control_queue(void)
{
    __atomic_add_fetch(&s_control_api_users, 1U, __ATOMIC_ACQUIRE);
    if (!stopping_requested() && s_control_queue != NULL) {
        queue_reset_count_dropped(s_control_queue, &s_control_queue_metrics);
    }
    __atomic_sub_fetch(&s_control_api_users, 1U, __ATOMIC_RELEASE);
}

bool dual_hid_host_vendor_busy(void)
{
    /*
     * 判据：**最近是否有厂商请求到达**（活动窗口），不是"密度"。
     * 实测依据：P 的转发把请求整形成约 106 次/秒（M 侧实测最小到达间隔
     * vmin_gap_us = 9410），根本不存在"5 ms 内的密集突发"——按密度判会永远不成立
     * （曾实测 vendor_motion=0，修复空转）。窗口取 30 ms（约 3 倍实测间隔）：
     * G HUB 初始化/查询期间持续成立 → 移动持续让路；稳态稀疏轮询时基本不触发。
     *
     * 2026-09-27：移动抑制全部移除后，本函数在固件内**已无调用者**（原两处调用分别是
     * hid_host_mouse.c 的活跃让路与 dual_proxy_app.c 的 on_mouse_report 死路径，后者
     * 物理上不会被调用）。保留接口以便回退或将来复用。
     */
    return s_vendor_arrival_last_us != 0 &&
        esp_timer_get_time() - s_vendor_arrival_last_us < 30000LL;
}

/*
 * 设备级 Vendor 控制传输自测入口（2026-09-27）：供 UART0 诊断命令调用，
 * 直接对物理设备发一笔厂商自定义 EP0 请求，用来验证 vendor_urb 的通用控制通道
 * （P 侧 TinyUSB 回调那条路需要主机发起 vendor 请求才能触发，Windows 用户态
 * 对 HID 类设备发不了，所以先用这个入口把 M 侧与物理设备这一段单独验掉）。
 * 无活动接口时返回 ESP_ERR_INVALID_STATE；设备 STALL 时返回 ESP_FAIL。
 */
esp_err_t dual_hid_host_vendor_selftest(
    uint8_t bm_request_type,
    uint8_t b_request,
    uint16_t w_value,
    uint16_t w_index,
    uint16_t w_length,
    uint32_t timeout_ms,
    uint8_t *out_data,
    size_t out_capacity,
    size_t *out_length)
{
    hid_host_device_handle_t handle = NULL;
    portENTER_CRITICAL(&s_interface_state_mux);
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        const hid_interface_slot_t *slot = &s_interface_slots[index];
        if (slot->active && slot->handle != NULL) {
            handle = slot->handle;
            break;
        }
    }
    portEXIT_CRITICAL(&s_interface_state_mux);
    if (handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return dual_vendor_urb_control(params.addr, bm_request_type, b_request,
                                   w_value, w_index, NULL, w_length, timeout_ms,
                                   out_data, out_capacity, out_length);
}

/*
 * 诊断注入入口（2026-09-27）：把一段原始鼠标报告投进 RX 回调，使统计、入队、转发
 * 与物理报告走**完全相同**的路径（不是复制一套逻辑）。
 * raw_report 可以是完整 USB 报告（含 Report ID），也可以是当前鼠标布局的正文；
 * 后者在这里自动补上已枚举到的 Report ID。
 */
bool dual_hid_host_inject_report(const uint8_t *raw_report, size_t length)
{
    if (raw_report == NULL || length == 0U || length > sizeof(s_injected_report)) {
        return false;
    }
    hid_host_device_handle_t handle = NULL;
    portENTER_CRITICAL(&s_interface_state_mux);
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        const hid_interface_slot_t *slot = &s_interface_slots[index];
        if (slot->active && slot->handle != NULL && slot->mouse_interface) {
            handle = slot->handle;
            break;
        }
    }
    portEXIT_CRITICAL(&s_interface_state_mux);
    if (handle == NULL) {
        return false;
    }
    /* 注入接口同时接受两种形式：
     * 1. 与 USB Host 回调完全一致的完整报告（含 Report ID）；
     * 2. 鼠标报告正文（不含 Report ID），由当前已识别布局补上 ID。
     * 这样诊断工具不需要硬编码某一只鼠标的 Report ID，同时仍保留
     * 原始报告注入能力。 */
    bool has_report_id = false;
    uint8_t mouse_report_id = 0U;
    size_t body_length = 0U;
    portENTER_CRITICAL(&s_interface_state_mux);
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        const hid_interface_slot_t *slot = &s_interface_slots[index];
        if (slot->active && slot->handle == handle && slot->mouse_interface) {
            has_report_id = slot->has_report_id;
            mouse_report_id = slot->mouse_report_id;
            body_length = slot->mouse_layout.report_bytes;
            break;
        }
    }
    portEXIT_CRITICAL(&s_interface_state_mux);
    if (body_length == 0U) {
        return false;
    }
    if (has_report_id && length == body_length) {
        if (length + 1U > sizeof(s_injected_report)) {
            return false;
        }
        s_injected_report[0] = mouse_report_id;
        memcpy(&s_injected_report[1], raw_report, length);
        s_injected_report_length = length + 1U;
    } else {
        memcpy(s_injected_report, raw_report, length);
        s_injected_report_length = length;
    }
    __atomic_store_n(&s_injected_report_pending, true, __ATOMIC_RELEASE);
    hid_interface_callback(handle, HID_HOST_INTERFACE_EVENT_INPUT_REPORT, NULL);
    return true;
}
