#include "hid_output.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "output_router.h"
#include "usb_cdc_input.h"
#include "usb_output_liveness.h"

#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE 2
#define MOUSE_REPORT_LENGTH 7
#define CONTROL_QUEUE_LENGTH 32
#define USB_HID_INTERFACE_COUNT 1
#define USB_CDC_INTERFACE_COUNT 2
#define USB_HID_INTERFACE 0
#define USB_CDC_INTERFACE 0
#define USB_HID_ENDPOINT 0x81
#define USB_CDC_NOTIFICATION_ENDPOINT 0x82
#define USB_CDC_DATA_OUT_ENDPOINT 0x03
#define USB_CDC_DATA_IN_ENDPOINT 0x83
#define USB_HID_CONFIG_TOTAL_LENGTH (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define USB_CDC_CONFIG_TOTAL_LENGTH (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define USB_PID_CDC_ONLY 0x4001
#define USB_PID_HID_ONLY 0x4004
#define MOUSE_SEND_PERIOD_MS 2
#define USB_UNAVAILABLE_TIMEOUT_MS 100
#define STATISTICS_PERIOD_MS 1000
#define COMPLETION_LATENCY_BUCKET_COUNT 7

static const char *TAG = "hid_output";
static QueueHandle_t s_control_queue;
static SemaphoreHandle_t s_control_mutex;
static TaskHandle_t s_hid_sender_task;
static portMUX_TYPE s_mouse_lock = portMUX_INITIALIZER_UNLOCKED;

typedef enum {
    HID_CONTROL_KEYBOARD,
    HID_CONTROL_MOUSE_BUTTONS,
    HID_CONTROL_RELEASE_ALL,
} hid_control_type_t;

typedef struct {
    hid_control_type_t type;
    uint8_t length;
    uint8_t payload[8];
} hid_control_event_t;

typedef struct {
    int64_t pending_x;
    int64_t pending_y;
    int64_t pending_wheel;
    int64_t pending_pan;
    int64_t received_x;
    int64_t received_y;
    int64_t submitted_x;
    int64_t submitted_y;
    int64_t completed_x;
    int64_t completed_y;
    int64_t discarded_x;
    int64_t discarded_y;
    uint64_t received_reports;
    uint64_t submitted_reports;
    uint64_t completed_reports;
    uint64_t control_drops;
    uint64_t max_pending_x;
    uint64_t max_pending_y;
    uint64_t pending_motion_reports;
    uint64_t max_pending_motion_reports;
    uint8_t received_buttons;
    uint8_t submitted_buttons;
    bool release_pending;
} mouse_state_t;

typedef struct {
    uint64_t sender_ticks;
    uint64_t mounted_true;
    uint64_t mounted_false;
    uint64_t ready_true;
    uint64_t ready_false;
    uint64_t motion_received;
    uint64_t motion_submitted;
    uint64_t motion_completed;
    uint64_t submit_failed;
    uint64_t completion_latency_samples;
    uint64_t completion_latency_total_us;
    uint64_t completion_latency_max_us;
    uint64_t completion_latency_buckets[COMPLETION_LATENCY_BUCKET_COUNT];
    uint64_t completion_gap_max_us;
} diagnostic_state_t;

static mouse_state_t s_mouse;
static diagnostic_state_t s_diagnostics;
static UBaseType_t s_control_queue_high_water;
static int64_t s_mouse_submit_time_us;
static int64_t s_last_motion_completion_time_us;
static usb_output_liveness_t s_usb_output_liveness;
static usb_device_profile_t s_usb_profile = USB_DEVICE_PROFILE_CDC;

static const tusb_desc_device_t s_cdc_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID,
    .idProduct = USB_PID_CDC_ONLY,
    .bcdDevice = 0x0200,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_t s_hid_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID,
    .idProduct = USB_PID_HID_ONLY,
    .bcdDevice = 0x0200,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const char s_usb_language_en_us[] = {0x09, 0x04};
static const char *s_cdc_string_descriptors[] = {
    s_usb_language_en_us,
    "HID Bridge",
    "HID Bridge CDC",
    "HIDBRIDGE-CDC",
    "HID Bridge Serial Interface",
};
static const char *s_hid_string_descriptors[] = {
    s_usb_language_en_us,
    "HID Bridge",
    "USB Keyboard with Touchpad",
    "HIDBRIDGE-HID",
    "Keyboard and Relative Touchpad",
};

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),

    /* 常见带触摸板键盘以相对指针集合暴露触摸板；当前协议不承载绝对坐标或触点。 */
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x85, REPORT_ID_MOUSE,
    0x09, 0x01,       // Usage (Pointer)
    0xA1, 0x00,       // Collection (Physical)
    0x05, 0x09,       // Usage Page (Button)
    0x19, 0x01,
    0x29, 0x05,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x05,
    0x75, 0x01,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x03,
    0x81, 0x03,
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x30,       // Usage (X)
    0x09, 0x31,       // Usage (Y)
    0x16, 0x00, 0x80, // Logical Minimum (-32768)
    0x26, 0xFF, 0x7F, // Logical Maximum (32767)
    0x75, 0x10,
    0x95, 0x02,
    0x81, 0x06,       // Input (Data, Variable, Relative)
    0x09, 0x38,       // Usage (Wheel)
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x06,
    0x05, 0x0C,       // Usage Page (Consumer)
    0x0A, 0x38, 0x02, // Usage (AC Pan)
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x06,
    0xC0,
    0xC0,
};

static const uint8_t s_hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        USB_HID_INTERFACE_COUNT,
        0,
        USB_HID_CONFIG_TOTAL_LENGTH,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    TUD_HID_DESCRIPTOR(
        USB_HID_INTERFACE,
        4,
        HID_ITF_PROTOCOL_NONE,
        sizeof(s_hid_report_descriptor),
        USB_HID_ENDPOINT,
        16,
        1),
};

static const uint8_t s_cdc_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        USB_CDC_INTERFACE_COUNT,
        0,
        USB_CDC_CONFIG_TOTAL_LENGTH,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    TUD_CDC_DESCRIPTOR(
        USB_CDC_INTERFACE,
        4,
        USB_CDC_NOTIFICATION_ENDPOINT,
        8,
        USB_CDC_DATA_OUT_ENDPOINT,
        USB_CDC_DATA_IN_ENDPOINT,
        64),
};

_Static_assert(
    sizeof(s_hid_configuration_descriptor) == USB_HID_CONFIG_TOTAL_LENGTH,
    "HID 配置描述符长度必须与 wTotalLength 一致");
_Static_assert(
    sizeof(s_cdc_configuration_descriptor) == USB_CDC_CONFIG_TOTAL_LENGTH,
    "CDC 配置描述符长度必须与 wTotalLength 一致");

static uint64_t absolute_u64(int64_t value)
{
    return (uint64_t)(value < 0 ? -value : value);
}

static int16_t read_i16_le(const uint8_t *value)
{
    return (int16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8));
}

static void write_i16_le(uint8_t *output, int16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)((uint16_t)value >> 8);
}

static int16_t clamp_i16(int64_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static int8_t clamp_i8(int64_t value)
{
    if (value > INT8_MAX) {
        return INT8_MAX;
    }
    if (value < INT8_MIN) {
        return INT8_MIN;
    }
    return (int8_t)value;
}

static bool report_has_motion(int16_t x, int16_t y, int8_t wheel, int8_t pan)
{
    return x != 0 || y != 0 || wheel != 0 || pan != 0;
}

static size_t completion_latency_bucket(uint64_t latency_us)
{
    static const uint64_t upper_bounds_us[] = {1000, 2000, 4000, 8000, 16000, 32000};
    for (size_t index = 0; index < sizeof(upper_bounds_us) / sizeof(upper_bounds_us[0]); index++) {
        if (latency_us < upper_bounds_us[index]) {
            return index;
        }
    }
    return COMPLETION_LATENCY_BUCKET_COUNT - 1;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t *buffer,
    uint16_t requested_length)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)requested_length;
    return 0;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t const *buffer,
    uint16_t buffer_size)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)buffer_size;
}

static void usb_event_callback(tinyusb_event_t *event, void *argument)
{
    (void)argument;
    if (event == NULL) {
        return;
    }
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        if (s_usb_profile == USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD) {
            ESP_LOGI(TAG, "原生 USB 已连接，等待键盘触摸板 HID 端点可发送");
        } else {
            ESP_LOGI(TAG, "原生 USB 已连接，CDC 输入等待主机打开串口");
        }
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        if (s_usb_profile == USB_DEVICE_PROFILE_CDC) {
            usb_cdc_input_on_detached();
            ESP_LOGI(TAG, "原生 USB CDC 已断开");
        } else {
            ESP_LOGI(TAG, "原生 USB HID 已断开，等待发送任务确认输出失活");
        }
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t length)
{
    (void)instance;
    int64_t completion_time_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_mouse_lock);
    if (length >= MOUSE_REPORT_LENGTH + 1 && report[0] == REPORT_ID_MOUSE) {
        int16_t x = read_i16_le(&report[2]);
        int16_t y = read_i16_le(&report[4]);
        int8_t wheel = (int8_t)report[6];
        int8_t pan = (int8_t)report[7];
        s_mouse.completed_reports++;
        s_mouse.completed_x += x;
        s_mouse.completed_y += y;
        if (report_has_motion(x, y, wheel, pan)) {
            s_diagnostics.motion_completed++;
            if (s_last_motion_completion_time_us != 0) {
                uint64_t gap_us = (uint64_t)(completion_time_us - s_last_motion_completion_time_us);
                if (gap_us > s_diagnostics.completion_gap_max_us) {
                    s_diagnostics.completion_gap_max_us = gap_us;
                }
            }
            s_last_motion_completion_time_us = completion_time_us;
        }
        if (s_mouse_submit_time_us != 0) {
            uint64_t latency_us = (uint64_t)(completion_time_us - s_mouse_submit_time_us);
            s_diagnostics.completion_latency_samples++;
            s_diagnostics.completion_latency_total_us += latency_us;
            if (latency_us > s_diagnostics.completion_latency_max_us) {
                s_diagnostics.completion_latency_max_us = latency_us;
            }
            s_diagnostics.completion_latency_buckets[completion_latency_bucket(latency_us)]++;
            s_mouse_submit_time_us = 0;
        }
    }
    portEXIT_CRITICAL(&s_mouse_lock);
    if (s_hid_sender_task != NULL) {
        xTaskNotifyGive(s_hid_sender_task);
    }
}

static bool wait_until_hid_ready(TickType_t timeout)
{
    const TickType_t start = xTaskGetTickCount();
    while ((!tud_mounted() || !tud_hid_ready()) &&
           xTaskGetTickCount() - start < timeout) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
    return tud_mounted() && tud_hid_ready();
}

static bool send_release_all(void)
{
    static const uint8_t keyboard[8] = {0};
    static const uint8_t mouse[MOUSE_REPORT_LENGTH] = {0};

    if (!wait_until_hid_ready(pdMS_TO_TICKS(100)) ||
        !tud_hid_report(REPORT_ID_KEYBOARD, keyboard, sizeof(keyboard))) {
        return false;
    }
    if (!wait_until_hid_ready(pdMS_TO_TICKS(100))) {
        return false;
    }
    int64_t submit_time_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_mouse_lock);
    s_mouse_submit_time_us = submit_time_us;
    portEXIT_CRITICAL(&s_mouse_lock);
    if (!tud_hid_report(REPORT_ID_MOUSE, mouse, sizeof(mouse))) {
        portENTER_CRITICAL(&s_mouse_lock);
        if (s_mouse_submit_time_us == submit_time_us) {
            s_mouse_submit_time_us = 0;
        }
        s_diagnostics.submit_failed++;
        portEXIT_CRITICAL(&s_mouse_lock);
        return false;
    }
    portENTER_CRITICAL(&s_mouse_lock);
    s_mouse.submitted_reports++;
    portEXIT_CRITICAL(&s_mouse_lock);
    return true;
}

static bool submit_mouse_report(uint8_t buttons, bool force)
{
    int16_t x;
    int16_t y;
    int8_t wheel;
    int8_t pan;
    portENTER_CRITICAL(&s_mouse_lock);
    if (s_mouse.release_pending) {
        portEXIT_CRITICAL(&s_mouse_lock);
        return false;
    }
    x = clamp_i16(s_mouse.pending_x);
    y = clamp_i16(s_mouse.pending_y);
    wheel = clamp_i8(s_mouse.pending_wheel);
    pan = clamp_i8(s_mouse.pending_pan);
    portEXIT_CRITICAL(&s_mouse_lock);

    if (!force && x == 0 && y == 0 && wheel == 0 && pan == 0) {
        return false;
    }

    uint8_t report[MOUSE_REPORT_LENGTH] = {buttons};
    write_i16_le(&report[1], x);
    write_i16_le(&report[3], y);
    report[5] = (uint8_t)wheel;
    report[6] = (uint8_t)pan;
    int64_t submit_time_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_mouse_lock);
    s_mouse_submit_time_us = submit_time_us;
    portEXIT_CRITICAL(&s_mouse_lock);
    if (!tud_hid_report(REPORT_ID_MOUSE, report, sizeof(report))) {
        portENTER_CRITICAL(&s_mouse_lock);
        if (s_mouse_submit_time_us == submit_time_us) {
            s_mouse_submit_time_us = 0;
        }
        s_diagnostics.submit_failed++;
        portEXIT_CRITICAL(&s_mouse_lock);
        return false;
    }

    portENTER_CRITICAL(&s_mouse_lock);
    s_mouse.pending_x -= x;
    s_mouse.pending_y -= y;
    s_mouse.pending_wheel -= wheel;
    s_mouse.pending_pan -= pan;
    s_mouse.submitted_x += x;
    s_mouse.submitted_y += y;
    s_mouse.submitted_reports++;
    s_mouse.submitted_buttons = buttons;
    if (report_has_motion(x, y, wheel, pan)) {
        s_diagnostics.motion_submitted++;
    }
    if (s_mouse.pending_x == 0 && s_mouse.pending_y == 0 &&
        s_mouse.pending_wheel == 0 && s_mouse.pending_pan == 0) {
        s_mouse.pending_motion_reports = 0;
    }
    portEXIT_CRITICAL(&s_mouse_lock);
    return true;
}

static void update_queue_high_water(void)
{
    UBaseType_t waiting = uxQueueMessagesWaiting(s_control_queue);
    if (waiting > s_control_queue_high_water) {
        s_control_queue_high_water = waiting;
    }
}

static esp_err_t enqueue_control_event(
    const hid_control_event_t *event,
    TickType_t wait,
    bool replace_pending)
{
    if (xSemaphoreTake(s_control_mutex, wait) != pdTRUE) {
        portENTER_CRITICAL(&s_mouse_lock);
        s_mouse.control_drops++;
        portEXIT_CRITICAL(&s_mouse_lock);
        return ESP_ERR_TIMEOUT;
    }
    if (replace_pending) {
        xQueueReset(s_control_queue);
    }
    BaseType_t sent = xQueueSend(s_control_queue, event, wait);
    if (sent == pdTRUE) {
        update_queue_high_water();
    }
    xSemaphoreGive(s_control_mutex);
    if (sent != pdTRUE) {
        portENTER_CRITICAL(&s_mouse_lock);
        s_mouse.control_drops++;
        portEXIT_CRITICAL(&s_mouse_lock);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void log_statistics(void)
{
    mouse_state_t snapshot;
    diagnostic_state_t diagnostics;
    portENTER_CRITICAL(&s_mouse_lock);
    snapshot = s_mouse;
    diagnostics = s_diagnostics;
    memset(&s_diagnostics, 0, sizeof(s_diagnostics));
    portEXIT_CRITICAL(&s_mouse_lock);
    uint64_t latency_average_us = diagnostics.completion_latency_samples == 0
        ? 0
        : diagnostics.completion_latency_total_us / diagnostics.completion_latency_samples;
    ESP_LOGI(
        TAG,
        "鼠标统计（500 Hz）：接收=%" PRIu64 " 位移=(%" PRId64 ",%" PRId64 ")，"
        "提交=%" PRIu64 " 位移=(%" PRId64 ",%" PRId64 ")，"
        "完成=%" PRIu64 " 位移=(%" PRId64 ",%" PRId64 ")，"
        "待发送=(%" PRId64 ",%" PRId64 ")，会话丢弃=(%" PRId64 ",%" PRId64 ")，"
        "最大积压=(%" PRIu64 ",%" PRIu64 ")/%" PRIu64 "帧，控制队列当前=%u，峰值=%u，控制丢弃=%" PRIu64,
        snapshot.received_reports,
        snapshot.received_x,
        snapshot.received_y,
        snapshot.submitted_reports,
        snapshot.submitted_x,
        snapshot.submitted_y,
        snapshot.completed_reports,
        snapshot.completed_x,
        snapshot.completed_y,
        snapshot.pending_x,
        snapshot.pending_y,
        snapshot.discarded_x,
        snapshot.discarded_y,
        snapshot.max_pending_x,
        snapshot.max_pending_y,
        snapshot.max_pending_motion_reports,
        (unsigned)uxQueueMessagesWaiting(s_control_queue),
        (unsigned)s_control_queue_high_water,
        snapshot.control_drops);
    ESP_LOGI(
        TAG,
        "USB诊断（最近1秒）：任务tick=%" PRIu64 "，mounted=%" PRIu64 "/%" PRIu64 "，"
        "ready=%" PRIu64 "/%" PRIu64 "，有效移动 接收/提交/完成=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "，"
        "提交失败=%" PRIu64 "，完成延迟 平均/最大=%" PRIu64 "/%" PRIu64 " us，完成最大间隔=%" PRIu64 " us，"
        "延迟分桶[<1/<2/<4/<8/<16/<32/>=32ms]=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64,
        diagnostics.sender_ticks,
        diagnostics.mounted_true,
        diagnostics.mounted_false,
        diagnostics.ready_true,
        diagnostics.ready_false,
        diagnostics.motion_received,
        diagnostics.motion_submitted,
        diagnostics.motion_completed,
        diagnostics.submit_failed,
        latency_average_us,
        diagnostics.completion_latency_max_us,
        diagnostics.completion_gap_max_us,
        diagnostics.completion_latency_buckets[0],
        diagnostics.completion_latency_buckets[1],
        diagnostics.completion_latency_buckets[2],
        diagnostics.completion_latency_buckets[3],
        diagnostics.completion_latency_buckets[4],
        diagnostics.completion_latency_buckets[5],
        diagnostics.completion_latency_buckets[6]);
}

static void hid_sender_task(void *argument)
{
    (void)argument;
    s_hid_sender_task = xTaskGetCurrentTaskHandle();
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_statistics = last_wake;

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MOUSE_SEND_PERIOD_MS));
        bool mounted = tud_mounted();
        bool ready = mounted && tud_hid_ready();
        usb_output_liveness_event_t liveness_event = usb_output_liveness_update(
            &s_usb_output_liveness,
            ready,
            MOUSE_SEND_PERIOD_MS,
            USB_UNAVAILABLE_TIMEOUT_MS);
        if (liveness_event == USB_OUTPUT_LIVENESS_BECAME_AVAILABLE) {
            ESP_LOGI(TAG, "USB HID 端点可发送，标记 USB 输出在线");
            output_router_set_connected(OUTPUT_MODE_USB, true);
        } else if (liveness_event == USB_OUTPUT_LIVENESS_BECAME_UNAVAILABLE) {
            ESP_LOGW(
                TAG,
                "USB HID 连续 %d ms 不可发送（mounted=%d ready=%d），标记 USB 输出离线",
                USB_UNAVAILABLE_TIMEOUT_MS,
                mounted,
                ready);
            output_router_set_connected(OUTPUT_MODE_USB, false);
        }
        portENTER_CRITICAL(&s_mouse_lock);
        s_diagnostics.sender_ticks++;
        if (mounted) {
            s_diagnostics.mounted_true++;
        } else {
            s_diagnostics.mounted_false++;
        }
        if (ready) {
            s_diagnostics.ready_true++;
        } else {
            s_diagnostics.ready_false++;
        }
        portEXIT_CRITICAL(&s_mouse_lock);
        if (xTaskGetTickCount() - last_statistics >= pdMS_TO_TICKS(STATISTICS_PERIOD_MS)) {
            log_statistics();
            last_statistics = xTaskGetTickCount();
        }
        if (!ready) {
            continue;
        }

        hid_control_event_t event;
        xSemaphoreTake(s_control_mutex, portMAX_DELAY);
        if (xQueuePeek(s_control_queue, &event, 0) == pdTRUE) {
            bool sent = false;
            if (event.type == HID_CONTROL_KEYBOARD && event.length == 8) {
                sent = tud_hid_report(REPORT_ID_KEYBOARD, event.payload, event.length);
            } else if (event.type == HID_CONTROL_MOUSE_BUTTONS) {
                sent = submit_mouse_report(event.payload[0], true);
            } else if (event.type == HID_CONTROL_RELEASE_ALL) {
                sent = send_release_all();
                if (sent) {
                    portENTER_CRITICAL(&s_mouse_lock);
                    s_mouse.submitted_buttons = 0;
                    s_mouse.release_pending = false;
                    portEXIT_CRITICAL(&s_mouse_lock);
                }
            }
            if (sent) {
                xQueueReceive(s_control_queue, &event, 0);
            }
            xSemaphoreGive(s_control_mutex);
            continue;
        }
        xSemaphoreGive(s_control_mutex);

        uint8_t buttons;
        portENTER_CRITICAL(&s_mouse_lock);
        buttons = s_mouse.submitted_buttons;
        portEXIT_CRITICAL(&s_mouse_lock);
        submit_mouse_report(buttons, false);
    }
}

esp_err_t hid_output_init(usb_device_profile_t profile)
{
    s_usb_profile = profile;

    if (profile == USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD) {
        s_control_queue = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(hid_control_event_t));
        if (s_control_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_control_mutex = xSemaphoreCreateMutex();
        if (s_control_mutex == NULL) {
            vQueueDelete(s_control_queue);
            s_control_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG(usb_event_callback);
    if (profile == USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD) {
        usb_config.descriptor.device = &s_hid_device_descriptor;
        usb_config.descriptor.string = s_hid_string_descriptors;
        usb_config.descriptor.string_count =
            sizeof(s_hid_string_descriptors) / sizeof(s_hid_string_descriptors[0]);
        usb_config.descriptor.full_speed_config = s_hid_configuration_descriptor;
    } else {
        usb_config.descriptor.device = &s_cdc_device_descriptor;
        usb_config.descriptor.string = s_cdc_string_descriptors;
        usb_config.descriptor.string_count =
            sizeof(s_cdc_string_descriptors) / sizeof(s_cdc_string_descriptors[0]);
        usb_config.descriptor.full_speed_config = s_cdc_configuration_descriptor;
    }

    esp_err_t error = tinyusb_driver_install(&usb_config);
    if (error != ESP_OK) {
        if (s_control_queue != NULL) {
            vQueueDelete(s_control_queue);
            s_control_queue = NULL;
        }
        if (s_control_mutex != NULL) {
            vSemaphoreDelete(s_control_mutex);
            s_control_mutex = NULL;
        }
        return error;
    }

    if (profile == USB_DEVICE_PROFILE_CDC) {
        error = usb_cdc_input_init();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "USB CDC 输入初始化失败：%s", esp_err_to_name(error));
            tinyusb_driver_uninstall();
            return error;
        }
        ESP_LOGI(TAG, "原生 USB 配置：CDC-only，VID:PID=303A:4001");
        return ESP_OK;
    }

    if (xTaskCreate(
            hid_sender_task,
            "hid_sender",
            4096,
            NULL,
            8,
            NULL) != pdPASS) {
        ESP_LOGE(TAG, "无法创建 HID 发送任务");
        tinyusb_driver_uninstall();
        vQueueDelete(s_control_queue);
        s_control_queue = NULL;
        vSemaphoreDelete(s_control_mutex);
        s_control_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "原生 USB 配置：键盘 + 相对触摸板 HID-only，VID:PID=303A:4004");
    return ESP_OK;
}

esp_err_t hid_output_submit(const bridge_frame_t *frame)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_usb_profile != USB_DEVICE_PROFILE_KEYBOARD_TOUCHPAD) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_control_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (frame->type == BRIDGE_MESSAGE_MOUSE_REPORT) {
        if (frame->payload_length != MOUSE_REPORT_LENGTH) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint8_t buttons = frame->payload[0];
        const int16_t x = read_i16_le(&frame->payload[1]);
        const int16_t y = read_i16_le(&frame->payload[3]);
        const int8_t wheel = (int8_t)frame->payload[5];
        const int8_t pan = (int8_t)frame->payload[6];

        uint8_t previous_buttons;
        portENTER_CRITICAL(&s_mouse_lock);
        previous_buttons = s_mouse.received_buttons;
        portEXIT_CRITICAL(&s_mouse_lock);
        if (buttons != previous_buttons) {
            hid_control_event_t event = {
                .type = HID_CONTROL_MOUSE_BUTTONS,
                .length = 1,
                .payload = {buttons},
            };
            esp_err_t queued = enqueue_control_event(
                &event,
                portMAX_DELAY,
                false);
            if (queued != ESP_OK) {
                return queued;
            }
        }

        portENTER_CRITICAL(&s_mouse_lock);
        s_mouse.received_buttons = buttons;
        s_mouse.pending_x += x;
        s_mouse.pending_y += y;
        s_mouse.pending_wheel += wheel;
        s_mouse.pending_pan += pan;
        s_mouse.received_x += x;
        s_mouse.received_y += y;
        s_mouse.received_reports++;
        if (report_has_motion(x, y, wheel, pan)) {
            s_diagnostics.motion_received++;
            s_mouse.pending_motion_reports++;
            if (s_mouse.pending_motion_reports > s_mouse.max_pending_motion_reports) {
                s_mouse.max_pending_motion_reports = s_mouse.pending_motion_reports;
            }
        }
        uint64_t pending_x = absolute_u64(s_mouse.pending_x);
        uint64_t pending_y = absolute_u64(s_mouse.pending_y);
        if (pending_x > s_mouse.max_pending_x) {
            s_mouse.max_pending_x = pending_x;
        }
        if (pending_y > s_mouse.max_pending_y) {
            s_mouse.max_pending_y = pending_y;
        }
        portEXIT_CRITICAL(&s_mouse_lock);
        return ESP_OK;
    }

    hid_control_event_t event = {0};
    if (frame->type == BRIDGE_MESSAGE_KEYBOARD_REPORT) {
        if (frame->payload_length != 8) {
            return ESP_ERR_INVALID_SIZE;
        }
        event.type = HID_CONTROL_KEYBOARD;
        event.length = 8;
        memcpy(event.payload, frame->payload, 8);
    } else if (frame->type == BRIDGE_MESSAGE_RELEASE_ALL) {
        event.type = HID_CONTROL_RELEASE_ALL;
        portENTER_CRITICAL(&s_mouse_lock);
        s_mouse.discarded_x += s_mouse.pending_x;
        s_mouse.discarded_y += s_mouse.pending_y;
        s_mouse.pending_x = 0;
        s_mouse.pending_y = 0;
        s_mouse.pending_wheel = 0;
        s_mouse.pending_pan = 0;
        s_mouse.received_buttons = 0;
        s_mouse.release_pending = true;
        s_last_motion_completion_time_us = 0;
        portEXIT_CRITICAL(&s_mouse_lock);
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return enqueue_control_event(
        &event,
        portMAX_DELAY,
        frame->type == BRIDGE_MESSAGE_RELEASE_ALL);
}
