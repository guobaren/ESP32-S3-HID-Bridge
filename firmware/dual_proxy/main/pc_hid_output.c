#include "pc_hid_output.h"

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

#include "dual_input_aggregator.h"
#include "dual_proxy_runtime_config.h"
#include "dual_status_led.h"
#include "hid_clone_descriptor.h"
#include "hid_device_profile.h"
#include "hid_report_layout.h"
#include "mouse_motion_smoother.h"
#include "uart1_link.h"
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
#define CLONE_STRING_COUNT 16U
#define CLONE_STRING_CAPACITY (HID_PROFILE_MAX_STRING_BYTES + 1U)
#define RECONFIGURE_TASK_STACK 8192
#define RECONFIGURE_TASK_PRIORITY 4
#define VENDOR_INPUT_QUEUE_LENGTH 128
#define VENDOR_CONTROL_QUEUE_LENGTH 8
#define VENDOR_CLONE_READY_TIMEOUT_MS 5000U
#define VENDOR_INPUT_TASK_STACK 3072
#define VENDOR_CONTROL_TASK_STACK 3072
#define VENDOR_GET_REPORT_TIMEOUT_MS 20

static const char *TAG = "dual_pc_hid";
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_sender_stopped;
static SemaphoreHandle_t s_reconfigure_mutex;
static TaskHandle_t s_sender_task;
static TaskHandle_t s_reconfigure_task;
static esp_timer_handle_t s_sender_timer;
static volatile bool s_sender_stop_requested;
static volatile bool s_reconfigure_enabled;
static volatile bool s_reconfigure_disconnect_requested;
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
static uint32_t s_last_stats_timer_ticks;
static uint32_t s_last_stats_physical_received;
static uint32_t s_last_stats_submitted;
static uint32_t s_last_stats_completions;
static hid_device_profile_t s_reconfigure_profile;
static hid_device_profile_t s_reconfigure_work_profile;
static hid_device_profile_t s_active_profile;
static hid_clone_descriptor_set_t s_clone_descriptors;
static tusb_desc_device_t s_clone_device_descriptor;
static volatile bool s_clone_active;
static uint8_t s_clone_mouse_instance;
static hid_mouse_report_layout_t s_clone_mouse_layout;
static uint8_t s_clone_mouse_template[DUAL_HID_RAW_INPUT_MAX_DATA];
static uint8_t s_clone_mouse_template_length;
static bool s_clone_mouse_template_valid;
static char s_clone_manufacturer[CLONE_STRING_CAPACITY];
static char s_clone_product[CLONE_STRING_CAPACITY];
static char s_clone_serial[CLONE_STRING_CAPACITY];
static char s_clone_language[] = {0x09, 0x04};
static const char *s_clone_string_descriptors[CLONE_STRING_COUNT];
static uint8_t s_clone_string_count;
static const char s_empty_string[] = "";
static volatile uint32_t s_clone_input_suppressed;
static mouse_motion_smoother_t s_software_smoother;
static uint8_t s_software_buttons;

typedef struct {
    uint8_t interface_number;
    uint8_t report_id;
    uint8_t length;
    uint8_t data[DUAL_HID_RAW_INPUT_MAX_DATA];
} vendor_input_item_t;

typedef struct {
    bool get_report;
    uint16_t transaction_id;
    uint8_t interface_number;
    uint8_t report_id;
    uint8_t report_type;
    uint8_t requested_length;
    uint8_t length;
    uint8_t data[DUAL_HID_CONTROL_MAX_DATA];
} vendor_control_item_t;

static QueueHandle_t s_vendor_input_queue;
static QueueHandle_t s_vendor_control_queue;
static TaskHandle_t s_vendor_input_task;
static TaskHandle_t s_vendor_control_task;
static SemaphoreHandle_t s_get_gate;
static SemaphoreHandle_t s_get_response_sem;
static SemaphoreHandle_t s_get_state_mutex;
static volatile bool s_get_inflight;
static volatile bool s_get_response_ready;
static uint16_t s_get_transaction_id;
static uint8_t s_get_interface_number;
static uint8_t s_get_report_id;
static uint8_t s_get_status;
static uint8_t s_get_response_length;
static uint8_t s_get_response_data[DUAL_HID_CONTROL_MAX_DATA];
static uint16_t s_next_vendor_transaction_id = 1;
static volatile uint32_t s_vendor_input_received;
static volatile uint32_t s_vendor_input_submitted;
static volatile uint32_t s_vendor_input_dropped;
static volatile uint32_t s_vendor_set_queued;
static volatile uint32_t s_vendor_set_dropped;
static volatile uint32_t s_vendor_get_requests;
static volatile uint32_t s_vendor_get_timeouts;
static volatile uint32_t s_vendor_get_mismatches;
static volatile bool s_usb_reconfigure_in_progress;

_Static_assert(CONFIG_FREERTOS_HZ == DUAL_PROXY_REQUIRED_FREERTOS_HZ,
               "dual_proxy要求CONFIG_FREERTOS_HZ=1000");
_Static_assert(pdMS_TO_TICKS(1) == 1, "1ms必须正好折算为1 tick");
_Static_assert(DUAL_PROXY_HID_PERIOD_US == 1000U, "HID周期必须保持1000us");

static void apply_next_scheduled_software_locked(void)
{
    const mouse_motion_delta_t scheduled =
        mouse_motion_smoother_take_next(&s_software_smoother);
    dual_input_software_report(
        &s_state,
        s_software_buttons,
        (int16_t)scheduled.x,
        (int16_t)scheduled.y,
        (int8_t)scheduled.wheel,
        (int8_t)scheduled.pan);
}

static bool clone_copy_ascii(
    char *destination,
    size_t capacity,
    const hid_profile_string_t *source)
{
    if (destination == NULL || capacity == 0 || source == NULL ||
        source->length >= capacity) {
        return false;
    }
    for (uint16_t index = 0; index < source->length; ++index) {
        const uint8_t value = (uint8_t)source->data[index];
        if (value == 0 || value > 0x7FU) {
            return false;
        }
        destination[index] = (char)value;
    }
    destination[source->length] = '\0';
    return true;
}

static bool clone_sanitize_interface_strings(
    uint8_t *configuration,
    size_t length)
{
    if (configuration == NULL || length < 9U) {
        return false;
    }
    size_t offset = 0;
    while (offset < length) {
        if (length - offset < 2U || configuration[offset] < 2U ||
            configuration[offset] > length - offset) {
            return false;
        }
        const uint8_t descriptor_length = configuration[offset];
        if (configuration[offset + 1U] == TUSB_DESC_INTERFACE &&
            descriptor_length >= 9U) {
            /* The profile currently carries only the three device strings.
             * Do not leave an interface string index pointing at an unknown
             * table entry; an empty iInterface is valid and deterministic. */
            configuration[offset + 8U] = 0;
        }
        offset += descriptor_length;
    }
    return offset == length;
}

static bool prepare_clone_descriptor_set(const hid_device_profile_t *profile)
{
    if (profile == NULL ||
        (profile->flags & (HID_PROFILE_FLAG_SYNTHETIC_DEVICE_DESCRIPTOR |
                           HID_PROFILE_FLAG_SYNTHETIC_CONFIG_DESCRIPTOR)) != 0 ||
        profile->device_descriptor.length != sizeof(tusb_desc_device_t) ||
        profile->device_descriptor.data[1] != TUSB_DESC_DEVICE ||
        profile->device_descriptor.data[17] != 1U ||
        profile->report_descriptor_count == 0U ||
        profile->report_descriptor_count > CONFIG_TINYUSB_HID_COUNT) {
        return false;
    }
    hid_clone_descriptor_set_t built;
    if (!hid_clone_descriptor_build_exact(profile, &built) ||
        built.hid_count == 0U || built.hid_count > CONFIG_TINYUSB_HID_COUNT ||
        !clone_sanitize_interface_strings(
            built.configuration_descriptor, built.configuration_length) ||
        !clone_copy_ascii(s_clone_manufacturer, sizeof(s_clone_manufacturer),
                          &profile->manufacturer) ||
        !clone_copy_ascii(s_clone_product, sizeof(s_clone_product),
                          &profile->product) ||
        !clone_copy_ascii(s_clone_serial, sizeof(s_clone_serial),
                          &profile->serial)) {
        return false;
    }

    uint8_t mouse_instance = UINT8_MAX;
    hid_mouse_report_layout_t mouse_layout;
    memset(&mouse_layout, 0, sizeof(mouse_layout));
    for (uint8_t instance = 0; instance < built.hid_count; ++instance) {
        const uint8_t report_index = built.profile_report_indices[instance];
        if (report_index >= profile->report_descriptor_count) {
            return false;
        }
        const hid_profile_report_descriptor_t *report =
            &profile->report_descriptors[report_index];
        hid_mouse_report_layout_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        if (hid_report_find_mouse_layout(
                report->data, report->length, &candidate)) {
            if (mouse_instance != UINT8_MAX) {
                return false;
            }
            if (candidate.report_bytes == 0U ||
                candidate.report_bytes > DUAL_HID_RAW_INPUT_MAX_DATA) {
                return false;
            }
            mouse_instance = instance;
            mouse_layout = candidate;
        }
    }
    if (mouse_instance == UINT8_MAX) {
        return false;
    }

    const uint8_t manufacturer_index = profile->device_descriptor.data[14];
    const uint8_t product_index = profile->device_descriptor.data[15];
    const uint8_t serial_index = profile->device_descriptor.data[16];
    uint8_t maximum_string_index = manufacturer_index;
    if (product_index > maximum_string_index) {
        maximum_string_index = product_index;
    }
    if (serial_index > maximum_string_index) {
        maximum_string_index = serial_index;
    }
    if (maximum_string_index >= CLONE_STRING_COUNT) {
        return false;
    }

    memcpy(&s_clone_descriptors, &built, sizeof(s_clone_descriptors));
    memcpy(&s_clone_device_descriptor, built.device_descriptor,
           sizeof(s_clone_device_descriptor));
    /* 严格克隆不附加 CDC/IAD，必须保留物理设备原始 class tuple。 */
    /*
     * bMaxPacketSize0 是设备控制器的硬件/编译期属性，不能像
     * VID/PID 和 HID 拓扑一样在运行时克隆。例如 C539 接收器声明
     * 32 bytes，而 ESP32-S3 TinyUSB 设备端按 CFG_TUD_ENDPOINT0_SIZE
     * 配置。描述符必须反映实际控制端点，否则主机可能在取配置
     * 描述符前就中止枚举。
     */
    s_clone_device_descriptor.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE;
    s_clone_device_descriptor.bNumConfigurations = 1;
    for (uint8_t index = 0; index < CLONE_STRING_COUNT; ++index) {
        s_clone_string_descriptors[index] = s_empty_string;
    }
    s_clone_string_descriptors[0] = s_clone_language;
    if (manufacturer_index != 0U) {
        s_clone_string_descriptors[manufacturer_index] = s_clone_manufacturer;
    }
    if (product_index != 0U) {
        s_clone_string_descriptors[product_index] = s_clone_product;
    }
    if (serial_index != 0U) {
        s_clone_string_descriptors[serial_index] = s_clone_serial;
    }
    s_clone_string_count = (uint8_t)(maximum_string_index + 1U);
    s_clone_mouse_instance = mouse_instance;
    s_clone_mouse_layout = mouse_layout;
    s_clone_mouse_template_length = (uint8_t)mouse_layout.report_bytes;
    s_clone_mouse_template_valid = false;
    memset(s_clone_mouse_template, 0, sizeof(s_clone_mouse_template));
    return true;
}

static bool clone_instance_for_interface(uint8_t interface_number, uint8_t *instance)
{
    if (!s_clone_active || instance == NULL) {
        return false;
    }
    for (uint8_t index = 0; index < s_clone_descriptors.hid_count; ++index) {
        if (s_clone_descriptors.hid_interface_numbers[index] == interface_number) {
            *instance = index;
            return true;
        }
    }
    return false;
}

static bool clone_interface_for_instance(uint8_t instance, uint8_t *interface_number)
{
    if (!s_clone_active || interface_number == NULL ||
        instance >= s_clone_descriptors.hid_count) {
        return false;
    }
    *interface_number = s_clone_descriptors.hid_interface_numbers[instance];
    return true;
}

static uint16_t next_vendor_transaction_id(void)
{
    uint16_t transaction_id = s_next_vendor_transaction_id++;
    if (transaction_id == 0U) {
        transaction_id = s_next_vendor_transaction_id++;
    }
    return transaction_id;
}

static void clear_pending_get(bool wake_waiter, uint8_t status)
{
    if (s_get_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_get_state_mutex, portMAX_DELAY);
    if (s_get_inflight) {
        s_get_status = status;
        s_get_response_length = 0;
        s_get_response_ready = wake_waiter;
        if (wake_waiter && s_get_response_sem != NULL) {
            xSemaphoreGive(s_get_response_sem);
        }
        s_get_inflight = false;
    }
    xSemaphoreGive(s_get_state_mutex);
}

static void vendor_input_task(void *argument)
{
    (void)argument;
    vendor_input_item_t item;
    while (true) {
        if (xQueueReceive(s_vendor_input_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.length == 0U) {
            ++s_vendor_input_dropped;
            continue;
        }
        uint8_t instance = 0;
        bool clone_ready = false;
        for (uint32_t waited_ms = 0; waited_ms < VENDOR_CLONE_READY_TIMEOUT_MS;
             ++waited_ms) {
            if (s_clone_active && s_installed && tud_mounted() &&
                clone_instance_for_interface(item.interface_number, &instance)) {
                clone_ready = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!clone_ready) {
            ++s_vendor_input_dropped;
            continue;
        }
        if (instance == s_clone_mouse_instance &&
            item.report_id == s_clone_mouse_layout.report_id &&
            item.length == s_clone_mouse_layout.report_bytes &&
            s_state_mutex != NULL) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            memcpy(s_clone_mouse_template, item.data, item.length);
            s_clone_mouse_template_length = item.length;
            s_clone_mouse_template_valid = true;
            xSemaphoreGive(s_state_mutex);
        }
        /*
         * UART 到包时刻与 USB IN 轮询不同步，不能因为端点正好
         * busy 就丢掉实体鼠标/键盘报告。由 complete callback 唤醒后
         * 立即重试，最多等待 4 ms；断线或长时间不 ready 仍有明确丢弃
         * 边界，避免单个故障接口堵死整条链路。
         */
        (void)ulTaskNotifyTake(pdTRUE, 0);
        bool submitted = false;
        for (uint8_t attempt = 0; attempt < 4U; ++attempt) {
            if (!s_clone_active || !s_installed || !tud_mounted()) {
                break;
            }
            if (tud_hid_n_ready(instance) &&
                tud_hid_n_report(instance, item.report_id,
                                 item.data, item.length)) {
                submitted = true;
                break;
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        }
        if (submitted) {
            ++s_vendor_input_submitted;
        } else {
            ++s_vendor_input_dropped;
        }
    }
}

static void vendor_control_task(void *argument)
{
    (void)argument;
    vendor_control_item_t item;
    while (true) {
        if (xQueueReceive(s_vendor_control_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint8_t instance = 0;
        if (!s_clone_active || !s_installed || !tud_mounted() ||
            !clone_instance_for_interface(item.interface_number, &instance)) {
            if (item.get_report) {
                clear_pending_get(true, DUAL_HID_REPORT_STATUS_TIMEOUT);
            }
            ++s_vendor_set_dropped;
            continue;
        }
        if (item.get_report) {
            if (dual_uart1_send_hid_get_request(
                    item.transaction_id, item.interface_number, item.report_id,
                    item.report_type, item.requested_length) != ESP_OK) {
                ++s_vendor_get_timeouts;
                clear_pending_get(true, DUAL_HID_REPORT_STATUS_TIMEOUT);
            }
        } else if (dual_uart1_send_hid_set_report(
                       item.transaction_id, item.interface_number, item.report_id,
                       item.report_type, item.data, item.length) != ESP_OK) {
            ++s_vendor_set_dropped;
        }
    }
}

static esp_err_t ensure_vendor_runtime(void)
{
    if (s_vendor_input_queue != NULL && s_vendor_control_queue != NULL &&
        s_get_gate != NULL && s_get_response_sem != NULL &&
        s_get_state_mutex != NULL && s_vendor_input_task != NULL &&
        s_vendor_control_task != NULL) {
        return ESP_OK;
    }

    s_vendor_input_queue = xQueueCreate(
        VENDOR_INPUT_QUEUE_LENGTH, sizeof(vendor_input_item_t));
    s_vendor_control_queue = xQueueCreate(
        VENDOR_CONTROL_QUEUE_LENGTH, sizeof(vendor_control_item_t));
    s_get_gate = xSemaphoreCreateBinary();
    s_get_response_sem = xSemaphoreCreateBinary();
    s_get_state_mutex = xSemaphoreCreateMutex();
    if (s_vendor_input_queue == NULL || s_vendor_control_queue == NULL ||
        s_get_gate == NULL || s_get_response_sem == NULL ||
        s_get_state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_get_gate);

    if (xTaskCreate(vendor_input_task, "hid_vendor_input",
                    VENDOR_INPUT_TASK_STACK, NULL, 7,
                    &s_vendor_input_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(vendor_control_task, "hid_vendor_control",
                    VENDOR_CONTROL_TASK_STACK, NULL, 6,
                    &s_vendor_control_task) != pdPASS) {
        vTaskDelete(s_vendor_input_task);
        s_vendor_input_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

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
    if (s_clone_active && instance < s_clone_descriptors.hid_count) {
        const uint8_t report_index =
            s_clone_descriptors.profile_report_indices[instance];
        if (report_index < s_active_profile.report_descriptor_count) {
            return s_active_profile.report_descriptors[report_index].data;
        }
        return NULL;
    }
    return s_report_descriptor;
}

/*
 * G HUB 将 C092 标记为 DEVIO。先记录所有落到设备级 vendor request
 * 的 SETUP 包，以确认其是否还依赖 HID++ 之外的 EP0 控制事务。
 * 尚未实现跨板转发时返回 false，让 TinyUSB 明确 STALL，禁止伪造成功。
 */
bool tud_vendor_control_xfer_cb(
    uint8_t rhport,
    uint8_t stage,
    tusb_control_request_t const *request)
{
    (void)rhport;
    if (stage == CONTROL_STAGE_SETUP && request != NULL) {
        ESP_LOGI(TAG,
                 "PC VENDOR_CONTROL: bm=%02X request=%02X value=%04X index=%04X length=%u",
                 request->bmRequestType, request->bRequest, request->wValue,
                 request->wIndex, request->wLength);
    }
    return false;
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t *buffer,
    uint16_t requested_length)
{
    uint8_t interface_number = 0;
    if (buffer == NULL || requested_length == 0U ||
        requested_length > DUAL_HID_CONTROL_MAX_DATA ||
        (uint8_t)report_type < DUAL_HID_REPORT_TYPE_INPUT ||
        (uint8_t)report_type > DUAL_HID_REPORT_TYPE_FEATURE ||
        s_vendor_control_queue == NULL || s_get_gate == NULL ||
        s_get_state_mutex == NULL ||
        !clone_interface_for_instance(instance, &interface_number)) {
        return 0;
    }
    ESP_LOGI(TAG,
             "PC GET_REPORT: instance=%u interface=%u id=%02X type=%u requested=%u",
             instance, interface_number, report_id, (unsigned)report_type,
             requested_length);
    if (xSemaphoreTake(s_get_gate, 0) != pdTRUE) {
        ++s_vendor_get_timeouts;
        return 0;
    }
    while (s_get_response_sem != NULL &&
           xSemaphoreTake(s_get_response_sem, 0) == pdTRUE) {
        /* Consume a stale completion before starting a new transaction. */
    }
    const uint16_t transaction_id = next_vendor_transaction_id();
    xSemaphoreTake(s_get_state_mutex, portMAX_DELAY);
    s_get_inflight = true;
    s_get_response_ready = false;
    s_get_transaction_id = transaction_id;
    s_get_interface_number = interface_number;
    s_get_report_id = report_id;
    s_get_status = DUAL_HID_REPORT_STATUS_TIMEOUT;
    s_get_response_length = 0;
    xSemaphoreGive(s_get_state_mutex);

    const vendor_control_item_t item = {
        .get_report = true,
        .transaction_id = transaction_id,
        .interface_number = interface_number,
        .report_id = report_id,
        .report_type = (uint8_t)report_type,
        .requested_length = (uint8_t)requested_length,
    };
    if (xQueueSend(s_vendor_control_queue, &item, 0) != pdTRUE) {
        ++s_vendor_get_timeouts;
        clear_pending_get(false, DUAL_HID_REPORT_STATUS_TIMEOUT);
        xSemaphoreGive(s_get_gate);
        return 0;
    }
    ++s_vendor_get_requests;
    const bool signaled = s_get_response_sem != NULL &&
        xSemaphoreTake(s_get_response_sem, pdMS_TO_TICKS(VENDOR_GET_REPORT_TIMEOUT_MS)) == pdTRUE;
    uint16_t result_length = 0;
    xSemaphoreTake(s_get_state_mutex, portMAX_DELAY);
    if (signaled && s_get_response_ready &&
        s_get_transaction_id == transaction_id &&
        s_get_interface_number == interface_number &&
        s_get_report_id == report_id &&
        s_get_status == DUAL_HID_REPORT_STATUS_OK &&
        s_get_response_length <= requested_length) {
        result_length = s_get_response_length;
        memcpy(buffer, s_get_response_data, result_length);
        if (result_length != 0U) {
            ESP_LOG_BUFFER_HEX_LEVEL(
                TAG, buffer, result_length, ESP_LOG_INFO);
        }
    } else if (!signaled) {
        ++s_vendor_get_timeouts;
    }
    s_get_inflight = false;
    s_get_response_ready = false;
    xSemaphoreGive(s_get_state_mutex);
    xSemaphoreGive(s_get_gate);
    ESP_LOGI(TAG,
             "PC GET_REPORT完成: interface=%u id=%02X returned=%u status=%u",
             interface_number, report_id, result_length, s_get_status);
    return result_length;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t const *buffer,
    uint16_t buffer_size)
{
    uint8_t interface_number = 0;
    if (s_vendor_control_queue == NULL ||
        !clone_interface_for_instance(instance, &interface_number) ||
        buffer_size > DUAL_HID_CONTROL_MAX_DATA ||
        (buffer == NULL && buffer_size != 0U) ||
        (uint8_t)report_type < DUAL_HID_REPORT_TYPE_INPUT ||
        (uint8_t)report_type > DUAL_HID_REPORT_TYPE_FEATURE) {
        ++s_vendor_set_dropped;
        return;
    }
    vendor_control_item_t item = {
        .get_report = false,
        .transaction_id = next_vendor_transaction_id(),
        .interface_number = interface_number,
        .report_id = report_id,
        .report_type = (uint8_t)report_type,
        .length = (uint8_t)buffer_size,
    };
    if (buffer_size != 0U) {
        memcpy(item.data, buffer, buffer_size);
    }
    ESP_LOGI(TAG,
             "PC SET_REPORT: instance=%u interface=%u id=%02X type=%u length=%u",
             instance, interface_number, report_id, (unsigned)report_type,
             buffer_size);
    if (buffer_size != 0U) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, buffer_size, ESP_LOG_INFO);
    }
    if (xQueueSend(s_vendor_control_queue, &item, 0) != pdTRUE) {
        ++s_vendor_set_dropped;
    } else {
        ++s_vendor_set_queued;
    }
}

void dual_pc_hid_handle_vendor_frame(const dual_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }
    if (frame->type == DUAL_MESSAGE_RAW_HID_INPUT) {
        const uint8_t *data = NULL;
        size_t data_length = 0;
        vendor_input_item_t item = {0};
        if (!dual_hid_raw_input_decode(
                frame->payload, frame->payload_length, &item.interface_number,
                &item.report_id, &data, &data_length) ||
            data_length > sizeof(item.data)) {
            ++s_vendor_input_dropped;
            return;
        }
        item.length = (uint8_t)data_length;
        if (data_length != 0U) {
            memcpy(item.data, data, data_length);
        }
        ++s_vendor_input_received;
        if (s_vendor_input_queue == NULL ||
            xQueueSend(s_vendor_input_queue, &item, 0) != pdTRUE) {
            ++s_vendor_input_dropped;
        }
        return;
    }
    if (frame->type != DUAL_MESSAGE_HID_GET_REPORT_RESPONSE) {
        return;
    }
    const uint8_t *data = NULL;
    size_t data_length = 0;
    uint16_t transaction_id = 0;
    uint8_t status = DUAL_HID_REPORT_STATUS_INVALID;
    uint8_t interface_number = 0;
    uint8_t report_id = 0;
    if (!dual_hid_get_response_decode(
            frame->payload, frame->payload_length, &transaction_id, &status,
            &interface_number, &report_id, &data, &data_length) ||
        data_length > sizeof(s_get_response_data) || s_get_state_mutex == NULL) {
        ++s_vendor_get_mismatches;
        return;
    }
    xSemaphoreTake(s_get_state_mutex, portMAX_DELAY);
    const bool matches = s_get_inflight &&
        s_get_transaction_id == transaction_id &&
        s_get_interface_number == interface_number &&
        s_get_report_id == report_id;
    if (!matches) {
        ++s_vendor_get_mismatches;
        xSemaphoreGive(s_get_state_mutex);
        return;
    }
    s_get_status = status;
    s_get_response_length = (uint8_t)data_length;
    if (data_length != 0U) {
        memcpy(s_get_response_data, data, data_length);
    }
    s_get_response_ready = true;
    if (s_get_response_sem != NULL) {
        xSemaphoreGive(s_get_response_sem);
    }
    xSemaphoreGive(s_get_state_mutex);
}

void dual_pc_hid_vendor_link_fault(void)
{
    if (s_vendor_input_queue != NULL) {
        xQueueReset(s_vendor_input_queue);
    }
    if (s_vendor_control_queue != NULL) {
        xQueueReset(s_vendor_control_queue);
    }
    clear_pending_get(true, DUAL_HID_REPORT_STATUS_TIMEOUT);
    if (s_get_gate != NULL) {
        (void)xSemaphoreGive(s_get_gate);
    }
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
        if (!s_usb_reconfigure_in_progress) {
            dual_pc_hid_vendor_link_fault();
        }
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
    if (s_vendor_input_task != NULL) {
        xTaskNotifyGive(s_vendor_input_task);
    }
    if (instance == (s_clone_active ? s_clone_mouse_instance : 0U)) {
        ++s_hid_completions;
        if (s_sender_task != NULL) {
            xTaskNotifyGive(s_sender_task);
        }
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
    if (instance != (s_clone_active ? s_clone_mouse_instance : 0U)) {
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
    if (s_last_stats_us == 0 || now_us - s_last_stats_us >= 1000000LL) {
        int64_t pending_x = 0;
        int64_t pending_y = 0;
        int64_t pending_wheel = 0;
        int64_t pending_pan = 0;
        if (s_state_mutex != NULL) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            const mouse_motion_delta_t scheduled =
                mouse_motion_smoother_pending(&s_software_smoother);
            pending_x = add_stat_axis(
                add_stat_axis(s_state.physical_x, s_state.software_x), scheduled.x);
            pending_y = add_stat_axis(
                add_stat_axis(s_state.physical_y, s_state.software_y), scheduled.y);
            pending_wheel = add_stat_axis(
                add_stat_axis(s_state.physical_wheel, s_state.software_wheel),
                scheduled.wheel);
            pending_pan = add_stat_axis(
                add_stat_axis(s_state.physical_pan, s_state.software_pan), scheduled.pan);
            xSemaphoreGive(s_state_mutex);
        }
        const int64_t input_x = add_stat_axis(s_physical_input_x, s_software_input_x);
        const int64_t input_y = add_stat_axis(s_physical_input_y, s_software_input_y);
        const int64_t input_wheel = add_stat_axis(s_physical_input_wheel, s_software_input_wheel);
        const int64_t input_pan = add_stat_axis(s_physical_input_pan, s_software_input_pan);
        const uint32_t timer_hz = s_hid_timer_ticks - s_last_stats_timer_ticks;
        const uint32_t physical_rx_hz =
            s_physical_received - s_last_stats_physical_received;
        const uint32_t submitted_hz = s_hid_submitted - s_last_stats_submitted;
        const uint32_t completion_hz = s_hid_completions - s_last_stats_completions;
        ESP_LOGI(TAG,
                 "HID统计：rate timer/phys_rx/submit/complete=%" PRIu32 "/%" PRIu32
                     "/%" PRIu32 "/%" PRIu32
                     " total timer=%" PRIu32 " not_mounted=%" PRIu32 " not_ready=%" PRIu32
                 " attempt=%" PRIu32 " submitted=%" PRIu32 " failed=%" PRIu32
                     " complete=%" PRIu32 " transfer_fail=%" PRIu32 " physical_rx=%" PRIu32
                     " clone_suppressed=%" PRIu32
                  " vendor_rx=%" PRIu32 " vendor_submitted=%" PRIu32
                      " vendor_dropped=%" PRIu32
                      " set_queued/dropped=%" PRIu32 "/%" PRIu32
                      " get_requests/timeouts/mismatches=%" PRIu32 "/%" PRIu32 "/%" PRIu32
                     " input_phys=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")"
                 " input_soft=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")"
                 " output=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")"
                 " pending=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")"
                  " balance=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")",
                 timer_hz, physical_rx_hz, submitted_hz, completion_hz,
                 s_hid_timer_ticks, s_hid_not_mounted, s_hid_not_ready,
                  s_hid_attempts, s_hid_submitted, s_hid_submit_failures,
                  s_hid_completions, s_hid_transfer_failures, s_physical_received,
                   s_clone_input_suppressed, s_vendor_input_received,
                   s_vendor_input_submitted, s_vendor_input_dropped,
                   s_vendor_set_queued, s_vendor_set_dropped,
                   s_vendor_get_requests, s_vendor_get_timeouts,
                   s_vendor_get_mismatches,
                 s_physical_input_x, s_physical_input_y, s_physical_input_wheel, s_physical_input_pan,
                 s_software_input_x, s_software_input_y, s_software_input_wheel, s_software_input_pan,
                 s_output_x, s_output_y, s_output_wheel, s_output_pan,
                 pending_x, pending_y, pending_wheel, pending_pan,
                 input_x - s_output_x - pending_x,
                 input_y - s_output_y - pending_y,
                  input_wheel - s_output_wheel - pending_wheel,
                  input_pan - s_output_pan - pending_pan);
        s_last_stats_timer_ticks = s_hid_timer_ticks;
        s_last_stats_physical_received = s_physical_received;
        s_last_stats_submitted = s_hid_submitted;
        s_last_stats_completions = s_hid_completions;
        s_last_stats_us = now_us;
    }
}

static void sender_task(void *argument)
{
    (void)argument;
    while (!s_sender_stop_requested) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_sender_stop_requested) {
            break;
        }
        log_hid_statistics_if_due();
        if (!tud_mounted()) {
            ++s_hid_not_mounted;
            continue;
        }
        if (s_clone_active) {
            if (!tud_hid_n_ready(s_clone_mouse_instance)) {
                ++s_hid_not_ready;
                continue;
            }
            dual_mouse_report_t report;
            uint8_t payload[DUAL_HID_RAW_INPUT_MAX_DATA] = {0};
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            const bool force = s_force_release;
            apply_next_scheduled_software_locked();
            const bool software_flash_pending = s_software_flash_pending;
            const bool available = dual_input_peek_report(
                &s_state, &report, force);
            if (!available) {
                xSemaphoreGive(s_state_mutex);
                continue;
            }
            const uint8_t payload_length = s_clone_mouse_template_length;
            if (payload_length == 0U ||
                payload_length != s_clone_mouse_layout.report_bytes) {
                ++s_hid_submit_failures;
                xSemaphoreGive(s_state_mutex);
                continue;
            }
            if (s_clone_mouse_template_valid) {
                memcpy(payload, s_clone_mouse_template, payload_length);
            }
            if (!hid_mouse_report_apply_overlay(
                    &s_clone_mouse_layout, payload, payload_length,
                    report.buttons, report.x, report.y,
                    report.wheel, report.pan)) {
                ++s_hid_submit_failures;
                xSemaphoreGive(s_state_mutex);
                continue;
            }
            ++s_hid_attempts;
            if (!tud_hid_n_report(
                    s_clone_mouse_instance, s_clone_mouse_layout.report_id,
                    payload, payload_length)) {
                ++s_hid_submit_failures;
                xSemaphoreGive(s_state_mutex);
                continue;
            }
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
        apply_next_scheduled_software_locked();
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
    s_sender_task = NULL;
    if (s_sender_stopped != NULL) {
        xSemaphoreGive(s_sender_stopped);
    }
    vTaskDelete(NULL);
}

static esp_err_t install_tinyusb(bool clone)
{
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event_callback);
    if (clone) {
        config.descriptor.device = &s_clone_device_descriptor;
        config.descriptor.string = s_clone_string_descriptors;
        config.descriptor.string_count = s_clone_string_count;
        config.descriptor.full_speed_config = s_clone_descriptors.configuration_descriptor;
    } else {
        config.descriptor.device = &s_device_descriptor;
        config.descriptor.string = s_string_descriptors;
        config.descriptor.string_count =
            sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0]);
        config.descriptor.full_speed_config = s_configuration_descriptor;
    }
    const esp_err_t result = tinyusb_driver_install(&config);
    if (result == ESP_OK) {
        s_installed = true;
    }
    return result;
}

esp_err_t dual_pc_hid_stop_sender(void)
{
    if (s_sender_timer != NULL) {
        (void)esp_timer_stop(s_sender_timer);
        (void)esp_timer_delete(s_sender_timer);
        s_sender_timer = NULL;
    }
    if (s_sender_task == NULL) {
        return ESP_OK;
    }
    s_sender_stop_requested = true;
    xTaskNotifyGive(s_sender_task);
    if (xSemaphoreTake(s_sender_stopped, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t restart_runtime_after_install(void)
{
    esp_err_t result = dual_pc_hid_start_sender();
    if (result != ESP_OK) {
        return result;
    }
    /* 严格克隆模式不暴露 CDC，避免改变 Logitech 设备的接口拓扑。 */
    if (s_clone_active) {
        return ESP_OK;
    }
    result = dual_usb_cdc_control_start(NULL, NULL);
    if (result != ESP_OK) {
        (void)dual_pc_hid_stop_sender();
    }
    return result;
}

static esp_err_t stop_installed_usb(void)
{
    if (!s_installed) {
        return ESP_OK;
    }
    dual_pc_hid_release_all();
    vTaskDelay(pdMS_TO_TICKS(3));
    esp_err_t result = dual_usb_cdc_control_stop();
    if (result == ESP_OK) {
        result = dual_pc_hid_stop_sender();
    }
    if (result == ESP_OK) {
        result = tinyusb_driver_uninstall();
    }
    if (result == ESP_OK) {
        s_installed = false;
        dual_status_led_set_pc_mounted(false);
    }
    return result;
}

static void reconfigure_task(void *argument)
{
    (void)argument;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_reconfigure_mutex, portMAX_DELAY);
        const bool disconnect_requested = s_reconfigure_disconnect_requested;
        memcpy(&s_reconfigure_work_profile, &s_reconfigure_profile,
               sizeof(s_reconfigure_work_profile));
        xSemaphoreGive(s_reconfigure_mutex);

        if (disconnect_requested) {
            s_usb_reconfigure_in_progress = false;
            const esp_err_t result = stop_installed_usb();
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "物理鼠标消失后断开USB失败：%s", esp_err_to_name(result));
                continue;
            }
            s_clone_active = false;
            s_clone_mouse_template_valid = false;
            s_clone_mouse_template_length = 0;
            memset(s_clone_mouse_template, 0, sizeof(s_clone_mouse_template));
            memset(&s_active_profile, 0, sizeof(s_active_profile));
            ESP_LOGW(TAG, "接收端USB已断开，等待新的完整物理HID Profile");
            continue;
        }

        if (!prepare_clone_descriptor_set(&s_reconfigure_work_profile)) {
            ESP_LOGE(TAG, "物理HID Profile不满足安全克隆条件，保持USB断开");
            continue;
        }

        s_usb_reconfigure_in_progress = true;
        esp_err_t result = stop_installed_usb();
        if (result != ESP_OK) {
            s_usb_reconfigure_in_progress = false;
            ESP_LOGE(TAG, "动态USB切换前停止任务失败：%s", esp_err_to_name(result));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(300));

        memcpy(&s_active_profile, &s_reconfigure_work_profile,
               sizeof(s_active_profile));
        s_clone_active = true;
        result = install_tinyusb(true);
        if (result == ESP_OK) {
            result = restart_runtime_after_install();
        }
        s_usb_reconfigure_in_progress = false;
        if (result == ESP_OK) {
            ESP_LOGI(TAG,
                     "动态USB严格克隆已启用：VID:PID=%04X:%04X HID=%u mouse_instance=%u CDC=disabled",
                     s_clone_device_descriptor.idVendor,
                     s_clone_device_descriptor.idProduct,
                     s_clone_descriptors.hid_count,
                     s_clone_mouse_instance);
            continue;
        }

        ESP_LOGE(TAG, "动态USB克隆启动失败：%s；保持USB断开", esp_err_to_name(result));
        if (s_installed) {
            (void)dual_usb_cdc_control_stop();
            (void)dual_pc_hid_stop_sender();
            (void)tinyusb_driver_uninstall();
            s_installed = false;
        }
        s_clone_active = false;
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
    mouse_motion_smoother_reset(&s_software_smoother);
    s_software_buttons = 0;
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
    s_last_stats_timer_ticks = 0;
    s_last_stats_physical_received = 0;
    s_last_stats_submitted = 0;
    s_last_stats_completions = 0;
    s_clone_active = false;
    s_clone_input_suppressed = 0;
    s_clone_mouse_template_length = 0;
    s_clone_mouse_template_valid = false;
    memset(s_clone_mouse_template, 0, sizeof(s_clone_mouse_template));
    const esp_err_t result = install_tinyusb(false);
    if (result != ESP_OK) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return result;
    }
    return ESP_OK;
}

esp_err_t dual_pc_hid_start_sender(void)
{
    if (!s_installed || s_sender_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t vendor_result = ensure_vendor_runtime();
    if (vendor_result != ESP_OK) {
        return vendor_result;
    }
    if (s_sender_stopped == NULL) {
        s_sender_stopped = xSemaphoreCreateBinary();
        if (s_sender_stopped == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_sender_stop_requested = false;
    if (xTaskCreate(sender_task, "dual_hid_sender", 4096, NULL, 8,
                    &s_sender_task) != pdPASS) {
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

void dual_pc_hid_enable_reconfigure(void)
{
    if (s_reconfigure_mutex == NULL) {
        s_reconfigure_mutex = xSemaphoreCreateMutex();
    }
    if (s_reconfigure_mutex == NULL) {
        ESP_LOGE(TAG, "创建动态USB Profile互斥锁失败");
        return;
    }
    if (s_reconfigure_task == NULL &&
        xTaskCreate(reconfigure_task, "dual_usb_reconfig", RECONFIGURE_TASK_STACK,
                    NULL, RECONFIGURE_TASK_PRIORITY, &s_reconfigure_task) != pdPASS) {
        ESP_LOGE(TAG, "创建动态USB重枚举任务失败");
        return;
    }
    s_reconfigure_enabled = true;
    s_reconfigure_disconnect_requested = false;
}

esp_err_t dual_pc_hid_schedule_reconfigure(const hid_device_profile_t *profile)
{
    if (!s_reconfigure_enabled || s_reconfigure_mutex == NULL ||
        s_reconfigure_task == NULL || profile == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_reconfigure_mutex, portMAX_DELAY);
    s_reconfigure_disconnect_requested = false;
    memcpy(&s_reconfigure_profile, profile, sizeof(s_reconfigure_profile));
    xSemaphoreGive(s_reconfigure_mutex);
    xTaskNotifyGive(s_reconfigure_task);
    return ESP_OK;
}

esp_err_t dual_pc_hid_schedule_disconnect(void)
{
    if (!s_reconfigure_enabled || s_reconfigure_mutex == NULL ||
        s_reconfigure_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_reconfigure_mutex, portMAX_DELAY);
    s_reconfigure_disconnect_requested = true;
    xSemaphoreGive(s_reconfigure_mutex);
    xTaskNotifyGive(s_reconfigure_task);
    return ESP_OK;
}

void dual_pc_hid_software_report(
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan,
    uint8_t smoothing_slots)
{
    if (s_state_mutex == NULL ||
        !mouse_motion_smoother_valid_slot_count(smoothing_slots)) {
        return;
    }
    const mouse_motion_delta_t delta = {
        .x = x,
        .y = y,
        .wheel = wheel,
        .pan = pan,
    };
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_software_buttons = buttons & 0x1FU;
    dual_input_software_report(&s_state, s_software_buttons, 0, 0, 0, 0);
    mouse_motion_smoother_enqueue(&s_software_smoother, delta, smoothing_slots);
    s_software_flash_pending = true;
    s_software_input_x += x;
    s_software_input_y += y;
    s_software_input_wheel += wheel;
    s_software_input_pan += pan;
    xSemaphoreGive(s_state_mutex);
    /* 即使 sender 任务异常未运行，也从输入侧留下每秒一次的闭环诊断。 */
    log_hid_statistics_if_due();
}

void dual_pc_hid_software_release(void)
{
    if (s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    mouse_motion_smoother_reset(&s_software_smoother);
    s_software_buttons = 0;
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
    mouse_motion_smoother_reset(&s_software_smoother);
    s_software_buttons = 0;
    dual_input_release_all(&s_state);
    s_software_flash_pending = false;
    s_force_release = true;
    xSemaphoreGive(s_state_mutex);
}

bool dual_pc_hid_ready(void)
{
    return s_installed && tud_mounted() && tud_hid_ready();
}
