#include "hid_host_cherryusb_backend.h"
#include "hid_host_cherryusb_logic.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usbh_core.h"
#include "usb_errno.h"
#include "usbh_hid.h"

#define CHERRY_BUS_ID 0U
#define CHERRY_CONTROL_BUSY_TIMEOUT_MS 25U
#define CHERRY_HEARTBEAT_PERIOD_MS 1000U
#define CHERRY_SUMMARY_PERIOD 5U
#define CHERRY_HEARTBEAT_HISTORY_REPLAY_PERIOD_US 60000000LL
#define CHERRY_HEARTBEAT_TIMEOUT_MS 250U
#define CHERRY_REPORT_PERIOD_FALLBACK_US 1000U
#define CHERRY_STOP_TIMEOUT_MS 3000U
#define CHERRY_MAX_REPORT_PACKET 64U
#define CHERRY_EP0_BUFFER_SIZE HID_PROFILE_MAX_CONFIG_DESCRIPTOR
#define CHERRY_USB_STRING_DESCRIPTOR_MAX 255U
#define CHERRY_PROFILE_CONTROL_TIMEOUT_MS 250U
#define CHERRY_HEARTBEAT_EXIT_BIT (1U << 0)

static const char *TAG = "hid_cherryusb";

typedef struct {
    bool used;
    bool interface_open;
    bool report_started;
    volatile bool stopping;
    volatile bool urb_active;
    volatile uint32_t timer_callback_users;
    volatile uint32_t completion_users;
    uint8_t interface_number;
    uint8_t subclass;
    uint8_t protocol;
    uint16_t endpoint_packet_size;
    uint32_t generation;
    struct usbh_hid *hid_class;
    struct usbh_hubport *hport;
    esp_timer_handle_t timer;
    uint8_t *input_buffer;
} cherry_hid_state_t;

typedef struct {
    bool lifecycle_wait_valid;
    uint32_t lifecycle_wait_us;
    bool ep0_wait_valid;
    uint32_t ep0_wait_us;
    bool urb_time_valid;
    uint32_t urb_time_us;
    bool raw_urb_result_valid;
    int32_t raw_urb_result;
    bool raw_actual_length_valid;
    uint32_t raw_actual_length;
    bool payload_length_valid;
    uint32_t payload_length;
    bool descriptor_header_valid;
    uint8_t descriptor_length;
    uint8_t descriptor_type;
    bool device_state_valid;
    uint32_t device_epoch;
    bool connected;
    bool stopping;
} cherry_control_trace_t;

typedef struct {
    const char *stage;
    const char *field;
    esp_err_t result;
    bool actual_length_valid;
    size_t actual_length;
    bool control_stage_valid;
    cherry_control_stage_t control_stage;
    bool trace_valid;
    cherry_control_trace_t trace;
} cherry_hid_init_diagnostic_t;

static SemaphoreHandle_t s_lifecycle_guard;
static SemaphoreHandle_t s_profile_guard;
static EventGroupHandle_t s_task_events;
static TaskHandle_t s_heartbeat_task;
static struct usbh_hubport *s_hport;
static bool s_initialized;
static volatile bool s_shutdown_requested;
static volatile bool s_device_stopping;
static volatile uint32_t s_guard_users;
static volatile uint32_t s_timer_callback_users;
static volatile uint32_t s_input_callback_users;
static uint32_t s_generation;
static uint32_t s_device_epoch;
static cherry_hid_state_t s_hid_states[HID_PROFILE_MAX_REPORT_DESCRIPTORS];
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static hid_device_profile_t s_profile_cache;
static bool s_profile_base_valid;
static bool s_profile_ready;
static uint8_t s_profile_expected_interfaces;
static bool s_profile_seen_interfaces[256];
static uint32_t s_input_reports;
static uint32_t s_input_bytes;
static uint32_t s_input_zero_completions;
static uint32_t s_input_nak_completions;
static uint32_t s_input_errors;
static uint32_t s_input_submit_errors;
static cherry_heartbeat_diagnostics_t s_heartbeat_diagnostics;
static int32_t s_heartbeat_last_result;
static uint32_t s_heartbeat_last_stage;
static uint32_t s_heartbeat_last_latency_us;
static int64_t s_last_input_us;
static int64_t s_last_heartbeat_history_replay_us;

static bool any_hid_state_active(void);
static void reset_profile_for_device_epoch(void);

/* EP0 SETUP and data buffers must remain DMA-aligned for the CherryUSB DWC2 port. */
static struct usb_setup_packet s_setup_storage USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX;
static uint8_t s_ep0_dma_buffer[USB_ALIGN_UP(CHERRY_EP0_BUFFER_SIZE,
                                             CONFIG_USB_ALIGN_SIZE)] USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX;
static uint8_t s_report_descriptor_buffer[USB_ALIGN_UP(
    HID_PROFILE_MAX_REPORT_DESCRIPTOR, CONFIG_USB_ALIGN_SIZE)] USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX;

void ld_include_hid(void)
{
}

static TickType_t remaining_ticks(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    uint64_t remaining_ms = ((uint64_t)remaining_us + 999U) / 1000U;
    if (remaining_ms > UINT32_MAX) {
        remaining_ms = UINT32_MAX;
    }
    TickType_t ticks = pdMS_TO_TICKS((uint32_t)remaining_ms);
    return ticks == 0U ? 1U : ticks;
}

static esp_err_t acquire_registered_hport(
    struct usbh_hubport *expected,
    uint32_t timeout_ms,
    struct usbh_hubport **out_hport,
    int64_t *out_deadline_us,
    cherry_control_stage_t *stage,
    cherry_control_trace_t *trace)
{
    if (stage != NULL) {
        *stage = CHERRY_CONTROL_STAGE_DEVICE_STATE;
    }
    if (s_lifecycle_guard == NULL || out_hport == NULL || timeout_ms == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)timeout_ms * 1000LL;
    const int64_t wait_started_us = esp_timer_get_time();
    const TickType_t ticks = remaining_ticks(deadline_us);
    if (ticks == 0U) {
        if (stage != NULL) {
            *stage = CHERRY_CONTROL_STAGE_DEADLINE;
        }
        return ESP_ERR_TIMEOUT;
    }
    const BaseType_t lock_result = xSemaphoreTake(s_lifecycle_guard, ticks);
    if (trace != NULL) {
        trace->lifecycle_wait_valid = true;
        trace->lifecycle_wait_us = (uint32_t)(esp_timer_get_time() - wait_started_us);
    }
    if (lock_result != pdTRUE) {
        if (stage != NULL) {
            *stage = CHERRY_CONTROL_STAGE_LIFECYCLE_LOCK;
        }
        return ESP_ERR_TIMEOUT;
    }
    struct usbh_hubport *hport = s_hport;
    if (trace != NULL) {
        trace->device_state_valid = true;
        trace->device_epoch = __atomic_load_n(&s_device_epoch, __ATOMIC_ACQUIRE);
        trace->connected = hport != NULL && hport->connected;
        trace->stopping = __atomic_load_n(&s_device_stopping, __ATOMIC_ACQUIRE);
    }
    const bool valid = !s_shutdown_requested && !s_device_stopping &&
        hport != NULL && hport->connected &&
        (expected == NULL || expected == hport);
    if (valid) {
        __atomic_add_fetch(&s_guard_users, 1U, __ATOMIC_ACQ_REL);
        *out_hport = hport;
        if (out_deadline_us != NULL) {
            *out_deadline_us = deadline_us;
        }
        if (stage != NULL) {
            *stage = CHERRY_CONTROL_STAGE_OK;
        }
    }
    xSemaphoreGive(s_lifecycle_guard);
    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t register_hport_and_acquire(
    struct usbh_hubport *hport,
    struct usbh_hubport **out_hport)
{
    if (hport == NULL || out_hport == NULL || s_lifecycle_guard == NULL ||
        xSemaphoreTake(s_lifecycle_guard, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    if (s_shutdown_requested || !hport->connected) {
        result = ESP_ERR_INVALID_STATE;
    } else if (s_hport != NULL && s_hport != hport) {
        result = ESP_ERR_NOT_SUPPORTED;
    } else {
        if (s_hport == NULL) {
            if (s_device_stopping) {
                if (any_hid_state_active()) {
                    xSemaphoreGive(s_lifecycle_guard);
                    return ESP_ERR_INVALID_STATE;
                }
                reset_profile_for_device_epoch();
                s_device_stopping = false;
            }
            __atomic_add_fetch(&s_device_epoch, 1U, __ATOMIC_ACQ_REL);
        }
        s_hport = hport;
        __atomic_add_fetch(&s_guard_users, 1U, __ATOMIC_ACQ_REL);
        *out_hport = hport;
    }
    xSemaphoreGive(s_lifecycle_guard);
    return result;
}

static void release_hport_guard(void)
{
    __atomic_sub_fetch(&s_guard_users, 1U, __ATOMIC_RELEASE);
}

static esp_err_t map_usb_result(int result)
{
    if (result == -USB_ERR_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
    if (result == -USB_ERR_NOTCONN || result == -USB_ERR_NODEV) {
        return ESP_ERR_NOT_FOUND;
    }
    if (result == -USB_ERR_BUSY) {
        return ESP_ERR_INVALID_STATE;
    }
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static const char *control_stage_name(uint32_t stage)
{
    switch ((cherry_control_stage_t)stage) {
    case CHERRY_CONTROL_STAGE_OK:
        return "ok";
    case CHERRY_CONTROL_STAGE_LIFECYCLE_LOCK:
        return "lifecycle_lock";
    case CHERRY_CONTROL_STAGE_EP0_LOCK:
        return "ep0_lock";
    case CHERRY_CONTROL_STAGE_DEADLINE:
        return "deadline";
    case CHERRY_CONTROL_STAGE_URB:
        return "urb";
    case CHERRY_CONTROL_STAGE_VALIDATION:
        return "validation";
    case CHERRY_CONTROL_STAGE_DEVICE_STATE:
        return "device_state";
    case CHERRY_CONTROL_STAGE_OTHER:
        return "other";
    default:
        return "unknown";
    }
}

static void set_hid_init_diagnostic(
    cherry_hid_init_diagnostic_t *diagnostic,
    const char *stage,
    const char *field,
    esp_err_t result,
    bool actual_length_valid,
    size_t actual_length)
{
    if (diagnostic == NULL) {
        return;
    }
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->stage = stage;
    diagnostic->field = field;
    diagnostic->result = result;
    diagnostic->actual_length_valid = actual_length_valid;
    diagnostic->actual_length = actual_length;
}

static void set_hid_control_diagnostic(
    cherry_hid_init_diagnostic_t *diagnostic,
    const char *stage,
    const char *field,
    esp_err_t result,
    cherry_control_stage_t control_stage,
    const cherry_control_trace_t *trace)
{
    set_hid_init_diagnostic(diagnostic, stage, field, result,
                            trace != NULL && trace->payload_length_valid,
                            trace != NULL && trace->payload_length_valid
                                ? trace->payload_length : 0U);
    if (diagnostic != NULL && trace != NULL) {
        diagnostic->control_stage_valid = true;
        diagnostic->control_stage = control_stage;
        diagnostic->trace_valid = true;
        diagnostic->trace = *trace;
    }
}

static const char *format_diag_length(bool valid, size_t value,
                                     char *buffer, size_t buffer_size)
{
    if (!valid) {
        return "unknown";
    }
    (void)snprintf(buffer, buffer_size, "%zu", value);
    return buffer;
}

static const char *format_diag_u32(bool valid, uint32_t value,
                                   char *buffer, size_t buffer_size)
{
    if (!valid) {
        return "unknown";
    }
    (void)snprintf(buffer, buffer_size, "%" PRIu32, value);
    return buffer;
}

static const char *format_diag_i32(bool valid, int32_t value,
                                   char *buffer, size_t buffer_size)
{
    if (!valid) {
        return "unknown";
    }
    (void)snprintf(buffer, buffer_size, "%" PRId32, value);
    return buffer;
}

static void log_hid_init_failure(
    const struct usbh_hubport *hport,
    const struct usbh_hid *hid_class,
    uint8_t interface_number,
    uint16_t report_size,
    uint16_t endpoint_packet_size,
    int64_t started_us,
    const cherry_hid_init_diagnostic_t *diagnostic)
{
    const bool trace_valid = diagnostic != NULL && diagnostic->trace_valid;
    const cherry_control_trace_t *trace = trace_valid ? &diagnostic->trace : NULL;
    char actual_length[16];
    char raw_actual[16];
    char raw_urb[16];
    const uint8_t bus_id = hport != NULL && hport->bus != NULL
        ? hport->bus->busid : UINT8_MAX;
    const uint8_t device_address = hport != NULL ? hport->dev_addr : UINT8_MAX;
    const uint8_t interface_index = hid_class != NULL ? hid_class->intf : UINT8_MAX;
    const uint8_t class_index = hid_class != NULL ? hid_class->minor : UINT8_MAX;
    const esp_err_t result = diagnostic != NULL
        ? diagnostic->result : ESP_ERR_INVALID_STATE;

    ESP_LOGE(TAG,
             "CherryUSB HID init failed: bus=%u addr=%u intf_index=%u intf_num=%u class_index=%u stage=%s field=%s error=%s(%d) report_size=%u endpoint_packet=%u actual_length=%s raw_actual=%s raw_urb=%s control_stage=%s elapsed_us=%" PRId64,
             (unsigned)bus_id, (unsigned)device_address,
             (unsigned)interface_index, (unsigned)interface_number,
             (unsigned)class_index,
             diagnostic != NULL && diagnostic->stage != NULL
                 ? diagnostic->stage : "unknown",
             diagnostic != NULL && diagnostic->field != NULL
                 ? diagnostic->field : "none",
             esp_err_to_name(result), (int)result,
             (unsigned)report_size, (unsigned)endpoint_packet_size,
             format_diag_length(
                 diagnostic != NULL && diagnostic->actual_length_valid,
                 diagnostic != NULL ? diagnostic->actual_length : 0U,
                 actual_length, sizeof(actual_length)),
             format_diag_u32(trace != NULL && trace->raw_actual_length_valid,
                             trace != NULL ? trace->raw_actual_length : 0U,
                             raw_actual, sizeof(raw_actual)),
             format_diag_i32(trace != NULL && trace->raw_urb_result_valid,
                             trace != NULL ? trace->raw_urb_result : 0,
                             raw_urb, sizeof(raw_urb)),
             diagnostic != NULL && diagnostic->control_stage_valid
                 ? control_stage_name(diagnostic->control_stage) : "unknown",
             esp_timer_get_time() - started_us);
}

static void reset_profile_for_device_epoch(void)
{
    if (s_profile_guard == NULL ||
        xSemaphoreTake(s_profile_guard, portMAX_DELAY) != pdTRUE) {
        return;
    }
    hid_device_profile_init(&s_profile_cache);
    memset(s_profile_seen_interfaces, 0, sizeof(s_profile_seen_interfaces));
    s_profile_base_valid = false;
    s_profile_ready = false;
    s_profile_expected_interfaces = 0U;
    xSemaphoreGive(s_profile_guard);
}

static bool any_active_input_urb(void)
{
    bool active = false;
    portENTER_CRITICAL(&s_state_mux);
    for (size_t index = 0; index < HID_PROFILE_MAX_REPORT_DESCRIPTORS; ++index) {
        if (s_hid_states[index].used &&
            __atomic_load_n(&s_hid_states[index].urb_active, __ATOMIC_ACQUIRE)) {
            active = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_mux);
    return active;
}

static bool any_hid_state_active(void)
{
    bool found = false;
    portENTER_CRITICAL(&s_state_mux);
    for (size_t index = 0; index < HID_PROFILE_MAX_REPORT_DESCRIPTORS; ++index) {
        if (s_hid_states[index].used) {
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_mux);
    return found;
}

static void stop_state_timer(cherry_hid_state_t *state)
{
    if (state == NULL || state->timer == NULL) {
        return;
    }
    esp_timer_handle_t timer = state->timer;
    while (true) {
        (void)esp_timer_stop(timer);
        const esp_err_t result = esp_timer_delete(timer);
        if (result == ESP_OK) {
            state->timer = NULL;
            return;
        }
        ESP_LOGE(TAG, "等待HID输入定时器删除失败：interface=%u result=%s；保持状态不释放",
                 state->interface_number, esp_err_to_name(result));
        vTaskDelay(1U);
    }
}

static void stop_all_state_timers(void)
{
    for (size_t index = 0; index < HID_PROFILE_MAX_REPORT_DESCRIPTORS; ++index) {
        cherry_hid_state_t *state = &s_hid_states[index];
        if (state->used) {
            __atomic_store_n(&state->stopping, true, __ATOMIC_RELEASE);
            stop_state_timer(state);
        }
    }
}

static void wait_for_hport_users_and_callbacks(void)
{
    int64_t next_log_us = esp_timer_get_time() + 3000000LL;
    while (__atomic_load_n(&s_guard_users, __ATOMIC_ACQUIRE) != 0U ||
           __atomic_load_n(&s_timer_callback_users, __ATOMIC_ACQUIRE) != 0U) {
        if (esp_timer_get_time() >= next_log_us) {
            ESP_LOGW(TAG, "等待CherryUSB生命周期引用归零：guard=%" PRIu32
                     " timer=%" PRIu32,
                     __atomic_load_n(&s_guard_users, __ATOMIC_ACQUIRE),
                     __atomic_load_n(&s_timer_callback_users, __ATOMIC_ACQUIRE));
            next_log_us = esp_timer_get_time() + 3000000LL;
        }
        vTaskDelay(1U);
    }
}

static void kill_active_input_urbs(void)
{
    for (size_t index = 0; index < HID_PROFILE_MAX_REPORT_DESCRIPTORS; ++index) {
        cherry_hid_state_t *state = &s_hid_states[index];
        if (state->used && state->hid_class != NULL &&
            __atomic_load_n(&state->urb_active, __ATOMIC_ACQUIRE)) {
            (void)usbh_kill_urb(&state->hid_class->intin_urb);
        }
    }
}

static void wait_for_input_urbs_and_callbacks(void)
{
    int64_t next_log_us = esp_timer_get_time() + 3000000LL;
    while (__atomic_load_n(&s_input_callback_users, __ATOMIC_ACQUIRE) != 0U ||
           any_active_input_urb()) {
        if (esp_timer_get_time() >= next_log_us) {
            ESP_LOGW(TAG, "等待HID输入URB与回调结束：callbacks=%" PRIu32
                     " active=%u；保持接口状态不释放",
                     __atomic_load_n(&s_input_callback_users, __ATOMIC_ACQUIRE),
                     any_active_input_urb() ? 1U : 0U);
            next_log_us = esp_timer_get_time() + 3000000LL;
        }
        vTaskDelay(1U);
    }
}

/* Caller holds a lifecycle reference; this function accounts for all wait/URB time. */
static esp_err_t control_transfer_guarded(
    struct usbh_hubport *hport,
    uint8_t bm_request_type,
    uint8_t b_request,
    uint16_t w_value,
    uint16_t w_index,
    uint16_t w_length,
    const uint8_t *out_data,
    uint8_t *in_data,
    size_t data_capacity,
    size_t *data_length,
    int64_t deadline_us,
    cherry_control_stage_t *stage,
    cherry_control_trace_t *trace)
{
    if (stage != NULL) {
        *stage = CHERRY_CONTROL_STAGE_VALIDATION;
    }
    if (hport == NULL || hport->setup == NULL || hport->mutex == NULL ||
        (w_length != 0U && ((bm_request_type & USB_REQUEST_DIR_IN) == 0U) &&
         out_data == NULL) ||
        ((bm_request_type & USB_REQUEST_DIR_IN) != 0U &&
        (in_data == NULL || data_capacity < w_length)) ||
        (((bm_request_type & USB_REQUEST_DIR_IN) == 0U) &&
         w_length != 0U && data_capacity < w_length) ||
        w_length > sizeof(s_ep0_dma_buffer)) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t ticks = remaining_ticks(deadline_us);
    if (ticks == 0U) {
        if (stage != NULL) {
            *stage = CHERRY_CONTROL_STAGE_DEADLINE;
        }
        return ESP_ERR_TIMEOUT;
    }
    const int64_t ep0_wait_started_us = esp_timer_get_time();
    const BaseType_t ep0_lock_result =
        xSemaphoreTake((SemaphoreHandle_t)hport->mutex, ticks);
    if (trace != NULL) {
        trace->ep0_wait_valid = true;
        trace->ep0_wait_us = (uint32_t)(esp_timer_get_time() - ep0_wait_started_us);
    }
    if (ep0_lock_result != pdTRUE) {
        if (stage != NULL) {
            *stage = CHERRY_CONTROL_STAGE_EP0_LOCK;
        }
        return ESP_ERR_TIMEOUT;
    }
    if (!hport->connected || s_shutdown_requested || s_device_stopping) {
        xSemaphoreGive((SemaphoreHandle_t)hport->mutex);
        if (stage != NULL) {
            *stage = CHERRY_CONTROL_STAGE_DEVICE_STATE;
        }
        return ESP_ERR_NOT_FOUND;
    }

    memset(&s_setup_storage, 0, sizeof(s_setup_storage));
    s_setup_storage.bmRequestType = bm_request_type;
    s_setup_storage.bRequest = b_request;
    s_setup_storage.wValue = w_value;
    s_setup_storage.wIndex = w_index;
    s_setup_storage.wLength = w_length;
    *hport->setup = s_setup_storage;
    if ((bm_request_type & USB_REQUEST_DIR_IN) == 0U && w_length != 0U) {
        memcpy(s_ep0_dma_buffer, out_data, w_length);
    } else if (w_length != 0U) {
        memset(s_ep0_dma_buffer, 0, w_length);
    }
    if (w_length < sizeof(s_ep0_dma_buffer)) {
        memset(s_ep0_dma_buffer + w_length, 0,
               sizeof(s_ep0_dma_buffer) - w_length);
    }

    ticks = remaining_ticks(deadline_us);
    if (ticks == 0U) {
        xSemaphoreGive((SemaphoreHandle_t)hport->mutex);
        if (stage != NULL) { *stage = CHERRY_CONTROL_STAGE_DEADLINE; }
        return ESP_ERR_TIMEOUT;
    }
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        xSemaphoreGive((SemaphoreHandle_t)hport->mutex);
        if (stage != NULL) {
            *stage = CHERRY_CONTROL_STAGE_DEADLINE;
        }
        return ESP_ERR_TIMEOUT;
    }
    const uint64_t remaining_ms64 = ((uint64_t)remaining_us + 999U) / 1000U;
    uint32_t remaining_ms = remaining_ms64 > UINT32_MAX
        ? UINT32_MAX : (uint32_t)remaining_ms64;

    usbh_control_urb_fill(&hport->ep0_urb, hport, hport->setup,
                          w_length == 0U ? NULL : s_ep0_dma_buffer,
                          w_length, remaining_ms, NULL, NULL);
    const int64_t urb_started_us = esp_timer_get_time();
    const int raw_result = usbh_submit_urb(&hport->ep0_urb);
    if (trace != NULL) {
        trace->urb_time_valid = true;
        trace->urb_time_us = (uint32_t)(esp_timer_get_time() - urb_started_us);
        trace->raw_urb_result_valid = true;
        trace->raw_urb_result = raw_result;
    }
    esp_err_t result = map_usb_result(raw_result);
    if (stage != NULL) {
        *stage = result == ESP_OK
            ? CHERRY_CONTROL_STAGE_OK : CHERRY_CONTROL_STAGE_URB;
    }
    if (result == ESP_OK) {
        const uint32_t actual = hport->ep0_urb.actual_length;
        if (trace != NULL) {
            trace->raw_actual_length_valid = true;
            trace->raw_actual_length = actual;
        }
        const bool is_in = (bm_request_type & USB_REQUEST_DIR_IN) != 0U;
        if (is_in) {
            size_t payload_length = 0U;
            if (!cherryusb_ep0_payload_length(
                    actual, w_length, data_capacity, &payload_length)) {
                result = ESP_ERR_INVALID_SIZE;
                if (stage != NULL) {
                    *stage = CHERRY_CONTROL_STAGE_VALIDATION;
                }
            } else {
                if (trace != NULL) {
                    trace->payload_length_valid = true;
                    trace->payload_length = (uint32_t)payload_length;
                    if (payload_length >= 2U) {
                        trace->descriptor_header_valid = true;
                        trace->descriptor_length = s_ep0_dma_buffer[0];
                        trace->descriptor_type = s_ep0_dma_buffer[1];
                    }
                }
                if (payload_length != 0U) {
                    memcpy(in_data, s_ep0_dma_buffer, payload_length);
                }
                if (data_length != NULL) {
                    *data_length = payload_length;
                }
            }
        } else if (data_length != NULL) {
            *data_length = 0U;
        }
    }
    xSemaphoreGive((SemaphoreHandle_t)hport->mutex);
    return result;
}

static esp_err_t control_transfer(
    bool get_report,
    bool vendor_control,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    uint8_t bm_request_type,
    uint8_t b_request,
    uint16_t w_value,
    uint16_t w_index,
    uint16_t w_length,
    uint32_t timeout_ms,
    uint8_t *data,
    size_t data_capacity,
    size_t *data_length,
    cherry_control_stage_t *stage,
    cherry_control_trace_t *trace)
{
    if (stage != NULL) {
        *stage = CHERRY_CONTROL_STAGE_VALIDATION;
    }
    struct usbh_hubport *hport = NULL;
    int64_t deadline_us = 0;
    if (timeout_ms == 0U || timeout_ms > 1000U || data_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *data_length = 0U;
    const esp_err_t acquired = acquire_registered_hport(
        NULL, timeout_ms, &hport, &deadline_us, stage, trace);
    if (acquired != ESP_OK) {
        return acquired;
    }

    if (!vendor_control) {
        bm_request_type = get_report
            ? (USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE)
            : (USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE);
        b_request = get_report ? HID_REQUEST_GET_REPORT : HID_REQUEST_SET_REPORT;
        w_value = cherryusb_hid_report_wvalue(report_type, report_id);
        w_index = cherryusb_hid_report_windex(interface_number);
    }
    const esp_err_t result = control_transfer_guarded(
        hport, bm_request_type, b_request, w_value, w_index, w_length,
        (bm_request_type & USB_REQUEST_DIR_IN) != 0U ? NULL : data,
        (bm_request_type & USB_REQUEST_DIR_IN) != 0U ? data : NULL,
        data_capacity, data_length, deadline_us, stage, trace);
    if (trace != NULL) {
        trace->device_state_valid = true;
        trace->device_epoch = __atomic_load_n(&s_device_epoch, __ATOMIC_ACQUIRE);
        trace->connected = hport->connected;
        trace->stopping = __atomic_load_n(&s_device_stopping, __ATOMIC_ACQUIRE);
    }
    release_hport_guard();
    return result;
}

esp_err_t dual_hid_host_cherryusb_control(
    bool get_report,
    bool vendor_control,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    uint8_t bm_request_type,
    uint8_t b_request,
    uint16_t w_value,
    uint16_t w_index,
    uint16_t w_length,
    uint32_t timeout_ms,
    uint8_t *data,
    size_t data_capacity,
    size_t *data_length)
{
    return control_transfer(get_report, vendor_control, interface_number,
                            report_id, report_type, bm_request_type,
                            b_request, w_value, w_index, w_length,
                            timeout_ms, data, data_capacity, data_length, NULL, NULL);
}

static bool fetch_usb_string_descriptor(
    struct usbh_hubport *hport,
    uint8_t index,
    uint16_t language_id,
    uint8_t descriptor[CHERRY_USB_STRING_DESCRIPTOR_MAX],
    size_t *descriptor_length,
    const char *field,
    cherry_hid_init_diagnostic_t *diagnostic)
{
    if (hport == NULL || index == 0U || descriptor == NULL ||
        descriptor_length == NULL) {
        set_hid_init_diagnostic(diagnostic, "profile_string_arguments", field,
                                ESP_ERR_INVALID_ARG, false, 0U);
        return false;
    }

    size_t actual_length = 0U;
    cherry_control_stage_t control_stage = CHERRY_CONTROL_STAGE_VALIDATION;
    cherry_control_trace_t trace = {0};
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)CHERRY_PROFILE_CONTROL_TIMEOUT_MS * 1000LL;
    const esp_err_t result = control_transfer_guarded(
        hport,
        USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE,
        USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)((USB_DESCRIPTOR_TYPE_STRING << 8U) | index),
        language_id,
        CHERRY_USB_STRING_DESCRIPTOR_MAX,
        NULL,
        descriptor,
        CHERRY_USB_STRING_DESCRIPTOR_MAX,
        &actual_length,
        deadline_us,
        &control_stage, &trace);
    if (result != ESP_OK) {
        set_hid_control_diagnostic(diagnostic, "profile_string_fetch", field,
                                   result, control_stage, &trace);
        return false;
    }
    if (actual_length < 2U ||
        actual_length > CHERRY_USB_STRING_DESCRIPTOR_MAX ||
        descriptor[0] != actual_length ||
        descriptor[1] != USB_DESCRIPTOR_TYPE_STRING ||
        (descriptor[0] & 1U) != 0U) {
        set_hid_control_diagnostic(diagnostic, "profile_string_validate", field,
                                   ESP_ERR_INVALID_SIZE,
                                   CHERRY_CONTROL_STAGE_VALIDATION, &trace);
        return false;
    }

    *descriptor_length = actual_length;
    return true;
}

static bool fetch_usb_string_language_id(
    struct usbh_hubport *hport,
    uint16_t *language_id,
    cherry_hid_init_diagnostic_t *diagnostic)
{
    uint8_t descriptor[CHERRY_USB_STRING_DESCRIPTOR_MAX] = {0};
    size_t descriptor_length = 0U;
    if (hport == NULL || language_id == NULL) {
        set_hid_init_diagnostic(diagnostic, "profile_langid_arguments", "language_id",
                                ESP_ERR_INVALID_ARG, false, 0U);
        return false;
    }

    size_t actual_length = 0U;
    cherry_control_stage_t control_stage = CHERRY_CONTROL_STAGE_VALIDATION;
    cherry_control_trace_t trace = {0};
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)CHERRY_PROFILE_CONTROL_TIMEOUT_MS * 1000LL;
    const esp_err_t result = control_transfer_guarded(
        hport,
        USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE,
        USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)(USB_DESCRIPTOR_TYPE_STRING << 8U),
        0U,
        CHERRY_USB_STRING_DESCRIPTOR_MAX,
        NULL,
        descriptor,
        sizeof(descriptor),
        &actual_length,
        deadline_us,
        &control_stage, &trace);
    if (result != ESP_OK) {
        set_hid_control_diagnostic(diagnostic, "profile_langid_fetch", "language_id",
                                   result, control_stage, &trace);
        return false;
    }
    if (actual_length < 4U ||
        actual_length > sizeof(descriptor) ||
        descriptor[0] != actual_length ||
        descriptor[1] != USB_DESCRIPTOR_TYPE_STRING ||
        (descriptor[0] & 1U) != 0U) {
        set_hid_control_diagnostic(diagnostic, "profile_langid_validate", "language_id",
                                   ESP_ERR_INVALID_SIZE,
                                   CHERRY_CONTROL_STAGE_VALIDATION, &trace);
        return false;
    }

    descriptor_length = descriptor[0];
    if (descriptor_length < 4U) {
        set_hid_control_diagnostic(diagnostic, "profile_langid_validate", "language_id",
                                   ESP_ERR_INVALID_SIZE,
                                   CHERRY_CONTROL_STAGE_VALIDATION, &trace);
        return false;
    }
    const uint16_t first_language = (uint16_t)descriptor[2] |
        ((uint16_t)descriptor[3] << 8U);
    if (first_language == 0U) {
        set_hid_control_diagnostic(diagnostic, "profile_langid_validate", "language_id",
                                   ESP_ERR_INVALID_SIZE,
                                   CHERRY_CONTROL_STAGE_VALIDATION, &trace);
        return false;
    }
    *language_id = first_language;
    return true;
}

static bool usb_string_utf16le_to_profile(
    const uint8_t *descriptor,
    size_t descriptor_length,
    hid_profile_string_t *destination)
{
    if (descriptor == NULL || destination == NULL || descriptor_length < 2U ||
        descriptor[0] != descriptor_length ||
        descriptor[1] != USB_DESCRIPTOR_TYPE_STRING ||
        ((descriptor_length - 2U) & 1U) != 0U) {
        return false;
    }

    char utf8[HID_PROFILE_MAX_STRING_BYTES + 1U];
    size_t written = 0U;
    for (size_t offset = 2U; offset < descriptor_length;) {
        uint32_t codepoint = (uint32_t)descriptor[offset] |
            ((uint32_t)descriptor[offset + 1U] << 8U);
        offset += 2U;
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
            if (offset + 1U >= descriptor_length) {
                return false;
            }
            const uint32_t low = (uint32_t)descriptor[offset] |
                ((uint32_t)descriptor[offset + 1U] << 8U);
            if (low < 0xDC00U || low > 0xDFFFU) {
                return false;
            }
            offset += 2U;
            codepoint = 0x10000U +
                (((codepoint - 0xD800U) << 10U) | (low - 0xDC00U));
        } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
            return false;
        }
        if (codepoint == 0U) {
            return false;
        }

        const size_t needed = codepoint <= 0x7FU ? 1U :
            codepoint <= 0x7FFU ? 2U : codepoint <= 0xFFFFU ? 3U : 4U;
        if (written + needed > HID_PROFILE_MAX_STRING_BYTES) {
            return false;
        }
        if (needed == 1U) {
            utf8[written++] = (char)codepoint;
        } else if (needed == 2U) {
            utf8[written++] = (char)(0xC0U | (codepoint >> 6U));
            utf8[written++] = (char)(0x80U | (codepoint & 0x3FU));
        } else if (needed == 3U) {
            utf8[written++] = (char)(0xE0U | (codepoint >> 12U));
            utf8[written++] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
            utf8[written++] = (char)(0x80U | (codepoint & 0x3FU));
        } else {
            utf8[written++] = (char)(0xF0U | (codepoint >> 18U));
            utf8[written++] = (char)(0x80U | ((codepoint >> 12U) & 0x3FU));
            utf8[written++] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
            utf8[written++] = (char)(0x80U | (codepoint & 0x3FU));
        }
    }
    utf8[written] = '\0';
    return hid_device_profile_set_string(destination, utf8, written);
}

static bool populate_profile_string(
    struct usbh_hubport *hport,
    uint8_t index,
    uint16_t language_id,
    hid_profile_string_t *destination,
    const char *field,
    cherry_hid_init_diagnostic_t *diagnostic)
{
    if (index == 0U) {
        return true;
    }
    uint8_t descriptor[CHERRY_USB_STRING_DESCRIPTOR_MAX] = {0};
    size_t descriptor_length = 0U;
    if (!fetch_usb_string_descriptor(hport, index, language_id,
                                     descriptor, &descriptor_length, field,
                                     diagnostic)) {
        return false;
    }
    if (!usb_string_utf16le_to_profile(descriptor, descriptor_length, destination)) {
        set_hid_init_diagnostic(diagnostic, "profile_string_decode", field,
                                ESP_ERR_INVALID_SIZE, true, descriptor_length);
        return false;
    }
    return true;
}

static bool populate_profile_strings(
    struct usbh_hubport *hport,
    cherry_hid_init_diagnostic_t *diagnostic)
{
    if (hport == NULL) {
        set_hid_init_diagnostic(diagnostic, "profile_strings_input", "device",
                                ESP_ERR_INVALID_ARG, false, 0U);
        return false;
    }
    const uint8_t manufacturer_index = hport->device_desc.iManufacturer;
    const uint8_t product_index = hport->device_desc.iProduct;
    const uint8_t serial_index = hport->device_desc.iSerialNumber;
    if (manufacturer_index == 0U && product_index == 0U && serial_index == 0U) {
        return true;
    }

    uint16_t language_id = 0U;
    if (!fetch_usb_string_language_id(hport, &language_id, diagnostic)) {
        return false;
    }
    return populate_profile_string(hport, manufacturer_index, language_id,
                                   &s_profile_cache.manufacturer, "manufacturer",
                                   diagnostic) &&
        populate_profile_string(hport, product_index, language_id,
                                &s_profile_cache.product, "product", diagnostic) &&
        populate_profile_string(hport, serial_index, language_id,
                                &s_profile_cache.serial, "serial", diagnostic);
}

static bool build_profile_base(
    struct usbh_hubport *hport,
    cherry_hid_init_diagnostic_t *diagnostic)
{
    if (hport == NULL || hport->raw_config_desc == NULL) {
        set_hid_init_diagnostic(diagnostic, "profile_base_input", "configuration",
                                ESP_ERR_INVALID_ARG, false, 0U);
        return false;
    }
    const size_t config_length = hport->config.config_desc.wTotalLength;
    if (config_length < USB_SIZEOF_CONFIG_DESC ||
        config_length > HID_PROFILE_MAX_CONFIG_DESCRIPTOR) {
        set_hid_init_diagnostic(diagnostic, "profile_config_length_validate", "configuration",
                                ESP_ERR_INVALID_SIZE, true, config_length);
        return false;
    }

    hid_device_profile_init(&s_profile_cache);
    if (!hid_device_profile_set_device_descriptor(
            &s_profile_cache, (const uint8_t *)&hport->device_desc,
            USB_SIZEOF_DEVICE_DESC) ||
        !hid_device_profile_set_config_descriptor(
            &s_profile_cache, hport->raw_config_desc, config_length)) {
        set_hid_init_diagnostic(diagnostic, "profile_descriptor_copy", "device_or_configuration",
                                ESP_ERR_INVALID_SIZE, true, config_length);
        return false;
    }
    if (!populate_profile_strings(hport, diagnostic)) {
        return false;
    }

    bool seen[256] = {false};
    uint8_t expected = 0U;
    size_t offset = 0U;
    while (offset + 2U <= config_length) {
        const uint8_t descriptor_length = hport->raw_config_desc[offset];
        const uint8_t descriptor_type = hport->raw_config_desc[offset + 1U];
        if (descriptor_length < 2U || offset + descriptor_length > config_length) {
            set_hid_init_diagnostic(diagnostic, "profile_configuration_walk", "configuration",
                                    ESP_ERR_INVALID_SIZE, true, offset);
            return false;
        }
        if (descriptor_type == USB_DESCRIPTOR_TYPE_INTERFACE && descriptor_length >= 9U &&
            hport->raw_config_desc[offset + 5U] == USB_DEVICE_CLASS_HID) {
            const uint8_t interface_number = hport->raw_config_desc[offset + 2U];
            if (!seen[interface_number]) {
                seen[interface_number] = true;
                ++expected;
            }
        }
        offset += descriptor_length;
    }
    if (offset != config_length || expected == 0U ||
        expected > HID_PROFILE_MAX_REPORT_DESCRIPTORS) {
        set_hid_init_diagnostic(diagnostic, "profile_interface_count_validate", "configuration",
                                ESP_ERR_INVALID_SIZE, true, config_length);
        return false;
    }
    memset(s_profile_seen_interfaces, 0, sizeof(s_profile_seen_interfaces));
    s_profile_expected_interfaces = expected;
    s_profile_base_valid = true;
    s_profile_ready = false;
    return true;
}

static cherry_hid_state_t *allocate_hid_state(void)
{
    cherry_hid_state_t *result = NULL;
    portENTER_CRITICAL(&s_state_mux);
    for (size_t index = 0; index < HID_PROFILE_MAX_REPORT_DESCRIPTORS; ++index) {
        if (!s_hid_states[index].used) {
            result = &s_hid_states[index];
            memset(result, 0, sizeof(*result));
            result->used = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_mux);
    return result;
}

static void free_hid_state(cherry_hid_state_t *state)
{
    if (state == NULL) {
        return;
    }
    __atomic_store_n(&state->stopping, true, __ATOMIC_RELEASE);
    stop_state_timer(state);
    while (__atomic_load_n(&state->timer_callback_users, __ATOMIC_ACQUIRE) != 0U ||
           __atomic_load_n(&state->completion_users, __ATOMIC_ACQUIRE) != 0U ||
           __atomic_load_n(&state->urb_active, __ATOMIC_ACQUIRE)) {
        vTaskDelay(1U);
    }
    if (state->input_buffer != NULL) {
        heap_caps_free(state->input_buffer);
        state->input_buffer = NULL;
    }
    portENTER_CRITICAL(&s_state_mux);
    memset(state, 0, sizeof(*state));
    portEXIT_CRITICAL(&s_state_mux);
}

static void input_completion(void *argument, int nbytes)
{
    cherry_hid_state_t *state = argument;
    if (state == NULL) {
        return;
    }
    __atomic_add_fetch(&s_input_callback_users, 1U, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&state->completion_users, 1U, __ATOMIC_ACQ_REL);
    BaseType_t higher_priority_task_woken = pdFALSE;
    const bool from_isr = xPortInIsrContext();
    if (nbytes > 0) {
        __atomic_add_fetch(&s_input_reports, 1U, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_input_bytes, (uint32_t)nbytes, __ATOMIC_RELAXED);
        __atomic_store_n(&s_last_input_us, esp_timer_get_time(), __ATOMIC_RELEASE);
        if (!__atomic_load_n(&state->stopping, __ATOMIC_ACQUIRE)) {
            bool queued;
            if (from_isr) {
                queued = dual_hid_host_backend_enqueue_report_from_isr(
                    state->interface_number, state->generation, state->input_buffer,
                    (size_t)nbytes, &higher_priority_task_woken);
            } else {
                queued = dual_hid_host_backend_enqueue_report(
                    state->interface_number, state->generation, state->input_buffer,
                    (size_t)nbytes, false);
            }
            if (!queued) {
                __atomic_add_fetch(&s_input_errors, 1U, __ATOMIC_RELAXED);
                if (from_isr) {
                    dual_hid_host_backend_request_safety_release_from_isr(
                        &higher_priority_task_woken);
                } else {
                    dual_hid_host_backend_request_safety_release();
                }
            }
        }
    } else if (nbytes == 0) {
        __atomic_add_fetch(&s_input_zero_completions, 1U, __ATOMIC_RELAXED);
    } else if (nbytes == -USB_ERR_NAK) {
        __atomic_add_fetch(&s_input_nak_completions, 1U, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&s_input_errors, 1U, __ATOMIC_RELAXED);
        if (from_isr) {
            dual_hid_host_backend_request_safety_release_from_isr(
                &higher_priority_task_woken);
        } else {
            dual_hid_host_backend_request_safety_release();
        }
    }
    /* DMA 缓冲区复制并入队完成后，才允许下一次 URB 重用它。 */
    __atomic_store_n(&state->urb_active, false, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&state->completion_users, 1U, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&s_input_callback_users, 1U, __ATOMIC_RELEASE);
    if (from_isr && higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t acquire_hport_for_submit(cherry_hid_state_t *state,
                                          struct usbh_hubport **out_hport)
{
    if (state == NULL || out_hport == NULL || s_lifecycle_guard == NULL ||
        xSemaphoreTake(s_lifecycle_guard, 0U) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    struct usbh_hubport *hport = s_hport;
    const bool valid = !s_shutdown_requested && !s_device_stopping &&
        !state->stopping && hport != NULL &&
        hport == state->hport && hport->connected;
    if (valid) {
        __atomic_add_fetch(&s_guard_users, 1U, __ATOMIC_ACQ_REL);
        *out_hport = hport;
    }
    xSemaphoreGive(s_lifecycle_guard);
    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void input_timer_callback(void *argument)
{
    cherry_hid_state_t *state = argument;
    if (state == NULL) {
        return;
    }
    __atomic_add_fetch(&s_timer_callback_users, 1U, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&state->timer_callback_users, 1U, __ATOMIC_ACQ_REL);
    struct usbh_hubport *hport = NULL;
    if (state->hid_class == NULL || state->input_buffer == NULL ||
        __atomic_load_n(&state->stopping, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&state->urb_active, __ATOMIC_ACQUIRE) ||
        acquire_hport_for_submit(state, &hport) != ESP_OK) {
        goto done;
    }
    if (__atomic_load_n(&state->stopping, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&state->urb_active, __ATOMIC_ACQUIRE)) {
        release_hport_guard();
        goto done;
    }
    usbh_int_urb_fill(&state->hid_class->intin_urb, hport,
                      state->hid_class->intin, state->input_buffer,
                      state->endpoint_packet_size, 0U, input_completion, state);
    __atomic_store_n(&state->urb_active, true, __ATOMIC_RELEASE);
    const int result = usbh_submit_urb(&state->hid_class->intin_urb);
    if (result != 0) {
        __atomic_store_n(&state->urb_active, false, __ATOMIC_RELEASE);
        __atomic_add_fetch(&s_input_submit_errors, 1U, __ATOMIC_RELAXED);
        if (result != -USB_ERR_NAK && result != -USB_ERR_NOTCONN) {
            ESP_LOGW(TAG, "HID interrupt-IN提交失败：interface=%u result=%d",
                     state->interface_number, result);
        }
        if (result != -USB_ERR_NAK) {
            dual_hid_host_backend_request_safety_release();
        }
    }
    release_hport_guard();
done:
    __atomic_sub_fetch(&state->timer_callback_users, 1U, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&s_timer_callback_users, 1U, __ATOMIC_RELEASE);
}

static uint32_t interface_poll_period_us(const struct usbh_hid *hid_class)
{
    const uint32_t period = USBH_GET_URB_INTERVAL(
        hid_class->intin->bInterval, hid_class->hport->speed);
    return period == 0U ? CHERRY_REPORT_PERIOD_FALLBACK_US : period;
}

static bool publish_cached_profile_if_complete(struct usbh_hubport *hport)
{
    if (s_profile_guard == NULL || hport == NULL ||
        xSemaphoreTake(s_profile_guard, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool ready = s_profile_ready && s_profile_base_valid &&
        s_profile_cache.report_descriptor_count == s_profile_expected_interfaces;
    const esp_err_t result = ready
        ? dual_hid_host_backend_publish_profile(
              hport->dev_addr, &s_profile_cache,
              s_profile_expected_interfaces,
              s_profile_cache.report_descriptor_count)
        : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_profile_guard);
    if (result != ESP_OK && ready) {
        ESP_LOGW(TAG, "CherryUSB完整HID Profile发布失败：%s",
                 esp_err_to_name(result));
    }
    return result == ESP_OK;
}

void usbh_hid_run(struct usbh_hid *hid_class)
{
    const int64_t started_us = esp_timer_get_time();
    cherry_hid_init_diagnostic_t diagnostic = {0};
    if (hid_class == NULL || hid_class->hport == NULL) {
        set_hid_init_diagnostic(&diagnostic, "run_arguments", "hid_class",
                                ESP_ERR_INVALID_ARG, false, 0U);
        log_hid_init_failure(hid_class != NULL ? hid_class->hport : NULL,
                             hid_class, UINT8_MAX, 0U, 0U, started_us,
                             &diagnostic);
        return;
    }
    struct usbh_hubport *hport = NULL;
    esp_err_t result = register_hport_and_acquire(hid_class->hport, &hport);
    if (result != ESP_OK) {
        set_hid_init_diagnostic(&diagnostic, "lifecycle_register", "device",
                                result, false, 0U);
        log_hid_init_failure(hid_class->hport, hid_class, UINT8_MAX,
                             hid_class->report_size, 0U, started_us,
                             &diagnostic);
        return;
    }

    const struct usb_interface_descriptor *intf_desc =
        &hport->config.intf[hid_class->intf].altsetting[0].intf_desc;
    const uint8_t interface_number = intf_desc->bInterfaceNumber;
    cherry_hid_state_t *state = allocate_hid_state();
    if (state == NULL) {
        release_hport_guard();
        set_hid_init_diagnostic(&diagnostic, "state_alloc", "hid_state",
                                ESP_ERR_NO_MEM, false, 0U);
        log_hid_init_failure(hport, hid_class, interface_number,
                             hid_class->report_size, 0U, started_us,
                             &diagnostic);
        return;
    }
    state->hid_class = hid_class;
    state->hport = hport;
    state->interface_number = intf_desc->bInterfaceNumber;
    state->subclass = intf_desc->bInterfaceSubClass;
    state->protocol = intf_desc->bInterfaceProtocol;
    state->generation = ++s_generation;
    if (state->generation == 0U) {
        state->generation = ++s_generation;
    }
    hid_class->user_data = state;

    if (hid_class->report_size == 0U ||
        hid_class->report_size > HID_PROFILE_MAX_REPORT_DESCRIPTOR) {
        result = ESP_ERR_INVALID_SIZE;
        set_hid_init_diagnostic(&diagnostic, "report_size_validate", "report_descriptor",
                                result, false, 0U);
        goto fail;
    }
    size_t descriptor_length = 0U;
    cherry_control_stage_t control_stage = CHERRY_CONTROL_STAGE_VALIDATION;
    cherry_control_trace_t control_trace = {0};
    const int64_t descriptor_deadline = esp_timer_get_time() +
        (int64_t)CHERRY_HEARTBEAT_TIMEOUT_MS * 1000LL;
    result = control_transfer_guarded(
        hport, USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_INTERFACE,
        USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)((HID_DESCRIPTOR_TYPE_HID_REPORT << 8U)),
        state->interface_number, hid_class->report_size,
        NULL, s_report_descriptor_buffer, sizeof(s_report_descriptor_buffer),
        &descriptor_length, descriptor_deadline, &control_stage, &control_trace);
    if (result != ESP_OK) {
        set_hid_control_diagnostic(&diagnostic, "full_descriptor_fetch",
                                   "report_descriptor", result, control_stage,
                                   &control_trace);
        goto fail;
    }
    if (descriptor_length != hid_class->report_size) {
        result = ESP_ERR_INVALID_SIZE;
        set_hid_control_diagnostic(&diagnostic, "full_descriptor_length_validate",
                                   "report_descriptor", result,
                                   CHERRY_CONTROL_STAGE_VALIDATION, &control_trace);
        goto fail;
    }

    if (hid_class->intin != NULL) {
        const uint16_t max_packet = USB_GET_MAXPACKETSIZE(
            hid_class->intin->wMaxPacketSize);
        if (max_packet == 0U || max_packet > CHERRY_MAX_REPORT_PACKET) {
            result = ESP_ERR_INVALID_SIZE;
            set_hid_init_diagnostic(&diagnostic, "endpoint_packet_validate",
                                    "interrupt_in", result, false, 0U);
            goto fail;
        }
        state->endpoint_packet_size = max_packet;
        state->input_buffer = heap_caps_aligned_alloc(
            CONFIG_USB_ALIGN_SIZE,
            USB_ALIGN_UP(max_packet, CONFIG_USB_ALIGN_SIZE),
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (state->input_buffer == NULL) {
            result = ESP_ERR_NO_MEM;
            set_hid_init_diagnostic(&diagnostic, "input_buffer_alloc",
                                    "interrupt_in", result, false, 0U);
            goto fail;
        }
        esp_timer_create_args_t timer_config = {
            .callback = input_timer_callback,
            .arg = state,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "cherry_hid_in",
            .skip_unhandled_events = true,
        };
        result = esp_timer_create(&timer_config, &state->timer);
        if (result != ESP_OK) {
            set_hid_init_diagnostic(&diagnostic, "input_timer_create",
                                    "interrupt_in", result, false, 0U);
            goto fail;
        }
    }

    result = dual_hid_host_backend_interface_open(
        state->interface_number, state->subclass, state->protocol,
        s_report_descriptor_buffer, descriptor_length, state->generation);
    if (result != ESP_OK) {
        set_hid_init_diagnostic(&diagnostic, "interface_open",
                                "report_descriptor", result, true,
                                descriptor_length);
        goto fail;
    }
    state->interface_open = true;
    state->report_started = true;

    if (s_profile_guard == NULL ||
        xSemaphoreTake(s_profile_guard, portMAX_DELAY) != pdTRUE) {
        result = ESP_ERR_INVALID_STATE;
        set_hid_init_diagnostic(&diagnostic, "profile_lock", "profile_cache",
                                result, false, 0U);
        goto fail;
    }
    if (!s_profile_base_valid && !build_profile_base(hport, &diagnostic)) {
        s_profile_base_valid = false;
        result = ESP_ERR_INVALID_SIZE;
    } else if (s_profile_seen_interfaces[state->interface_number]) {
        result = ESP_ERR_INVALID_SIZE;
        set_hid_init_diagnostic(&diagnostic, "profile_add", "duplicate_interface",
                                result, true, descriptor_length);
    } else if (!hid_device_profile_add_report_descriptor(
                   &s_profile_cache, state->interface_number, state->subclass,
                   state->protocol, s_report_descriptor_buffer, descriptor_length)) {
        result = ESP_ERR_INVALID_SIZE;
        set_hid_init_diagnostic(&diagnostic, "profile_add", "report_descriptor",
                                result, true, descriptor_length);
    } else {
        s_profile_seen_interfaces[state->interface_number] = true;
        s_profile_ready = s_profile_cache.report_descriptor_count ==
            s_profile_expected_interfaces;
    }
    xSemaphoreGive(s_profile_guard);
    if (result != ESP_OK) {
        goto fail;
    }

    if (state->timer != NULL) {
        result = esp_timer_start_periodic(
            state->timer, interface_poll_period_us(hid_class));
        if (result != ESP_OK) {
            set_hid_init_diagnostic(&diagnostic, "input_timer_start",
                                    "interrupt_in", result, false, 0U);
            goto fail;
        }
    }
    if (s_profile_ready) {
        (void)publish_cached_profile_if_complete(hport);
    }
    ESP_LOGI(TAG, "CherryUSB HID接口已启动：interface=%u protocol=%u input=%s report_bytes=%u",
             state->interface_number, state->protocol,
             state->timer != NULL ? "yes" : "no", hid_class->report_size);
    release_hport_guard();
    return;

fail:
    if (state->interface_open) {
        dual_hid_host_backend_interface_close(
            state->interface_number, state->generation, false);
        state->interface_open = false;
    }
    __atomic_store_n(&state->stopping, true, __ATOMIC_RELEASE);
    log_hid_init_failure(hport, hid_class, interface_number,
                         hid_class->report_size, state->endpoint_packet_size,
                         started_us, &diagnostic);
    free_hid_state(state);
    hid_class->user_data = NULL;
    release_hport_guard();
}

void usbh_hid_stop(struct usbh_hid *hid_class)
{
    cherry_hid_state_t *state = hid_class != NULL
        ? (cherry_hid_state_t *)hid_class->user_data : NULL;
    struct usbh_hubport *hport = state != NULL
        ? state->hport : (hid_class != NULL ? hid_class->hport : NULL);
    if (s_lifecycle_guard == NULL) {
        return;
    }
    bool first_device_stop = false;
    if (xSemaphoreTake(s_lifecycle_guard, portMAX_DELAY) == pdTRUE) {
        if (hport != NULL && s_hport == hport) {
            s_hport = NULL;
            s_device_stopping = true;
            first_device_stop = true;
            portENTER_CRITICAL(&s_state_mux);
            for (size_t index = 0; index < HID_PROFILE_MAX_REPORT_DESCRIPTORS; ++index) {
                if (s_hid_states[index].used) {
                    __atomic_store_n(&s_hid_states[index].stopping, true,
                                     __ATOMIC_RELEASE);
                }
            }
            portEXIT_CRITICAL(&s_state_mux);
        } else if (state != NULL) {
            __atomic_store_n(&state->stopping, true, __ATOMIC_RELEASE);
        }
        xSemaphoreGive(s_lifecycle_guard);
    }
    if (first_device_stop || s_shutdown_requested) {
        stop_all_state_timers();
    } else {
        stop_state_timer(state);
    }
    wait_for_hport_users_and_callbacks();
    if (first_device_stop || s_shutdown_requested) {
        kill_active_input_urbs();
    } else if (state != NULL && state->hid_class != NULL &&
               __atomic_load_n(&state->urb_active, __ATOMIC_ACQUIRE)) {
        (void)usbh_kill_urb(&state->hid_class->intin_urb);
    }
    wait_for_input_urbs_and_callbacks();
    if (first_device_stop) {
        reset_profile_for_device_epoch();
    }
    if (state != NULL && state->interface_open) {
        dual_hid_host_backend_interface_close(
            state->interface_number, state->generation,
            hport == NULL || !hport->connected || s_device_stopping ||
                s_shutdown_requested);
        state->interface_open = false;
    }
    if (state != NULL) {
        free_hid_state(state);
        if (hid_class != NULL) {
            hid_class->user_data = NULL;
        }
    }
}


static const char *format_u32_or_unknown(bool valid, uint32_t value,
                                         char *buffer, size_t capacity)
{
    if (!valid) {
        return "unknown";
    }
    (void)snprintf(buffer, capacity, "%" PRIu32, value);
    return buffer;
}

static const char *format_i32_or_unknown(bool valid, int32_t value,
                                         char *buffer, size_t capacity)
{
    if (!valid) {
        return "unknown";
    }
    (void)snprintf(buffer, capacity, "%" PRId32, value);
    return buffer;
}

static const char *format_bool_or_unknown(bool valid, bool value)
{
    if (!valid) {
        return "unknown";
    }
    return value ? "1" : "0";
}

/* 只在后台心跳任务、全部 USB 锁和生命周期引用释放后输出。 */
static void log_heartbeat_failure(const cherry_heartbeat_failure_t *f, bool replay)
{
    char raw_result[16];
    char actual_length[16];
    char payload_length[16];
    char descriptor_header[16];
    char lifecycle_wait[16];
    char ep0_wait[16];
    char urb_time[16];
    char device_epoch[16];
    char input_age[16];
    if (f->descriptor_header_valid) {
        (void)snprintf(descriptor_header, sizeof(descriptor_header), "%u/%u",
                       f->descriptor_length, f->descriptor_type);
    } else {
        (void)snprintf(descriptor_header, sizeof(descriptor_header), "unknown");
    }
    ESP_LOGW(TAG, "HB_DIAG replay=%u seq=%" PRIu32 " uptime_us=%" PRId64
        " stage=%s esp=%" PRId32 " raw_urb=%s actual=%s payload=%s"
        " descriptor=%s total_us=%" PRIu32
        " lifecycle_wait_us=%s ep0_wait_us=%s urb_us=%s"
        " device_epoch=%s connected=%s stopping=%s input=%" PRIu32 " bytes=%" PRIu32
        " input_age_ms=%s transfer_err=%" PRIu32
        " submit_err=%" PRIu32 " streak=%" PRIu32 " streak_us=%" PRId64,
        replay, f->sequence, f->uptime_us, control_stage_name(f->stage), f->esp_error,
        format_i32_or_unknown(f->raw_urb_result_valid, f->raw_urb_result,
                              raw_result, sizeof(raw_result)),
        format_u32_or_unknown(f->raw_actual_length_valid, f->raw_actual_length,
                              actual_length, sizeof(actual_length)),
        format_u32_or_unknown(f->payload_length_valid, f->payload_length,
                              payload_length, sizeof(payload_length)),
        descriptor_header, f->total_latency_us,
        format_u32_or_unknown(f->lifecycle_wait_valid, f->lifecycle_wait_us,
                              lifecycle_wait, sizeof(lifecycle_wait)),
        format_u32_or_unknown(f->ep0_wait_valid, f->ep0_wait_us,
                              ep0_wait, sizeof(ep0_wait)),
        format_u32_or_unknown(f->urb_time_valid, f->urb_time_us,
                              urb_time, sizeof(urb_time)),
        format_u32_or_unknown(f->device_state_valid, f->device_epoch,
                              device_epoch, sizeof(device_epoch)),
        format_bool_or_unknown(f->device_state_valid, f->connected),
        format_bool_or_unknown(f->device_state_valid, f->stopping),
        f->input_reports, f->input_bytes,
        format_u32_or_unknown(f->input_age_valid, f->input_age_ms,
                              input_age, sizeof(input_age)),
        f->transfer_errors, f->submit_errors, f->consecutive_failures, f->streak_duration_us);
}

static void heartbeat_task(void *argument)
{
    (void)argument;
    uint32_t elapsed = 0U;
    while (!s_shutdown_requested) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHERRY_HEARTBEAT_PERIOD_MS));
        if (s_shutdown_requested) { break; }
        uint8_t descriptor[USB_SIZEOF_DEVICE_DESC] = {0};
        const int64_t started_us = esp_timer_get_time();
        const uint32_t sequence = cherry_heartbeat_next_sequence(&s_heartbeat_diagnostics);
        size_t descriptor_length = 0U;
        cherry_control_trace_t trace = {0};
        cherry_control_stage_t stage = CHERRY_CONTROL_STAGE_OK;
        esp_err_t result = control_transfer(
            false, true, 0U, 0U, 0U,
            USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE,
            USB_REQUEST_GET_DESCRIPTOR,
            (uint16_t)(USB_DESCRIPTOR_TYPE_DEVICE << 8U), 0U,
            USB_SIZEOF_DEVICE_DESC, CHERRY_HEARTBEAT_TIMEOUT_MS,
            descriptor, sizeof(descriptor), &descriptor_length, &stage, &trace);
        const int64_t ended_us = esp_timer_get_time();
        const uint32_t latency_us = (uint32_t)(ended_us - started_us);
        if (result == ESP_OK && (descriptor_length != USB_SIZEOF_DEVICE_DESC ||
            descriptor[0] != USB_SIZEOF_DEVICE_DESC || descriptor[1] != USB_DESCRIPTOR_TYPE_DEVICE)) {
            result = ESP_ERR_INVALID_RESPONSE;
            stage = CHERRY_CONTROL_STAGE_VALIDATION;
        }
        s_heartbeat_last_result = result;
        s_heartbeat_last_stage = stage;
        s_heartbeat_last_latency_us = latency_us;
        const int64_t last_input_us = __atomic_load_n(&s_last_input_us, __ATOMIC_ACQUIRE);
        const bool input_age_valid = last_input_us != 0 && last_input_us <= ended_us;
        const uint32_t input_age_ms = input_age_valid
            ? (uint32_t)((ended_us - last_input_us) / 1000LL) : 0U;
        if (result == ESP_OK) {
            cherry_heartbeat_recovery_t recovery;
            if (cherry_heartbeat_record_success(&s_heartbeat_diagnostics, sequence, ended_us, &recovery)) {
                ESP_LOGI(TAG, "HB_RECOVER seq=%" PRIu32 " uptime_us=%" PRId64
                    " streak=%" PRIu32 " streak_us=%" PRId64,
                    recovery.sequence, ended_us, recovery.consecutive_failures, recovery.streak_duration_us);
            }
        } else {
            cherry_heartbeat_failure_t failure = {
                .sequence = sequence, .uptime_us = ended_us, .stage = stage,
                .esp_error = result, .total_latency_us = latency_us,
                .input_reports = __atomic_load_n(&s_input_reports, __ATOMIC_RELAXED),
                .input_bytes = __atomic_load_n(&s_input_bytes, __ATOMIC_RELAXED),
                .input_age_valid = input_age_valid, .input_age_ms = input_age_ms,
                .transfer_errors = __atomic_load_n(&s_input_errors, __ATOMIC_RELAXED),
                .submit_errors = __atomic_load_n(&s_input_submit_errors, __ATOMIC_RELAXED),
            };
#define COPY_TRACE(field) failure.field = trace.field
            COPY_TRACE(raw_urb_result_valid); COPY_TRACE(raw_urb_result);
            COPY_TRACE(raw_actual_length_valid); COPY_TRACE(raw_actual_length);
            COPY_TRACE(payload_length_valid); COPY_TRACE(payload_length);
            COPY_TRACE(descriptor_header_valid); COPY_TRACE(descriptor_length); COPY_TRACE(descriptor_type);
            COPY_TRACE(lifecycle_wait_valid); COPY_TRACE(lifecycle_wait_us);
            COPY_TRACE(ep0_wait_valid); COPY_TRACE(ep0_wait_us);
            COPY_TRACE(urb_time_valid); COPY_TRACE(urb_time_us);
            COPY_TRACE(device_state_valid); COPY_TRACE(device_epoch); COPY_TRACE(connected); COPY_TRACE(stopping);
#undef COPY_TRACE
            const cherry_heartbeat_outcome_t outcome = cherry_heartbeat_classify_outcome(
                stage, trace.device_state_valid, trace.connected, trace.stopping);
            cherry_heartbeat_record_failure(&s_heartbeat_diagnostics, failure, outcome);
            log_heartbeat_failure(cherry_heartbeat_history_at(&s_heartbeat_diagnostics,
                s_heartbeat_diagnostics.history_count - 1U), false);
        }
        if (++elapsed >= CHERRY_SUMMARY_PERIOD) {
            elapsed = 0U;
            const cherry_heartbeat_diagnostics_t *d = &s_heartbeat_diagnostics;
            char summary_input_age[16];
            ESP_LOGI(TAG, "CherryUSB摘要：input=%" PRIu32 " bytes=%" PRIu32
                " nak=%" PRIu32 " transfer_err=%" PRIu32 " submit_err=%" PRIu32
                " hb_ok=%" PRIu32 " hb_fail=%" PRIu32 " hb_busy=%" PRIu32
                " hb_no_device=%" PRIu32 " hb_last=%s hb_stage=%s hb_us=%" PRIu32
                " input_age_ms=%s seq=%" PRIu32 " history=%u streak=%" PRIu32
                " recover=%" PRIu32 " lifecycle=%" PRIu32 " ep0=%" PRIu32
                " deadline=%" PRIu32 " urb=%" PRIu32 " validation=%" PRIu32
                " device=%" PRIu32 " other=%" PRIu32 " stack_hwm=%u",
                __atomic_load_n(&s_input_reports, __ATOMIC_RELAXED),
                __atomic_load_n(&s_input_bytes, __ATOMIC_RELAXED),
                __atomic_load_n(&s_input_nak_completions, __ATOMIC_RELAXED),
                __atomic_load_n(&s_input_errors, __ATOMIC_RELAXED),
                __atomic_load_n(&s_input_submit_errors, __ATOMIC_RELAXED),
                d->success_count, d->failure_count, d->busy_count, d->no_device_count,
                esp_err_to_name(s_heartbeat_last_result), control_stage_name(s_heartbeat_last_stage),
                s_heartbeat_last_latency_us,
                format_u32_or_unknown(input_age_valid, input_age_ms,
                                      summary_input_age, sizeof(summary_input_age)),
                sequence, d->history_count,
                d->consecutive_failures, d->recovery_count,
                d->stage_counts[CHERRY_CONTROL_STAGE_LIFECYCLE_LOCK], d->stage_counts[CHERRY_CONTROL_STAGE_EP0_LOCK],
                d->stage_counts[CHERRY_CONTROL_STAGE_DEADLINE], d->stage_counts[CHERRY_CONTROL_STAGE_URB],
                d->stage_counts[CHERRY_CONTROL_STAGE_VALIDATION], d->stage_counts[CHERRY_CONTROL_STAGE_DEVICE_STATE],
                d->stage_counts[CHERRY_CONTROL_STAGE_OTHER],
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        if (ended_us - s_last_heartbeat_history_replay_us >=
            CHERRY_HEARTBEAT_HISTORY_REPLAY_PERIOD_US) {
            s_last_heartbeat_history_replay_us = ended_us;
            for (size_t index = 0; index < s_heartbeat_diagnostics.history_count; ++index) {
                log_heartbeat_failure(cherry_heartbeat_history_at(&s_heartbeat_diagnostics, index), true);
            }
        }
    }
    s_heartbeat_task = NULL;
    if (s_task_events != NULL) { xEventGroupSetBits(s_task_events, CHERRY_HEARTBEAT_EXIT_BIT); }
    vTaskDelete(NULL);
}

esp_err_t dual_hid_host_cherryusb_start(void)
{
    if (s_initialized || s_lifecycle_guard != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lifecycle_guard = xSemaphoreCreateMutex();
    s_profile_guard = xSemaphoreCreateMutex();
    s_task_events = xEventGroupCreate();
    if (s_lifecycle_guard == NULL || s_profile_guard == NULL || s_task_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_shutdown_requested = false;
    s_device_stopping = false;
    s_guard_users = 0U;
    s_timer_callback_users = 0U;
    s_input_callback_users = 0U;
    s_generation = 0U;
    s_hport = NULL;
    memset(s_hid_states, 0, sizeof(s_hid_states));
    memset(&s_profile_cache, 0, sizeof(s_profile_cache));
    memset(s_profile_seen_interfaces, 0, sizeof(s_profile_seen_interfaces));
    s_profile_base_valid = false;
    s_profile_ready = false;
    s_profile_expected_interfaces = 0U;
    s_input_reports = 0U;
    s_input_bytes = 0U;
    s_input_zero_completions = 0U;
    s_input_nak_completions = 0U;
    s_input_errors = 0U;
    s_input_submit_errors = 0U;
    memset(&s_heartbeat_diagnostics, 0, sizeof(s_heartbeat_diagnostics));
    s_device_epoch = 0U;
    s_last_heartbeat_history_replay_us = esp_timer_get_time();
    s_heartbeat_last_result = ESP_ERR_NOT_FOUND;
    s_heartbeat_last_stage = CHERRY_CONTROL_STAGE_OK;
    s_heartbeat_last_latency_us = 0U;
    s_last_input_us = 0;

    if (usbh_initialize(CHERRY_BUS_ID, ESP_USB_FS0_BASE) != 0) {
        return ESP_FAIL;
    }
    s_initialized = true;
    if (xTaskCreate(heartbeat_task, "cherry_hid_heartbeat", 4096, NULL, 2,
                    &s_heartbeat_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "CherryUSB FS HID Host已启动；未启用USB自动恢复");
    return ESP_OK;
}

esp_err_t dual_hid_host_cherryusb_stop(void)
{
    if (s_lifecycle_guard == NULL) {
        return ESP_OK;
    }
    s_shutdown_requested = true;
    if (s_lifecycle_guard != NULL &&
        xSemaphoreTake(s_lifecycle_guard, portMAX_DELAY) == pdTRUE) {
        s_hport = NULL;
        s_device_stopping = true;
        portENTER_CRITICAL(&s_state_mux);
        for (size_t index = 0; index < HID_PROFILE_MAX_REPORT_DESCRIPTORS; ++index) {
            if (s_hid_states[index].used) {
                __atomic_store_n(&s_hid_states[index].stopping, true,
                                 __ATOMIC_RELEASE);
            }
        }
        portEXIT_CRITICAL(&s_state_mux);
        xSemaphoreGive(s_lifecycle_guard);
    }
    stop_all_state_timers();
    if (s_heartbeat_task != NULL) {
        xTaskNotifyGive(s_heartbeat_task);
        const EventBits_t bits = xEventGroupWaitBits(
            s_task_events, CHERRY_HEARTBEAT_EXIT_BIT, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(CHERRY_STOP_TIMEOUT_MS));
        if ((bits & CHERRY_HEARTBEAT_EXIT_BIT) == 0U) {
            return ESP_ERR_TIMEOUT;
        }
    }
    wait_for_hport_users_and_callbacks();
    kill_active_input_urbs();
    wait_for_input_urbs_and_callbacks();
    if (s_initialized) {
        const int result = usbh_deinitialize(CHERRY_BUS_ID);
        if (result != 0) {
            return ESP_FAIL;
        }
        s_initialized = false;
    }
    reset_profile_for_device_epoch();
    for (size_t index = 0; index < HID_PROFILE_MAX_REPORT_DESCRIPTORS; ++index) {
        cherry_hid_state_t *state = &s_hid_states[index];
        if (state->used) {
            if (state->interface_open) {
                dual_hid_host_backend_interface_close(
                    state->interface_number, state->generation, true);
                state->interface_open = false;
            }
            free_hid_state(state);
        }
    }
    if (s_task_events != NULL) {
        vEventGroupDelete(s_task_events);
        s_task_events = NULL;
    }
    if (s_profile_guard != NULL) {
        vSemaphoreDelete(s_profile_guard);
        s_profile_guard = NULL;
    }
    if (s_lifecycle_guard != NULL) {
        vSemaphoreDelete(s_lifecycle_guard);
        s_lifecycle_guard = NULL;
    }
    ESP_LOGI(TAG, "CherryUSB Host已停止并释放生命周期引用");
    return ESP_OK;
}

esp_err_t dual_hid_host_cherryusb_refresh_profile(void)
{
    struct usbh_hubport *hport = NULL;
    const esp_err_t acquired = acquire_registered_hport(
        NULL, CHERRY_CONTROL_BUSY_TIMEOUT_MS, &hport, NULL, NULL, NULL);
    if (acquired != ESP_OK) {
        return acquired;
    }
    if (s_profile_guard == NULL ||
        xSemaphoreTake(s_profile_guard, pdMS_TO_TICKS(CHERRY_CONTROL_BUSY_TIMEOUT_MS)) != pdTRUE) {
        release_hport_guard();
        return ESP_ERR_TIMEOUT;
    }
    const bool ready = s_profile_ready && s_profile_base_valid && hport != NULL;
    const esp_err_t result = ready
        ? dual_hid_host_backend_publish_profile(
              hport->dev_addr, &s_profile_cache,
              s_profile_expected_interfaces,
              s_profile_cache.report_descriptor_count)
        : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_profile_guard);
    release_hport_guard();
    return result;
}
