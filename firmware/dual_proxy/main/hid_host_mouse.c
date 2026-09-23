#include "hid_host_mouse.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#include "dual_status_led.h"
#include "hid_device_profile.h"
#include "hid_report_layout.h"
#include "uart1_link.h"

#define HID_EVENT_QUEUE_LENGTH 16
#define HID_REPORT_QUEUE_LENGTH 64
#define HID_RAW_REPORT_MAX 64
#define HID_PROFILE_DEBOUNCE_MS 200
#define HID_INTERFACE_SLOT_COUNT HID_PROFILE_MAX_REPORT_DESCRIPTORS
#define HID_CONTROL_QUEUE_LENGTH 8
#define HID_CONTROL_TASK_STACK 4096

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
    uint8_t report_id;
    uint8_t length;
    bool mouse_report;
    uint8_t data[HID_RAW_REPORT_MAX];
} raw_report_event_t;

typedef struct {
    bool active;
    bool mouse_interface;
    bool has_report_id;
    uint8_t mouse_report_id;
    uint8_t interface_number;
    hid_host_device_handle_t handle;
} hid_interface_slot_t;

typedef struct {
    bool get_report;
    uint16_t transaction_id;
    uint8_t interface_number;
    uint8_t report_id;
    uint8_t report_type;
    uint8_t requested_length;
    uint8_t length;
    uint8_t data[DUAL_HID_CONTROL_MAX_DATA];
} hid_control_event_t;

static QueueHandle_t s_hid_event_queue;
static QueueHandle_t s_report_queue;
static dual_physical_mouse_callback_t s_report_callback;
static dual_physical_release_callback_t s_release_callback;
static volatile uint32_t s_reports;
static volatile uint32_t s_errors;
static uint8_t s_logged_device_addr = UINT8_MAX;
static hid_device_profile_t s_profile_build;
static hid_device_profile_t s_profile_snapshot;
static uint8_t s_profile_serialized_blob[HID_PROFILE_MAX_BLOB];
static uint8_t s_profile_device_addr = UINT8_MAX;
static volatile uint32_t s_profile_revision;
static TaskHandle_t s_profile_task;
static SemaphoreHandle_t s_profile_mutex;
static usb_host_client_handle_t s_descriptor_client;
static hid_interface_slot_t s_interface_slots[HID_INTERFACE_SLOT_COUNT];
static QueueHandle_t s_control_queue;
static TaskHandle_t s_control_task;
static volatile uint32_t s_vendor_reports;
static volatile uint32_t s_vendor_input_failures;
static volatile uint32_t s_vendor_control_requests;
static volatile uint32_t s_vendor_control_failures;
static bool s_device_present;

#define HID_STATS_PERIOD_MS 5000

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

static bool any_interface_active(void)
{
    for (size_t index = 0; index < HID_INTERFACE_SLOT_COUNT; ++index) {
        if (s_interface_slots[index].active) {
            return true;
        }
    }
    return false;
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
    ++s_profile_revision;
    hid_device_profile_init(&s_profile_build);
    memset(s_interface_slots, 0, sizeof(s_interface_slots));
    if (s_control_queue != NULL) {
        xQueueReset(s_control_queue);
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

static void profile_collect_raw_usb_descriptors(uint8_t device_addr)
{
    if (s_descriptor_client == NULL) {
        return;
    }
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
        profile_collect_raw_usb_descriptors(params->addr);
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
        xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
        const uint32_t revision = s_profile_revision;
        xSemaphoreGive(s_profile_mutex);
        vTaskDelay(pdMS_TO_TICKS(HID_PROFILE_DEBOUNCE_MS));
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
                &s_profile_snapshot, s_profile_serialized_blob,
                sizeof(s_profile_serialized_blob), &blob_length)) {
            ++s_errors;
            ESP_LOGW(TAG, "Profile序列化失败：interfaces=%u partial=%s",
                     s_profile_snapshot.report_descriptor_count,
                     (s_profile_snapshot.flags & HID_PROFILE_FLAG_PARTIAL) != 0 ? "yes" : "no");
            continue;
        }
        xSemaphoreTake(s_profile_mutex, portMAX_DELAY);
        const bool stale = revision != s_profile_revision;
        xSemaphoreGive(s_profile_mutex);
        if (stale) {
            continue;
        }
        const uint32_t crc32 = hid_profile_crc32(s_profile_serialized_blob, blob_length);
        if (dual_uart1_queue_profile(
                s_profile_serialized_blob, blob_length, crc32) != ESP_OK) {
            ++s_errors;
            ESP_LOGW(TAG, "Profile排队发送失败：length=%u", (unsigned)blob_length);
        } else {
            ESP_LOGI(TAG, "Profile观察快照已排队：addr=%u interfaces=%u length=%u partial=%s",
                     device_addr, s_profile_snapshot.report_descriptor_count,
                     (unsigned)blob_length,
                     (s_profile_snapshot.flags & HID_PROFILE_FLAG_PARTIAL) != 0 ? "yes" : "no");
        }
    }
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

static void raw_report_task(void *argument)
{
    (void)argument;
    raw_report_event_t event;
    while (true) {
        if (xQueueReceive(s_report_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.length == 0U ||
            dual_uart1_send_raw_hid_input(
                event.interface_number, event.report_id, event.data,
                event.length) != ESP_OK) {
            ++s_errors;
            ++s_vendor_input_failures;
        } else if (event.mouse_report) {
            ++s_reports;
        } else {
            ++s_vendor_reports;
        }
    }
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
        hid_interface_slot_t *slot = find_interface_slot(request.interface_number);
        if (slot == NULL || !slot->active || slot->handle == NULL) {
            ++s_vendor_control_failures;
            if (request.get_report) {
                (void)dual_uart1_send_hid_get_response(
                    request.transaction_id, DUAL_HID_REPORT_STATUS_INVALID,
                    request.interface_number, request.report_id, NULL, 0);
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
            const esp_err_t result = hid_class_request_get_report(
                slot->handle, request.report_type, request.report_id,
                response, &response_length);
            const uint8_t status = result == ESP_OK
                ? DUAL_HID_REPORT_STATUS_OK : DUAL_HID_REPORT_STATUS_UNSUPPORTED;
            if (result != ESP_OK) {
                ++s_vendor_control_failures;
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
            if (dual_uart1_send_hid_get_response(
                    request.transaction_id, status, request.interface_number,
                    request.report_id,
                    status == DUAL_HID_REPORT_STATUS_OK ? response_data : NULL,
                    status == DUAL_HID_REPORT_STATUS_OK ? response_length : 0) != ESP_OK) {
                ++s_vendor_control_failures;
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
            const esp_err_t result = hid_class_request_set_report(
                slot->handle, request.report_type, request.report_id,
                mutable_data, mutable_length);
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
            if (result != ESP_OK) {
                ++s_vendor_control_failures;
            }
        }
    }
}

static void hid_stats_task(void *argument)
{
    (void)argument;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(HID_STATS_PERIOD_MS));
        ESP_LOGI(TAG, "Host HID统计：reports=%" PRIu32 " vendor_reports=%" PRIu32
                 " input_fail=%" PRIu32 " control=%" PRIu32
                 " control_fail=%" PRIu32 " errors=%" PRIu32,
                 s_reports, s_vendor_reports, s_vendor_input_failures,
                 s_vendor_control_requests, s_vendor_control_failures,
                 s_errors);
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
        hid_interface_slot_t *slot = find_interface_slot(params.iface_num);
        if (slot == NULL || !slot->active) {
            ++s_errors;
            return;
        }
        uint8_t report_id = 0;
        size_t data_offset = 0;
        if (slot->has_report_id) {
            if (length < 1U) {
                ++s_errors;
                return;
            }
            report_id = data[0];
            data_offset = 1U;
        }
        const size_t forwarded_length = length - data_offset;
        if (forwarded_length > DUAL_HID_RAW_INPUT_MAX_DATA) {
            ++s_errors;
            ++s_vendor_input_failures;
            return;
        }
        raw_report_event_t queued = {
            .interface_number = params.iface_num,
            .report_id = report_id,
            .length = (uint8_t)forwarded_length,
            .mouse_report = slot->mouse_interface &&
                report_id == slot->mouse_report_id,
        };
        memcpy(queued.data, &data[data_offset], forwarded_length);
        if (xQueueSend(s_report_queue, &queued, 0) != pdTRUE) {
            ++s_errors;
            if (s_release_callback != NULL) {
                s_release_callback(false);
            }
        }
        return;
    }
    if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        hid_interface_slot_t *slot = find_interface_slot(params.iface_num);
        const bool mouse_interface = slot != NULL && slot->mouse_interface;
        clear_interface_slot(params.iface_num);
        if (mouse_interface) {
            ESP_LOGW(TAG, "鼠标 HID 接口断开：interface=%u", params.iface_num);
            dual_status_led_set_host_mouse_ready(false);
        } else {
            ESP_LOGI(TAG, "vendor HID接口断开：interface=%u", params.iface_num);
            if (s_control_queue != NULL) {
                xQueueReset(s_control_queue);
            }
        }
        if (s_device_present && !any_interface_active()) {
            s_device_present = false;
            ESP_LOGW(TAG, "物理USB HID设备已完全拔出");
            if (s_release_callback != NULL) {
                s_release_callback(true);
            }
        }
        profile_reset_collector();
        (void)hid_host_device_close(handle);
        return;
    }
    if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        ++s_errors;
        ESP_LOGW(TAG, "标准鼠标接口传输错误：interface=%u", params.iface_num);
        if (s_release_callback != NULL) {
            s_release_callback(false);
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
            s_release_callback(false);
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
        log_report_descriptor(event.handle, params.iface_num);
        size_t descriptor_length = 0;
        uint8_t *descriptor = hid_host_get_report_descriptor(
            event.handle, &descriptor_length);
        profile_collect_interface(
            &params, info_valid ? &info : NULL, descriptor, descriptor_length);
        hid_interface_slot_t *slot = allocate_interface_slot(params.iface_num);
        if (slot == NULL) {
            ++s_errors;
            ESP_LOGW(TAG, "HID接口槽位耗尽，拒绝接口=%u", params.iface_num);
            (void)hid_host_device_close(event.handle);
            continue;
        }
        slot->handle = event.handle;
        hid_mouse_report_layout_t mouse_layout;
        memset(&mouse_layout, 0, sizeof(mouse_layout));
        slot->mouse_interface = hid_report_find_mouse_layout(
            descriptor, descriptor_length, &mouse_layout);
        slot->mouse_report_id = slot->mouse_interface
            ? mouse_layout.report_id : 0U;
        slot->has_report_id = report_descriptor_has_report_id(
            descriptor, descriptor_length);
        if (!slot->mouse_interface) {
            ESP_LOGI(TAG,
                      "已打开并启动vendor HID接口：interface=%u subclass=%u protocol=%u report_id=%s",
                     params.iface_num, params.sub_class, params.proto,
                     slot->has_report_id ? "yes" : "no");
        }
        /* 保持设备上电后的 Report Protocol，与 Windows 直连枚举一致。
         * 强制 Boot Protocol 可能改变厂商接口的 HID++ 工作状态。 */
        if (result == ESP_OK) {
            const esp_err_t idle_result = hid_class_request_set_idle(
                event.handle, 0, 0);
            ESP_LOGI(TAG, "HID SET_IDLE: interface=%u result=%s",
                     params.iface_num, esp_err_to_name(idle_result));
        }
        if (result == ESP_OK) {
            result = hid_host_device_start(event.handle);
        }
        if (result != ESP_OK) {
            ++s_errors;
            ESP_LOGE(TAG, "HID接口启动失败：interface=%u error=%s",
                     params.iface_num, esp_err_to_name(result));
            clear_interface_slot(params.iface_num);
            (void)hid_host_device_close(event.handle);
            if (s_release_callback != NULL) {
                s_release_callback(false);
            }
        } else {
            s_device_present = true;
            if (slot->mouse_interface) {
                dual_status_led_set_host_mouse_ready(true);
                ESP_LOGI(TAG,
                         "动态鼠标输入已启动：interface=%u report_id=%u bytes=%u",
                         params.iface_num, mouse_layout.report_id,
                         mouse_layout.report_bytes);
            }
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
        if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
            ++s_errors;
            ESP_LOGW(TAG, "描述符客户端事件处理失败：%s", esp_err_to_name(result));
        }
    }
}

esp_err_t dual_hid_host_start(
    dual_physical_mouse_callback_t report_callback,
    dual_physical_release_callback_t release_callback)
{
    s_report_callback = report_callback;
    s_release_callback = release_callback;
    s_profile_task = NULL;
    s_profile_mutex = xSemaphoreCreateMutex();
    if (s_profile_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_profile_revision = 0;
    s_vendor_reports = 0;
    s_vendor_input_failures = 0;
    s_vendor_control_requests = 0;
    s_vendor_control_failures = 0;
    s_device_present = false;
    memset(s_interface_slots, 0, sizeof(s_interface_slots));
    profile_reset_collector();
    s_hid_event_queue = xQueueCreate(HID_EVENT_QUEUE_LENGTH, sizeof(hid_event_t));
    s_report_queue = xQueueCreate(HID_REPORT_QUEUE_LENGTH, sizeof(raw_report_event_t));
    s_control_queue = xQueueCreate(HID_CONTROL_QUEUE_LENGTH, sizeof(hid_control_event_t));
    if (s_hid_event_queue == NULL || s_report_queue == NULL || s_control_queue == NULL) {
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
    esp_err_t result = usb_host_client_register(
        &descriptor_client_config, &s_descriptor_client);
    if (result != ESP_OK) {
        return result;
    }
    if (xTaskCreate(descriptor_client_task, "usb_desc_client", 3072,
                    NULL, 2, NULL) != pdPASS) {
        (void)usb_host_client_deregister(s_descriptor_client);
        s_descriptor_client = NULL;
        return ESP_ERR_NO_MEM;
    }
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
        return result;
    }
    /* 创建发布任务后再消费枚举事件，避免首个设备的采集通知在句柄建立前丢失。 */
    if (xTaskCreate(profile_publish_task, "hid_profile_publish", 4096, NULL, 2,
        &s_profile_task) != pdPASS ||
        xTaskCreate(hid_event_task, "hid_host_events", 4096, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(raw_report_task, "hid_raw_reports", 3072, NULL, 6, NULL) != pdPASS ||
        xTaskCreate(hid_control_task, "hid_control_worker", HID_CONTROL_TASK_STACK, NULL, 5, &s_control_task) != pdPASS ||
        xTaskCreate(hid_stats_task, "hid_host_stats", 2048, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB角色锁定：MOUSE_HOST；仅接受protocol=2标准鼠标接口");
    return ESP_OK;
}

void dual_hid_host_handle_control_frame(const dual_frame_t *frame)
{
    if (frame == NULL || s_control_queue == NULL) {
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
    }
    if (!decoded) {
        ++s_vendor_control_failures;
        return;
    }
    ++s_vendor_control_requests;
    if (xQueueSend(s_control_queue, &request, 0) != pdTRUE) {
        ++s_vendor_control_failures;
        if (request.get_report) {
            (void)dual_uart1_send_hid_get_response(
                request.transaction_id, DUAL_HID_REPORT_STATUS_TIMEOUT,
                request.interface_number, request.report_id, NULL, 0);
        }
    }
}

void dual_hid_host_clear_control_queue(void)
{
    if (s_control_queue != NULL) {
        xQueueReset(s_control_queue);
    }
}
