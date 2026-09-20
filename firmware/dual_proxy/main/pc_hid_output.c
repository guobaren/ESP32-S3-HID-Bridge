#include "pc_hid_output.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#include "dual_input_aggregator.h"
#include "dual_proxy_runtime_config.h"
#include "dual_status_led.h"
#include "usb_cdc_control.h"

#define REPORT_ID_MOUSE 1
#define MOUSE_REPORT_LENGTH 7
#define USB_HID_INTERFACE 0
#define USB_HID_ENDPOINT 0x81
#define USB_CDC_INTERFACE 1
#define USB_INTERFACE_COUNT 3
#define USB_CDC_NOTIFICATION_ENDPOINT 0x82
#define USB_CDC_DATA_OUT_ENDPOINT 0x03
#define USB_CDC_DATA_IN_ENDPOINT 0x83
#define USB_CONFIG_TOTAL_LENGTH (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)
#define USB_PID_HID_CDC 0x4005

static const char *TAG = "dual_pc_hid";
static SemaphoreHandle_t s_state_mutex;
static TaskHandle_t s_sender_task;
static esp_timer_handle_t s_sender_timer;
static dual_input_state_t s_state;
static volatile bool s_force_release;
static volatile bool s_installed;
static volatile uint32_t s_hid_timer_ticks;
static volatile uint32_t s_hid_not_mounted;
static volatile uint32_t s_hid_not_ready;
static volatile uint32_t s_hid_attempts;
static volatile uint32_t s_hid_submitted;
static volatile uint32_t s_hid_submit_failures;
static volatile uint32_t s_hid_completions;
static volatile uint32_t s_hid_transfer_failures;
static volatile uint32_t s_physical_received;
static bool s_software_flash_pending;
static int64_t s_software_input_x;
static int64_t s_software_input_y;
static int64_t s_software_input_wheel;
static int64_t s_software_input_pan;
static int64_t s_physical_input_x;
static int64_t s_physical_input_y;
static int64_t s_physical_input_wheel;
static int64_t s_physical_input_pan;
static int64_t s_output_x;
static int64_t s_output_y;
static int64_t s_output_wheel;
static int64_t s_output_pan;
static int64_t s_last_stats_us;

_Static_assert(CONFIG_FREERTOS_HZ == DUAL_PROXY_REQUIRED_FREERTOS_HZ,
               "dual_proxy要求CONFIG_FREERTOS_HZ=1000");
_Static_assert(pdMS_TO_TICKS(1) == 1, "1ms必须正好折算为1 tick");
_Static_assert(DUAL_PROXY_HID_PERIOD_US == 1000U, "HID周期必须保持1000us");

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID,
    .idProduct = USB_PID_HID_CDC,
    .bcdDevice = 0x0200,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const char s_language_en_us[] = {0x09, 0x04};
static const char *s_string_descriptors[] = {
    s_language_en_us,
    "HID Bridge",
    "Dual Proxy HID+CDC",
    "HIDBRIDGE-DUAL",
    "Dual Proxy Mouse",
    "Dual Proxy Control CDC",
};

static const uint8_t s_report_descriptor[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x02,       /* Usage (Mouse) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, REPORT_ID_MOUSE,
    0x09, 0x01,       /* Usage (Pointer) */
    0xA1, 0x00,       /* Collection (Physical) */
    0x05, 0x09,       /* Usage Page (Button) */
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
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x30,       /* X */
    0x09, 0x31,       /* Y */
    0x16, 0x00, 0x80, /* Logical Minimum (-32768) */
    0x26, 0xFF, 0x7F, /* Logical Maximum (32767) */
    0x75, 0x10,
    0x95, 0x02,
    0x81, 0x06,
    0x09, 0x38,       /* Wheel */
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x06,
    0x05, 0x0C,       /* Consumer */
    0x0A, 0x38, 0x02, /* AC Pan */
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x06,
    0xC0,
    0xC0,
};

static const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        USB_INTERFACE_COUNT,
        0,
        USB_CONFIG_TOTAL_LENGTH,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    TUD_HID_DESCRIPTOR(
        USB_HID_INTERFACE,
        4,
        HID_ITF_PROTOCOL_MOUSE,
        sizeof(s_report_descriptor),
        USB_HID_ENDPOINT,
        16,
        1),
    TUD_CDC_DESCRIPTOR(
        USB_CDC_INTERFACE,
        5,
        USB_CDC_NOTIFICATION_ENDPOINT,
        8,
        USB_CDC_DATA_OUT_ENDPOINT,
        USB_CDC_DATA_IN_ENDPOINT,
        64),
};

_Static_assert(sizeof(s_configuration_descriptor) == USB_CONFIG_TOTAL_LENGTH,
               "HID+CDC配置描述符长度错误");

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_report_descriptor;
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
        ESP_LOGI(TAG, "PC侧USB HID已连接");
        dual_status_led_set_pc_mounted(true);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGW(TAG, "PC侧USB HID已断开，清理所有输入");
        dual_status_led_set_pc_mounted(false);
        dual_usb_cdc_control_on_detached();
        dual_pc_hid_release_all();
    }
}

static void sender_timer_callback(void *argument)
{
    (void)argument;
    ++s_hid_timer_ticks;
    if (s_sender_task != NULL) {
        xTaskNotifyGive(s_sender_task);
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)report;
    (void)len;
    if (instance != 0) {
        return;
    }
    ++s_hid_completions;
    if (s_sender_task != NULL) {
        xTaskNotifyGive(s_sender_task);
    }
}

void tud_hid_report_failed_cb(
    uint8_t instance,
    hid_report_type_t report_type,
    uint8_t const *report,
    uint16_t xferred_bytes)
{
    (void)report_type;
    (void)report;
    (void)xferred_bytes;
    if (instance != 0) {
        return;
    }
    ++s_hid_transfer_failures;
    if (s_sender_task != NULL) {
        xTaskNotifyGive(s_sender_task);
    }
}

static int64_t add_stat_axis(int64_t first, int64_t second)
{
    return first + second;
}

static void log_hid_statistics_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (s_last_stats_us == 0 || now_us - s_last_stats_us >= 5000000LL) {
        int64_t pending_x = 0;
        int64_t pending_y = 0;
        int64_t pending_wheel = 0;
        int64_t pending_pan = 0;
        if (s_state_mutex != NULL) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            pending_x = add_stat_axis(s_state.physical_x, s_state.software_x);
            pending_y = add_stat_axis(s_state.physical_y, s_state.software_y);
            pending_wheel = add_stat_axis(s_state.physical_wheel, s_state.software_wheel);
            pending_pan = add_stat_axis(s_state.physical_pan, s_state.software_pan);
            xSemaphoreGive(s_state_mutex);
        }
        const int64_t input_x = add_stat_axis(s_physical_input_x, s_software_input_x);
        const int64_t input_y = add_stat_axis(s_physical_input_y, s_software_input_y);
        const int64_t input_wheel = add_stat_axis(s_physical_input_wheel, s_software_input_wheel);
        const int64_t input_pan = add_stat_axis(s_physical_input_pan, s_software_input_pan);
        ESP_LOGI(TAG,
                 "HID统计：timer=%" PRIu32 " not_mounted=%" PRIu32 " not_ready=%" PRIu32
                 " attempt=%" PRIu32 " submitted=%" PRIu32 " failed=%" PRIu32
                 " complete=%" PRIu32 " transfer_fail=%" PRIu32 " physical_rx=%" PRIu32
                 " input_phys=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")"
                 " input_soft=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")"
                 " output=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")"
                 " pending=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")"
                 " balance=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")",
                 s_hid_timer_ticks, s_hid_not_mounted, s_hid_not_ready,
                 s_hid_attempts, s_hid_submitted, s_hid_submit_failures,
                 s_hid_completions, s_hid_transfer_failures, s_physical_received,
                 s_physical_input_x, s_physical_input_y, s_physical_input_wheel, s_physical_input_pan,
                 s_software_input_x, s_software_input_y, s_software_input_wheel, s_software_input_pan,
                 s_output_x, s_output_y, s_output_wheel, s_output_pan,
                 pending_x, pending_y, pending_wheel, pending_pan,
                 input_x - s_output_x - pending_x,
                 input_y - s_output_y - pending_y,
                 input_wheel - s_output_wheel - pending_wheel,
                 input_pan - s_output_pan - pending_pan);
        s_last_stats_us = now_us;
    }
}

static void sender_task(void *argument)
{
    (void)argument;
    s_sender_task = xTaskGetCurrentTaskHandle();
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        log_hid_statistics_if_due();
        if (!tud_mounted()) {
            ++s_hid_not_mounted;
            continue;
        }
        if (!tud_hid_ready()) {
            /* Endpoint busy/not-ready is transient; preserve pending motion. */
            ++s_hid_not_ready;
            continue;
        }
        dual_mouse_report_t report;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        const bool force = s_force_release;
        const bool software_flash_pending = s_software_flash_pending;
        const bool available = dual_input_peek_report(&s_state, &report, force);
        if (!available) {
            xSemaphoreGive(s_state_mutex);
            continue;
        }
        uint8_t payload[MOUSE_REPORT_LENGTH] = {
            report.buttons,
            (uint8_t)report.x,
            (uint8_t)((uint16_t)report.x >> 8),
            (uint8_t)report.y,
            (uint8_t)((uint16_t)report.y >> 8),
            (uint8_t)report.wheel,
            (uint8_t)report.pan,
        };
        ++s_hid_attempts;
        if (!tud_hid_report(REPORT_ID_MOUSE, payload, sizeof(payload))) {
            ++s_hid_submit_failures;
            xSemaphoreGive(s_state_mutex);
            ESP_LOGW(TAG, "USB HID报告提交失败，保留积累输入重试");
        } else {
            dual_input_commit_report(&s_state, &report);
            if (force) {
                s_force_release = false;
            }
            if (software_flash_pending) {
                s_software_flash_pending = false;
            }
            ++s_hid_submitted;
            s_output_x += report.x;
            s_output_y += report.y;
            s_output_wheel += report.wheel;
            s_output_pan += report.pan;
            xSemaphoreGive(s_state_mutex);
            if (software_flash_pending) {
                dual_status_led_notify_software_success(
                    (uint32_t)(esp_timer_get_time() / 1000LL));
            }
        }
    }
}

esp_err_t dual_pc_hid_install_device(void)
{
    if (s_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    dual_input_init(&s_state);
    s_force_release = false;
    s_hid_timer_ticks = 0;
    s_hid_not_mounted = 0;
    s_hid_not_ready = 0;
    s_hid_attempts = 0;
    s_hid_completions = 0;
    s_hid_transfer_failures = 0;
    s_physical_received = 0;
    s_software_flash_pending = false;
    s_hid_submitted = 0;
    s_hid_submit_failures = 0;
    s_software_input_x = 0;
    s_software_input_y = 0;
    s_software_input_wheel = 0;
    s_software_input_pan = 0;
    s_physical_input_x = 0;
    s_physical_input_y = 0;
    s_physical_input_wheel = 0;
    s_physical_input_pan = 0;
    s_output_x = 0;
    s_output_y = 0;
    s_output_wheel = 0;
    s_output_pan = 0;
    s_last_stats_us = 0;
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event_callback);
    config.descriptor.device = &s_device_descriptor;
    config.descriptor.string = s_string_descriptors;
    config.descriptor.string_count = sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0]);
    config.descriptor.full_speed_config = s_configuration_descriptor;
    const esp_err_t result = tinyusb_driver_install(&config);
    if (result != ESP_OK) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return result;
    }
    s_installed = true;
    return ESP_OK;
}

esp_err_t dual_pc_hid_start_sender(void)
{
    if (!s_installed || s_sender_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(sender_task, "dual_hid_sender", 4096, NULL, 8, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = sender_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "dual_hid_tick",
        .skip_unhandled_events = false,
    };
    esp_err_t result = esp_timer_create(&timer_args, &s_sender_timer);
    if (result != ESP_OK) {
        vTaskDelete(s_sender_task);
        s_sender_task = NULL;
        return result;
    }
    result = esp_timer_start_periodic(s_sender_timer, DUAL_PROXY_HID_PERIOD_US);
    if (result != ESP_OK) {
        (void)esp_timer_delete(s_sender_timer);
        s_sender_timer = NULL;
        vTaskDelete(s_sender_task);
        s_sender_task = NULL;
        return result;
    }
    return ESP_OK;
}

void dual_pc_hid_software_report(uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan)
{
    if (s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    dual_input_software_report(&s_state, buttons, x, y, wheel, pan);
    s_software_flash_pending = true;
    s_software_input_x += x;
    s_software_input_y += y;
    s_software_input_wheel += wheel;
    s_software_input_pan += pan;
    xSemaphoreGive(s_state_mutex);
}

void dual_pc_hid_software_release(void)
{
    if (s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    dual_input_software_release(&s_state);
    s_software_flash_pending = false;
    xSemaphoreGive(s_state_mutex);
}

void dual_pc_hid_physical_report(uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan)
{
    if (s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    dual_input_physical_report(&s_state, buttons, x, y, wheel, pan);
    ++s_physical_received;
    s_physical_input_x += x;
    s_physical_input_y += y;
    s_physical_input_wheel += wheel;
    s_physical_input_pan += pan;
    xSemaphoreGive(s_state_mutex);
}

void dual_pc_hid_physical_release(void)
{
    if (s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    dual_input_physical_release(&s_state);
    xSemaphoreGive(s_state_mutex);
}

void dual_pc_hid_release_all(void)
{
    if (s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    dual_input_release_all(&s_state);
    s_software_flash_pending = false;
    s_force_release = true;
    xSemaphoreGive(s_state_mutex);
}

bool dual_pc_hid_ready(void)
{
    return s_installed && tud_mounted() && tud_hid_ready();
}
