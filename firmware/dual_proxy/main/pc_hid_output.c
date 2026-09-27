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
#include "hid_vendor_session_logic.h"
#include "link_recovery_logic.h"
#include "mouse_motion_smoother.h"
#include "diag_stream.h"
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
#define VENDOR_CONTROL_QUEUE_LENGTH 128
#define VENDOR_CLONE_READY_TIMEOUT_MS 5000U
/*
 * 握手保护：接过设备后 G HUB 会连续做 SET/GET_REPORT 握手，这段窗口里若被
 * 1 kHz 移动报文灌满共享的厂商输入队列，握手报文会被丢弃 → “连上后立刻快速
 * 移动就识别失败 / 显示连接异常”（用户实测：等识别完再移动就没问题）。
 * 因此移动报文在握手窗口内可丢，厂商报文不可丢。
 */
#define CLONE_HANDSHAKE_GRACE_US 2500000LL
#define VENDOR_BUSY_WINDOW_US 400000LL
/* 厂商输入队列里给移动报文保留的上限，其余槽位留给厂商/握手报文。 */
#define VENDOR_MOTION_QUEUE_CAP 96U
/*
 * 移动专用队列：与厂商队列同为 128 槽。
 * 先用过 8 槽 + “满了清空只留最新”，实测丢得太多、手感明显变差，按用户要求
 * 改回 128 槽（宁可容忍少量积压延迟，也不要把移动采样丢掉）。
 * 消费端仍保持“厂商优先”，避免握手/查询排在移动后面。
 */
#define MOTION_INPUT_QUEUE_LENGTH 128U
/* 一次最多合并多少条积压移动（32 条约 32 ms 的量）。 */
#define MOTION_MERGE_MAX_ITEMS 32U
/* 移动报文在应用层停留时间的峰值（到达→提交），单位微秒。 */
static volatile int64_t s_motion_latency_peak_us;
static volatile uint32_t s_motion_queue_peak;
/* 非纯移动（含按键）报文在应用层的停留峰值，以及"等克隆就绪"循环的等待统计。
 * 这段路径此前完全没有观测：它在克隆未就绪时最长可等 5 秒。 */
static volatile int64_t s_vendor_item_latency_peak_us;
static volatile uint32_t s_vendor_ready_wait_count;
static volatile int64_t s_vendor_ready_wait_total_us;
static volatile uint32_t s_motion_merged_total;
static QueueHandle_t s_motion_input_queue;
static volatile int64_t s_clone_mount_us;
static volatile int64_t s_vendor_busy_us;
/*
 * 只有“密集突发”才值得让路：G HUB 初始化时 SET/GET 间隔在微秒级（日志实测
 * 18707/18717/18727 µs，间隔 10 µs），而它正常轮询的间隔是几十毫秒。
 * 早期实现对“最后一次厂商请求后 400 ms”一律让路，结果正常轮询把窗口不断续上，
 * 移动被持续丢弃（实测 motion_skipped=8675）→ 指针延迟巨大。
 */
#define VENDOR_BURST_GAP_US 5000LL
#define VENDOR_BURST_MIN_COUNT 5U
#define VENDOR_BURST_HOLD_US 50000LL
static volatile int64_t s_vendor_last_request_us;
static volatile uint32_t s_vendor_burst_count;
static volatile int64_t s_vendor_burst_last_us;

static void vendor_note_request(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t previous = s_vendor_last_request_us;
    s_vendor_last_request_us = now;
    if (previous != 0 && now - previous < VENDOR_BURST_GAP_US) {
        if (s_vendor_burst_count < 100000U) {
            ++s_vendor_burst_count;
        }
    } else {
        s_vendor_burst_count = 1U;
    }
    if (s_vendor_burst_count >= VENDOR_BURST_MIN_COUNT) {
        s_vendor_burst_last_us = now;
    }
}

static bool vendor_burst_active(void)
{
    return s_vendor_burst_last_us != 0 &&
        esp_timer_get_time() - s_vendor_burst_last_us < VENDOR_BURST_HOLD_US;
}
static volatile uint32_t s_vendor_motion_skipped;
#define VENDOR_INPUT_TASK_STACK 3072
#define VENDOR_CONTROL_TASK_STACK 3072
#define VENDOR_GET_REPORT_TIMEOUT_MS 250
#define USB_RECONFIGURE_EVENT_GUARD_MS 500U

static const char *TAG = "dual_pc_hid";

typedef struct {
    volatile uint32_t received;
    volatile uint32_t rejected;
    volatile uint32_t dropped;
    volatile uint32_t peak;
} queue_metrics_t;

static queue_metrics_t s_vendor_input_queue_metrics;
static queue_metrics_t s_motion_input_queue_metrics;
static queue_metrics_t s_vendor_control_queue_metrics;

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

static uint8_t diag_copy_bytes(uint8_t *target, size_t capacity,
                               const uint8_t *source, size_t length)
{
    if (length != 0U && (source == NULL || target == NULL)) {
        return 0U;
    }
    size_t copied = length < capacity ? length : capacity;
    if (copied != 0U) {
        memcpy(target, source, copied);
    }
    return (uint8_t)copied;
}

/* SET_REPORT: instance, interface, report ID, type, queued(0/1), original len, raw data. */
static void diag_capture_set_report(uint8_t instance, uint8_t interface_number,
                                    uint8_t report_id, uint8_t report_type,
                                    bool queued, const uint8_t *data, size_t length)
{
    uint8_t event[DUAL_DIAG_EVENT_DATA_MAX] = {0};
    event[0] = instance;
    event[1] = interface_number;
    event[2] = report_id;
    event[3] = report_type;
    event[4] = queued ? 1U : 0U;
    event[5] = (uint8_t)(length > UINT8_MAX ? UINT8_MAX : length);
    const uint8_t copied = diag_copy_bytes(&event[6], sizeof(event) - 6U,
                                           data, length);
    dual_diag_stream_record(DUAL_DIAG_SOURCE_P_USB, DUAL_DIAG_KIND_P_SET_REPORT,
                            event, (uint8_t)(6U + copied));
}

/* GET_RESULT: instance, interface, report ID, type, status, returned len, response bytes. */
static void diag_capture_get_result(uint8_t instance, uint8_t interface_number,
                                    uint8_t report_id, uint8_t report_type,
                                    uint8_t status, const uint8_t *data,
                                    uint16_t length)
{
    uint8_t event[DUAL_DIAG_EVENT_DATA_MAX] = {0};
    event[0] = instance;
    event[1] = interface_number;
    event[2] = report_id;
    event[3] = report_type;
    event[4] = status;
    const uint8_t copied = diag_copy_bytes(&event[6], sizeof(event) - 6U,
                                           data, length);
    event[5] = copied;
    dual_diag_stream_record(DUAL_DIAG_SOURCE_P_USB,
                            DUAL_DIAG_KIND_P_GET_REPORT_RESULT,
                            event, (uint8_t)(6U + copied));
}

/* Submit result: accepted by TinyUSB, instance, report ID, original len, raw report bytes. */
static void diag_capture_report_submit(bool submitted, uint8_t instance,
                                       uint8_t report_id, const uint8_t *data,
                                       size_t length)
{
    uint8_t event[DUAL_DIAG_EVENT_DATA_MAX] = {0};
    event[0] = submitted ? 1U : 0U;
    event[1] = instance;
    event[2] = report_id;
    event[3] = (uint8_t)(length > UINT8_MAX ? UINT8_MAX : length);
    const uint8_t copied = diag_copy_bytes(&event[4], sizeof(event) - 4U,
                                           data, length);
    dual_diag_stream_record(DUAL_DIAG_SOURCE_P_USB,
                            DUAL_DIAG_KIND_P_USB_REPORT_SUBMIT,
                            event, (uint8_t)(4U + copied));
}

static void diag_capture_report_complete(uint8_t instance, const uint8_t *report,
                                         uint16_t length)
{
    uint8_t event[DUAL_DIAG_EVENT_DATA_MAX] = {0};
    event[0] = instance;
    event[1] = (uint8_t)(length > UINT8_MAX ? UINT8_MAX : length);
    const uint8_t copied = diag_copy_bytes(&event[2], sizeof(event) - 2U,
                                           report, length);
    dual_diag_stream_record(DUAL_DIAG_SOURCE_P_USB,
                            DUAL_DIAG_KIND_P_USB_REPORT_COMPLETE,
                            event, (uint8_t)(2U + copied));
}

static void diag_capture_report_failed(uint8_t instance, uint8_t report_type,
                                       const uint8_t *report,
                                       uint16_t transferred_length,
                                       uint16_t report_length)
{
    uint8_t event[DUAL_DIAG_EVENT_DATA_MAX] = {0};
    event[0] = instance;
    event[1] = report_type;
    memcpy(&event[2], &transferred_length, sizeof(transferred_length));
    event[4] = (uint8_t)(report_length > UINT8_MAX ? UINT8_MAX : report_length);
    const uint8_t copied = diag_copy_bytes(&event[5], sizeof(event) - 5U,
                                           report, report_length);
    dual_diag_stream_record(DUAL_DIAG_SOURCE_P_USB,
                            DUAL_DIAG_KIND_P_USB_REPORT_FAILED,
                            event, (uint8_t)(5U + copied));
}

static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_sender_stopped;
static SemaphoreHandle_t s_reconfigure_mutex;
static SemaphoreHandle_t s_vendor_session_mutex;
static TaskHandle_t s_sender_task;
static TaskHandle_t s_reconfigure_task;
static esp_timer_handle_t s_sender_timer;
static volatile bool s_sender_stop_requested;
static volatile bool s_reconfigure_enabled;
static volatile bool s_reconfigure_disconnect_requested;
static volatile bool s_reconfigure_profile_pending;
static uint32_t s_reconfigure_epoch = 1U;
static uint32_t s_reconfigure_profile_epoch;
static uint32_t s_reconfigure_disconnect_peer_generation;
static uint32_t s_reconfigure_disconnect_event_id;
static uint8_t s_reconfigure_disconnect_ack_type;
static uint32_t s_reconfigure_disconnect_ack_flow_id;
static bool s_reconfigure_allow_profile_request;
static uint32_t s_reconfigure_peer_generation;
static uint32_t s_reconfigure_failed_peer_generation;
static uint32_t s_reconfigure_last_gone_peer_generation;
static uint32_t s_reconfigure_last_gone_event_id;
static uint32_t s_reconfigure_last_offer_peer_generation;
static uint32_t s_reconfigure_last_offer_flow_id;
/* 清理的 USB 副作用是否已经完成：已完成时只允许补发确认帧。 */
static bool s_reconfigure_cleanup_done;
static uint8_t s_reconfigure_cleanup_attempts;
static uint8_t s_reconfigure_ack_attempts;
static uint32_t s_reconfigure_cleanup_retries;
static uint32_t s_reconfigure_ack_retries;
static uint32_t s_reconfigure_installed_transfer_id;
static uint32_t s_reconfigure_installed_crc32;
/* 同一 (type, flow) 的“已完成重复请求”只打印一次，避免刷屏。 */
static uint8_t s_reconfigure_dup_logged_type;
static uint32_t s_reconfigure_dup_logged_flow_id;
static bool s_reconfigure_dup_logged_valid;
static uint32_t s_reconfigure_duplicate_requests;
static bool s_reconfigure_disconnect_failed;
static dual_input_state_t s_state;
static volatile bool s_force_release;
static volatile bool s_installed;
static volatile bool s_pc_usb_attached;
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
static uint32_t s_reconfigure_transfer_id;
static uint32_t s_reconfigure_crc32;
static uint32_t s_work_transfer_id;
static uint32_t s_work_crc32;
static hid_device_profile_t s_active_profile;
static hid_clone_descriptor_set_t s_clone_descriptors;
static tusb_desc_device_t s_clone_device_descriptor;
static volatile bool s_clone_active;
static uint8_t s_clone_mouse_instance;
static hid_mouse_report_layout_t s_clone_mouse_layout;
/*
 * 「转发输出的位移」统计（2026-09-27）：M 转发的鼠标报告在 P 侧真正提交给主机时累加。
 * 注意与 HID统计 里的 output=(...) 的区别：那个只统计 P 自己生成的**软件输入**，
 * 不覆盖 M→P 转发路径——实测转发期间 output 一直是 (0,0)，所以必须单独统计这一路。
 * 与 M 侧 motion_rx_dx/dy 对比即可看出桥接层有没有吞掉/放大位移。
 */
static volatile int64_t s_motion_fwd_dx;
static volatile int64_t s_motion_fwd_dy;
/* 解析诊断（2026-09-27）：与 M 侧同源，定位两侧位移不一致的原因。 */
static volatile uint32_t s_motion_fwd_ok;
static volatile uint32_t s_motion_fwd_skip;

static void note_forwarded_motion(const uint8_t *payload, uint8_t length,
                                  uint8_t interface_number, uint8_t report_id)
{
    /* 判据必须**同时**满足两条，缺一不可（两轮实测得出）：
     *   ① 源接口号 == 鼠标接口号 —— 厂商接口的报告 report_id 与鼠标相同，只靠 ID 拦不住；
     *   ② report_id == 鼠标报告 ID —— 鼠标接口上还有滚轮等其它 report_id 的报告，
     *      只靠接口号会多统计约 19% 的帧（其字节被误当作 X/Y，位移偏高约 8%）。 */
    if (payload == NULL || length == 0U || !s_clone_mouse_layout.valid ||
        length != s_clone_mouse_layout.report_bytes ||
        report_id != s_clone_mouse_layout.report_id || !s_clone_active ||
        s_clone_mouse_instance >= s_clone_descriptors.hid_count ||
        s_clone_descriptors.hid_interface_numbers[s_clone_mouse_instance] !=
            interface_number) {
        __atomic_add_fetch(&s_motion_fwd_skip, 1U, __ATOMIC_RELAXED);
        return;
    }
    int32_t x = 0;
    int32_t y = 0;
    int32_t wheel = 0;
    int32_t pan = 0;
    if (hid_mouse_report_read_axes(payload, length, &s_clone_mouse_layout,
                                   &x, &y, &wheel, &pan)) {
        __atomic_add_fetch(&s_motion_fwd_dx, x, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_motion_fwd_dy, y, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_motion_fwd_ok, 1U, __ATOMIC_RELAXED);
        /* 样本级对比（2026-09-27）：前 10 帧连字节一起打出来，与 M 侧 M样本#k 逐帧对齐。 */
        const uint32_t sample_index =
            (uint32_t)__atomic_load_n(&s_motion_fwd_ok, __ATOMIC_RELAXED);
        if (sample_index <= 10U || (sample_index % 1000U) == 0U) {
            ESP_LOGI(TAG, "P样本#%u 长度=%u字节 布局=%u字节 dx=%d dy=%d",
                     (unsigned)sample_index, (unsigned)length,
                     (unsigned)s_clone_mouse_layout.report_bytes, (int)x, (int)y);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, length, ESP_LOG_INFO);
        }
    } else {
        __atomic_add_fetch(&s_motion_fwd_skip, 1U, __ATOMIC_RELAXED);
    }
}
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
    uint32_t session_generation;
    /* 入队时刻：用于测量"到达→提交"的应用层停留时间（延迟定位）。 */
    int64_t enqueued_us;
    uint8_t interface_number;
    uint8_t report_id;
    uint8_t length;
    uint8_t data[DUAL_HID_RAW_INPUT_MAX_DATA];
} vendor_input_item_t;

typedef struct {
    uint32_t session_generation;
    bool get_report;
    uint16_t transaction_id;
    uint8_t interface_number;
    uint8_t report_id;
    uint8_t report_type;
    uint8_t requested_length;
    uint8_t length;
    /*
     * 设备级 Vendor 控制请求（2026-09-27）：is_vendor_control 为 true 时上面的
     * interface/report 字段无意义，改由下面四个字段描述一笔任意 EP0 控制传输。
     * 它与厂商控制共用同一条队列（用户指令），转发到 M 侧后用通用 URB 发往物理设备。
     */
    bool is_vendor_control;
    uint8_t bm_request_type;
    uint8_t b_request;
    uint16_t w_value;
    uint16_t w_index;
    uint16_t w_length;
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
static uint32_t s_get_session_generation;
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
static volatile int64_t s_usb_reconfigure_guard_until_us;
static uint32_t s_vendor_session_generation = 1U;

_Static_assert(CONFIG_FREERTOS_HZ == DUAL_PROXY_REQUIRED_FREERTOS_HZ,
               "dual_proxy要求CONFIG_FREERTOS_HZ=1000");
_Static_assert(pdMS_TO_TICKS(1) == 1, "1ms必须正好折算为1 tick");
_Static_assert(DUAL_PROXY_HID_PERIOD_US == 1000U, "HID周期必须保持1000us");

static uint32_t vendor_session_generation(void)
{
    return __atomic_load_n(&s_vendor_session_generation, __ATOMIC_ACQUIRE);
}

static uint32_t advance_vendor_session_generation(void)
{
    uint32_t current = vendor_session_generation();
    while (true) {
        const uint32_t next = hid_vendor_session_advance(current);
        if (__atomic_compare_exchange_n(&s_vendor_session_generation, &current,
                                       next, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE)) {
            return next;
        }
    }
}

static void vendor_session_wait_idle(void)
{
    if (s_vendor_session_mutex != NULL) {
        xSemaphoreTake(s_vendor_session_mutex, portMAX_DELAY);
        xSemaphoreGive(s_vendor_session_mutex);
    }
}

static bool vendor_session_item_is_current(uint32_t item_generation)
{
    return hid_vendor_session_matches(item_generation, vendor_session_generation());
}

static bool vendor_session_lock_if_current(uint32_t item_generation)
{
    if (s_vendor_session_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_vendor_session_mutex, portMAX_DELAY);
    if (!vendor_session_item_is_current(item_generation)) {
        xSemaphoreGive(s_vendor_session_mutex);
        return false;
    }
    return true;
}

static uint32_t next_reconfigure_epoch_locked(void)
{
    ++s_reconfigure_epoch;
    if (s_reconfigure_epoch == 0U) {
        s_reconfigure_epoch = 1U;
    }
    return s_reconfigure_epoch;
}

static bool reconfigure_epoch_is_current(uint32_t epoch)
{
    bool current;
    xSemaphoreTake(s_reconfigure_mutex, portMAX_DELAY);
    current = dual_profile_operation_is_current(
        epoch, s_reconfigure_epoch, s_reconfigure_disconnect_requested);
    xSemaphoreGive(s_reconfigure_mutex);
    return current;
}

static void clear_clone_session_state(void)
{
    s_clone_active = false;
    s_clone_mouse_template_valid = false;
    s_clone_mouse_template_length = 0;
    memset(s_clone_mouse_template, 0, sizeof(s_clone_mouse_template));
    memset(&s_active_profile, 0, sizeof(s_active_profile));
    memset(&s_clone_descriptors, 0, sizeof(s_clone_descriptors));
    memset(&s_clone_device_descriptor, 0, sizeof(s_clone_device_descriptor));
    memset(&s_clone_mouse_layout, 0, sizeof(s_clone_mouse_layout));
    s_clone_mouse_instance = 0;
    memset(s_clone_manufacturer, 0, sizeof(s_clone_manufacturer));
    memset(s_clone_product, 0, sizeof(s_clone_product));
    memset(s_clone_serial, 0, sizeof(s_clone_serial));
    for (uint8_t index = 0; index < CLONE_STRING_COUNT; ++index) {
        s_clone_string_descriptors[index] = s_empty_string;
    }
    s_clone_string_descriptors[0] = s_clone_language;
    s_clone_string_count = 1U;
}

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
    if (s_installed) {
        ESP_LOGE(TAG, "拒绝在旧USB设备仍安装时改写克隆描述符");
        return false;
    }
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
        /*
         * 厂商队列优先但不等待（0 超时）：有厂商/握手报文就先处理它，没有就
         * 立刻取移动——不给移动白加延迟。两者都空时在移动队列上等 10 ms。
         */
        if (xQueueReceive(s_vendor_input_queue, &item, 0) != pdTRUE) {
            if (xQueueReceive(s_motion_input_queue, &item, pdMS_TO_TICKS(10)) != pdTRUE) {
                continue;
            }
        }
        if (item.length == 0U) {
            ++s_vendor_input_dropped;
            continue;
        }
        if (!vendor_session_item_is_current(item.session_generation)) {
            ++s_vendor_input_dropped;
            continue;
        }
        uint8_t instance = 0;
        bool clone_ready = false;
        /*
         * 队头阻塞是握手失败的主因：消费端是单条 FIFO，原来每条（包括移动报文）
         * 都要等克隆就绪，最长 5000 ms——握手期克隆正在安装，队头一条移动报文
         * 就能把整条队列卡住数秒，G HUB 的 GET 请求全排在后面超时。
         * 移动报文不需要等：没就绪就直接丢（等识别完再移动不会丢，见
         * CLONE_HANDSHAKE_GRACE_US 的注释）。
         */
        const bool motion_only = item.length > 0U &&
            item.report_id == s_clone_mouse_layout.report_id &&
            item.length == s_clone_mouse_layout.report_bytes &&
            item.data[0] == 0U;
        if (motion_only && item.enqueued_us != 0) {
            const int64_t residence = esp_timer_get_time() - item.enqueued_us;
            if (residence > s_motion_latency_peak_us) {
                s_motion_latency_peak_us = residence;
            }
        }
        const bool ready_now = !s_usb_reconfigure_in_progress && s_clone_active &&
            s_installed && tud_mounted() &&
            clone_instance_for_interface(item.interface_number, &instance);
        if (ready_now) {
            clone_ready = true;
        } else if (motion_only) {
            ++s_vendor_motion_skipped;
            continue;
        }
        const int64_t ready_wait_begin_us = esp_timer_get_time();
        for (uint32_t waited_ms = 0; !clone_ready && waited_ms < VENDOR_CLONE_READY_TIMEOUT_MS;
             ++waited_ms) {
            if (!vendor_session_item_is_current(item.session_generation)) {
                break;
            }
            if (!s_usb_reconfigure_in_progress && s_clone_active &&
                s_installed && tud_mounted() &&
                clone_instance_for_interface(item.interface_number, &instance)) {
                clone_ready = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!clone_ready) {
            const int64_t waited = esp_timer_get_time() - ready_wait_begin_us;
            if (waited > 1000) {
                ++s_vendor_ready_wait_count;
                s_vendor_ready_wait_total_us += waited;
            }
        }
        if (!clone_ready || !vendor_session_item_is_current(item.session_generation)) {
            ++s_vendor_input_dropped;
            continue;
        }
        if (instance == s_clone_mouse_instance &&
            item.report_id == s_clone_mouse_layout.report_id &&
            item.length == s_clone_mouse_layout.report_bytes &&
            s_state_mutex != NULL) {
            if (vendor_session_lock_if_current(item.session_generation)) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                memcpy(s_clone_mouse_template, item.data, item.length);
                s_clone_mouse_template_length = item.length;
                s_clone_mouse_template_valid = true;
                xSemaphoreGive(s_state_mutex);
                xSemaphoreGive(s_vendor_session_mutex);
            } else {
                ++s_vendor_input_dropped;
                continue;
            }
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
            if (!vendor_session_lock_if_current(item.session_generation)) {
                break;
            }
            const bool clone_ready_now = !s_usb_reconfigure_in_progress &&
                s_clone_active && s_installed && tud_mounted();
            if (clone_ready_now && tud_hid_n_ready(instance)) {
                const bool report_queued = tud_hid_n_report(
                    instance, item.report_id, item.data, item.length);
                diag_capture_report_submit(report_queued, instance,
                                           item.report_id, item.data, item.length);
                if (report_queued) {
                    /* 判据：源接口号 + 报告 ID（见 note_forwarded_motion 的说明）。 */
                    note_forwarded_motion(item.data, (uint8_t)item.length,
                                          item.interface_number, item.report_id);
                    submitted = true;
                    xSemaphoreGive(s_vendor_session_mutex);
                    break;
                }
            }
            xSemaphoreGive(s_vendor_session_mutex);
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
        if (!vendor_session_lock_if_current(item.session_generation)) {
            ++s_vendor_set_dropped;
            continue;
        }
        if (item.is_vendor_control) {
            /*
             * 设备级 Vendor 控制请求：不针对克隆的某个 HID 接口，所以跳过下面的
             * 克隆/接口校验，直接按原始 bmRequestType 转发给 M（用户 2026-09-27
             * 指令：与厂商控制同队列）。M 侧用通用 EP0 URB 发往物理设备。
             */
            if (dual_uart1_send_vendor_control_request(
                    item.transaction_id, item.bm_request_type, item.b_request,
                    item.w_value, item.w_index, item.w_length, item.data,
                    item.length) != ESP_OK) {
                ++s_vendor_set_dropped;
                clear_pending_get(true, DUAL_HID_REPORT_STATUS_TIMEOUT);
            }
            xSemaphoreGive(s_vendor_session_mutex);
            continue;
        }
        uint8_t instance = 0;
        if (s_usb_reconfigure_in_progress || !s_clone_active ||
            !s_installed || !tud_mounted() ||
            !clone_instance_for_interface(item.interface_number, &instance)) {
            xSemaphoreGive(s_vendor_session_mutex);
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
        xSemaphoreGive(s_vendor_session_mutex);
    }
}

static esp_err_t ensure_vendor_runtime(void)
{
    if (s_vendor_input_queue != NULL && s_vendor_control_queue != NULL &&
        s_get_gate != NULL && s_get_response_sem != NULL &&
        s_get_state_mutex != NULL && s_vendor_input_task != NULL &&
        s_vendor_control_task != NULL && s_vendor_session_mutex != NULL) {
        return ESP_OK;
    }

    if (s_vendor_session_mutex == NULL) {
        s_vendor_session_mutex = xSemaphoreCreateMutex();
    }
    s_vendor_input_queue = xQueueCreate(
        VENDOR_INPUT_QUEUE_LENGTH, sizeof(vendor_input_item_t));
    /* 移动报文走独立的 128 槽队列，消费端保持厂商控制与报告优先。 */
    s_motion_input_queue = xQueueCreate(MOTION_INPUT_QUEUE_LENGTH,
                                        sizeof(vendor_input_item_t));
    s_vendor_control_queue = xQueueCreate(
        VENDOR_CONTROL_QUEUE_LENGTH, sizeof(vendor_control_item_t));
    s_get_gate = xSemaphoreCreateBinary();
    s_get_response_sem = xSemaphoreCreateBinary();
    s_get_state_mutex = xSemaphoreCreateMutex();
    if (s_vendor_session_mutex == NULL ||
        s_vendor_input_queue == NULL || s_vendor_control_queue == NULL ||
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
 * 设备级 Vendor 控制请求转发（2026-09-27 实现）。
 *
 * 背景：G HUB 把接收器标记为 DEVIO，历史上怀疑它除了 HID++ 之外还依赖厂商自定义的
 * EP0 事务；此前这里一律返回 false，让 TinyUSB 明确 STALL，以免"假装成功"。
 * 现在按用户指令改为真正转发：请求进厂商控制队列 → M 侧用通用 EP0 URB 发往物理设备
 * → 响应经板间链路带回后在这里完成控制传输。
 *
 * 复用 GET_REPORT 的等待设施（s_get_gate / s_get_response_sem / s_get_* 状态），
 * 保证同一时刻只有一笔厂商事务在等响应，不必再引入一套状态机。
 *
 * v1 限制：只支持设备级/接口级请求，且仅覆盖「OUT 且 wLength==0」与「IN」两类；
 * 带数据的 OUT（wLength>0 且方向为 OUT）需要在 DATA 阶段取数据，本版明确 STALL。
 */
static uint8_t s_vendor_xfer_buffer[DUAL_VENDOR_CONTROL_MAX_DATA];

bool tud_vendor_control_xfer_cb(
    uint8_t rhport,
    uint8_t stage,
    tusb_control_request_t const *request)
{
    if (request == NULL) {
        return false;
    }
    if (stage != CONTROL_STAGE_SETUP) {
        /* DATA/STATUS 阶段由 tud_control_xfer()/tud_control_status() 驱动。 */
        return true;
    }
    uint8_t event[10] = {0};
    event[0] = stage;
    event[1] = request->bmRequestType;
    event[2] = request->bRequest;
    memcpy(&event[3], &request->wValue, sizeof(request->wValue));
    memcpy(&event[5], &request->wIndex, sizeof(request->wIndex));
    memcpy(&event[7], &request->wLength, sizeof(request->wLength));
    dual_diag_stream_record(DUAL_DIAG_SOURCE_P_USB,
                            DUAL_DIAG_KIND_P_VENDOR_SETUP, event, sizeof(event));
    ESP_LOGI(TAG,
             "PC VENDOR_CONTROL: bm=%02X request=%02X value=%04X index=%04X length=%u",
             request->bmRequestType, request->bRequest, request->wValue,
             request->wIndex, request->wLength);

    const uint8_t recipient = request->bmRequestType & 0x1FU;
    const bool is_in = (request->bmRequestType & 0x80U) != 0U;
    const uint16_t w_length = request->wLength;
    const bool forwardable = s_vendor_control_queue != NULL && s_get_gate != NULL &&
        s_get_response_sem != NULL && s_get_state_mutex != NULL &&
        !s_usb_reconfigure_in_progress && s_clone_active && s_installed &&
        tud_mounted() && recipient <= 1U &&
        w_length <= DUAL_VENDOR_CONTROL_MAX_DATA && (is_in || w_length == 0U);
    if (!forwardable) {
        event[9] = 0U;   /* 不可转发：保持原来的明确 STALL，不伪造成功。 */
        return false;
    }
    if (xSemaphoreTake(s_get_gate, 0) != pdTRUE) {
        return false;   /* 已有厂商事务在等响应，本笔不排队。 */
    }
    while (s_get_response_sem != NULL &&
           xSemaphoreTake(s_get_response_sem, 0) == pdTRUE) {
        /* 丢掉过期完成，避免误配到本笔事务。 */
    }
    const uint16_t transaction_id = next_vendor_transaction_id();
    const uint32_t request_generation = vendor_session_generation();
    xSemaphoreTake(s_get_state_mutex, portMAX_DELAY);
    s_get_inflight = true;
    s_get_response_ready = false;
    s_get_transaction_id = transaction_id;
    s_get_interface_number = UINT8_MAX;   /* vendor 请求按事务号匹配，不按接口 */
    s_get_report_id = 0U;
    s_get_status = DUAL_HID_REPORT_STATUS_TIMEOUT;
    s_get_response_length = 0;
    s_get_session_generation = request_generation;
    xSemaphoreGive(s_get_state_mutex);

    vendor_control_item_t item = {
        .session_generation = request_generation,
        .get_report = false,
        .transaction_id = transaction_id,
        .is_vendor_control = true,
        .bm_request_type = request->bmRequestType,
        .b_request = request->bRequest,
        .w_value = request->wValue,
        .w_index = request->wIndex,
        .w_length = w_length,
        .length = 0U,
    };
    if (xQueueSend(s_vendor_control_queue, &item, 0) != pdTRUE) {
        xSemaphoreTake(s_get_state_mutex, portMAX_DELAY);
        s_get_inflight = false;
        xSemaphoreGive(s_get_state_mutex);
        xSemaphoreGive(s_get_gate);
        return false;
    }
    const bool signaled = xSemaphoreTake(
        s_get_response_sem, pdMS_TO_TICKS(VENDOR_GET_REPORT_TIMEOUT_MS)) == pdTRUE;
    uint8_t status = DUAL_HID_REPORT_STATUS_TIMEOUT;
    uint8_t response_length = 0;
    xSemaphoreTake(s_get_state_mutex, portMAX_DELAY);
    if (signaled && s_get_response_ready && s_get_inflight &&
        s_get_transaction_id == transaction_id) {
        status = s_get_status;
        response_length = s_get_response_length;
        if (response_length > sizeof(s_vendor_xfer_buffer)) {
            response_length = sizeof(s_vendor_xfer_buffer);
        }
        if (response_length != 0U) {
            memcpy(s_vendor_xfer_buffer, s_get_response_data, response_length);
        }
    }
    s_get_inflight = false;
    s_get_response_ready = false;
    xSemaphoreGive(s_get_state_mutex);
    xSemaphoreGive(s_get_gate);

    if (status != DUAL_HID_REPORT_STATUS_OK) {
        ++s_vendor_get_timeouts;
        return false;   /* 失败/超时：STALL，让主机看到真实结果而不是假成功。 */
    }
    if (w_length == 0U) {
        return tud_control_status(rhport, request);
    }
    uint16_t send_length = response_length;
    if (send_length > w_length) {
        send_length = w_length;
    }
    return tud_control_xfer(rhport, request, s_vendor_xfer_buffer, send_length);
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t *buffer,
    uint16_t requested_length)
{
    vendor_note_request();
    queue_metric_increment(&s_vendor_control_queue_metrics.received);
    uint8_t request_event[6] = {0};
    request_event[0] = instance;
    request_event[1] = report_id;
    request_event[2] = (uint8_t)report_type;
    memcpy(&request_event[3], &requested_length, sizeof(requested_length));
    dual_diag_stream_record(DUAL_DIAG_SOURCE_P_USB,
                            DUAL_DIAG_KIND_P_GET_REPORT_REQUEST,
                            request_event, sizeof(request_event));
    uint8_t interface_number = 0;
    if (buffer == NULL || requested_length == 0U ||
        requested_length > DUAL_HID_CONTROL_MAX_DATA ||
        (uint8_t)report_type < DUAL_HID_REPORT_TYPE_INPUT ||
        (uint8_t)report_type > DUAL_HID_REPORT_TYPE_FEATURE ||
        s_vendor_control_queue == NULL || s_get_gate == NULL ||
        s_get_state_mutex == NULL ||
         s_usb_reconfigure_in_progress ||
         !clone_interface_for_instance(instance, &interface_number)) {
        queue_metric_increment(&s_vendor_control_queue_metrics.rejected);
        diag_capture_get_result(instance, UINT8_MAX, report_id,
                                (uint8_t)report_type,
                                DUAL_HID_REPORT_STATUS_INVALID, NULL, 0U);
        return 0;
    }
    ESP_LOGI(TAG,
             "PC GET_REPORT: instance=%u interface=%u id=%02X type=%u requested=%u",
             instance, interface_number, report_id, (unsigned)report_type,
             requested_length);
    if (xSemaphoreTake(s_get_gate, 0) != pdTRUE) {
        queue_metric_increment(&s_vendor_control_queue_metrics.rejected);
        ++s_vendor_get_timeouts;
        diag_capture_get_result(instance, interface_number, report_id,
                                (uint8_t)report_type,
                                DUAL_HID_REPORT_STATUS_TIMEOUT, NULL, 0U);
        return 0;
    }
    while (s_get_response_sem != NULL &&
           xSemaphoreTake(s_get_response_sem, 0) == pdTRUE) {
        /* Consume a stale completion before starting a new transaction. */
    }
    const uint16_t transaction_id = next_vendor_transaction_id();
    const uint32_t request_session_generation = vendor_session_generation();
    xSemaphoreTake(s_get_state_mutex, portMAX_DELAY);
    s_get_inflight = true;
    s_get_response_ready = false;
    s_get_transaction_id = transaction_id;
    s_get_interface_number = interface_number;
    s_get_report_id = report_id;
    s_get_status = DUAL_HID_REPORT_STATUS_TIMEOUT;
    s_get_response_length = 0;
    s_get_session_generation = request_session_generation;
    xSemaphoreGive(s_get_state_mutex);

    const vendor_control_item_t item = {
        .session_generation = request_session_generation,
        .get_report = true,
        .transaction_id = transaction_id,
        .interface_number = interface_number,
        .report_id = report_id,
        .report_type = (uint8_t)report_type,
        .requested_length = (uint8_t)requested_length,
    };
    if (xQueueSend(s_vendor_control_queue, &item, 0) != pdTRUE) {
        queue_metric_increment(&s_vendor_control_queue_metrics.dropped);
        ++s_vendor_get_timeouts;
        clear_pending_get(false, DUAL_HID_REPORT_STATUS_TIMEOUT);
        xSemaphoreGive(s_get_gate);
        diag_capture_get_result(instance, interface_number, report_id,
                                (uint8_t)report_type,
                                DUAL_HID_REPORT_STATUS_TIMEOUT, NULL, 0U);
        return 0;
    }
    queue_metric_observe_depth(&s_vendor_control_queue_metrics,
                               s_vendor_control_queue);
    ++s_vendor_get_requests;
    const bool signaled = s_get_response_sem != NULL &&
        xSemaphoreTake(s_get_response_sem, pdMS_TO_TICKS(VENDOR_GET_REPORT_TIMEOUT_MS)) == pdTRUE;
    uint16_t result_length = 0;
    uint8_t result_status = DUAL_HID_REPORT_STATUS_TIMEOUT;
    xSemaphoreTake(s_get_state_mutex, portMAX_DELAY);
    if (signaled && s_get_response_ready &&
        s_get_transaction_id == transaction_id &&
        s_get_interface_number == interface_number &&
        s_get_report_id == report_id &&
        s_get_session_generation == vendor_session_generation() &&
        s_get_status == DUAL_HID_REPORT_STATUS_OK &&
        s_get_response_length <= requested_length) {
        result_length = s_get_response_length;
        result_status = s_get_status;
        memcpy(buffer, s_get_response_data, result_length);
        if (result_length != 0U) {
            ESP_LOG_BUFFER_HEX_LEVEL(
                TAG, buffer, result_length, ESP_LOG_INFO);
        }
    } else if (!signaled) {
        ++s_vendor_get_timeouts;
    } else {
        result_status = s_get_status;
    }
    s_get_inflight = false;
    s_get_response_ready = false;
    xSemaphoreGive(s_get_state_mutex);
    xSemaphoreGive(s_get_gate);
    ESP_LOGI(TAG,
             "PC GET_REPORT完成: interface=%u id=%02X returned=%u status=%u",
             interface_number, report_id, result_length, s_get_status);
    diag_capture_get_result(instance, interface_number, report_id,
                            (uint8_t)report_type, result_status,
                            buffer, result_length);
    return result_length;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t const *buffer,
    uint16_t buffer_size)
{
    vendor_note_request();
    queue_metric_increment(&s_vendor_control_queue_metrics.received);
    uint8_t interface_number = UINT8_MAX;
    if (s_vendor_control_queue == NULL ||
        s_usb_reconfigure_in_progress ||
        !clone_interface_for_instance(instance, &interface_number) ||
        buffer_size > DUAL_HID_CONTROL_MAX_DATA ||
        (buffer == NULL && buffer_size != 0U) ||
         (uint8_t)report_type < DUAL_HID_REPORT_TYPE_INPUT ||
         (uint8_t)report_type > DUAL_HID_REPORT_TYPE_FEATURE) {
        queue_metric_increment(&s_vendor_control_queue_metrics.rejected);
        ++s_vendor_set_dropped;
        diag_capture_set_report(instance, interface_number, report_id,
                                (uint8_t)report_type, false, buffer, buffer_size);
        return;
    }
    vendor_control_item_t item = {
        .session_generation = vendor_session_generation(),
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
        queue_metric_increment(&s_vendor_control_queue_metrics.dropped);
        ++s_vendor_set_dropped;
        diag_capture_set_report(instance, interface_number, report_id,
                                (uint8_t)report_type, false, buffer, buffer_size);
    } else {
        queue_metric_observe_depth(&s_vendor_control_queue_metrics,
                                   s_vendor_control_queue);
        ++s_vendor_set_queued;
        diag_capture_set_report(instance, interface_number, report_id,
                                (uint8_t)report_type, true, buffer, buffer_size);
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
            queue_metric_increment(&s_vendor_input_queue_metrics.received);
            queue_metric_increment(&s_vendor_input_queue_metrics.rejected);
            ++s_vendor_input_dropped;
            return;
        }
        if (s_usb_reconfigure_in_progress || !s_clone_active ||
            !s_installed || !tud_mounted()) {
            queue_metric_increment(&s_vendor_input_queue_metrics.received);
            queue_metric_increment(&s_vendor_input_queue_metrics.rejected);
            ++s_vendor_input_dropped;
            return;
        }
        uint8_t item_instance = 0U;
        /*
         * 判断这条原始输入是不是“纯移动报文”（鼠标接口、匹配鼠标布局、按键字节为 0）。
         * 只有它能被牺牲：握手窗口内直接丢，或队列里到达上限时丢，用于保证
         * G HUB 的 SET/GET_REPORT 握手永远有槽位可用。
         */
        const bool motion_only = data_length > 0U && data_length == s_clone_mouse_layout.report_bytes &&
            item.report_id == s_clone_mouse_layout.report_id &&
            clone_instance_for_interface(item.interface_number, &item_instance) &&
            item_instance == s_clone_mouse_instance && data[0] == 0U;
        const int64_t now_us = esp_timer_get_time();
        if (motion_only) {
            const bool handshake_window = s_clone_mount_us != 0 &&
                now_us - s_clone_mount_us < CLONE_HANDSHAKE_GRACE_US;
            const bool vendor_busy = vendor_burst_active();
            if (handshake_window || vendor_busy) {
                queue_metric_increment(&s_motion_input_queue_metrics.received);
                queue_metric_increment(&s_motion_input_queue_metrics.rejected);
                ++s_vendor_motion_skipped;
                return;
            }
        }
        item.session_generation = vendor_session_generation();
        item.enqueued_us = esp_timer_get_time();
        item.length = (uint8_t)data_length;
        if (data_length != 0U) {
            memcpy(item.data, data, data_length);
        }
        ++s_vendor_input_received;
        if (motion_only) {
            /* 移动走独立队列；只在真的排满时才丢（计到 vendor_input_dropped）。 */
            queue_metric_increment(&s_motion_input_queue_metrics.received);
            if (s_motion_input_queue == NULL) {
                queue_metric_increment(&s_motion_input_queue_metrics.rejected);
                ++s_vendor_input_dropped;
            } else if (xQueueSend(s_motion_input_queue, &item, 0) != pdTRUE) {
                queue_metric_increment(&s_motion_input_queue_metrics.dropped);
                ++s_vendor_input_dropped;
            } else {
                queue_metric_observe_depth(&s_motion_input_queue_metrics,
                                           s_motion_input_queue);
                const uint32_t depth = (uint32_t)uxQueueMessagesWaiting(s_motion_input_queue);
                if (depth > s_motion_queue_peak) {
                    s_motion_queue_peak = depth;
                }
            }
            return;
        }
        queue_metric_increment(&s_vendor_input_queue_metrics.received);
        if (s_vendor_input_queue == NULL) {
            queue_metric_increment(&s_vendor_input_queue_metrics.rejected);
            ++s_vendor_input_dropped;
            return;
        }
        if (xQueueSend(s_vendor_input_queue, &item, 0) != pdTRUE) {
            queue_metric_increment(&s_vendor_input_queue_metrics.dropped);
            ++s_vendor_input_dropped;
            return;
        }
        queue_metric_observe_depth(&s_vendor_input_queue_metrics,
                                   s_vendor_input_queue);
        return;
    }
    if (frame->type == DUAL_MESSAGE_VENDOR_CONTROL_RESPONSE) {
        /*
         * 设备级 Vendor 控制请求的响应（2026-09-27）：因为复用 GET_REPORT 的等待
         * 状态，这里按事务号匹配后同样写状态并放行信号量即可。
         */
        uint16_t vendor_transaction_id = 0;
        uint8_t vendor_status = 0;
        const uint8_t *vendor_data = NULL;
        size_t vendor_data_length = 0;
        if (!dual_vendor_control_response_decode(
                frame->payload, frame->payload_length, &vendor_transaction_id,
                &vendor_status, &vendor_data, &vendor_data_length) ||
            vendor_data_length > sizeof(s_get_response_data) ||
            s_get_state_mutex == NULL) {
            return;
        }
        xSemaphoreTake(s_get_state_mutex, portMAX_DELAY);
        const bool vendor_matches = s_get_inflight &&
            s_get_session_generation == vendor_session_generation() &&
            s_get_transaction_id == vendor_transaction_id;
        if (!vendor_matches) {
            ++s_vendor_get_mismatches;
            xSemaphoreGive(s_get_state_mutex);
            return;
        }
        s_get_status = vendor_status;
        s_get_response_length = (uint8_t)vendor_data_length;
        if (vendor_data_length != 0U) {
            memcpy(s_get_response_data, vendor_data, vendor_data_length);
        }
        s_get_response_ready = true;
        if (s_get_response_sem != NULL) {
            xSemaphoreGive(s_get_response_sem);
        }
        xSemaphoreGive(s_get_state_mutex);
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
        s_get_session_generation == vendor_session_generation() &&
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
    /* The atomic token invalidates already-dequeued work; the short mutex
     * serializes that invalidation against its final USB/UART side effect. */
    if (s_vendor_session_mutex != NULL) {
        xSemaphoreTake(s_vendor_session_mutex, portMAX_DELAY);
    }
    (void)advance_vendor_session_generation();
    dual_uart1_cancel_vendor_hid_session();
    if (s_vendor_input_queue != NULL) {
        queue_reset_count_dropped(s_vendor_input_queue,
                                  &s_vendor_input_queue_metrics);
    }
    if (s_vendor_control_queue != NULL) {
        queue_reset_count_dropped(s_vendor_control_queue,
                                  &s_vendor_control_queue_metrics);
    }
    clear_pending_get(true, DUAL_HID_REPORT_STATUS_TIMEOUT);
    if (s_get_gate != NULL) {
        (void)xSemaphoreGive(s_get_gate);
    }
    if (s_vendor_session_mutex != NULL) {
        xSemaphoreGive(s_vendor_session_mutex);
    }
}

static void usb_event_callback(tinyusb_event_t *event, void *argument)
{
    (void)argument;
    if (event == NULL) {
        return;
    }
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_pc_usb_attached = true;
        ESP_LOGI(TAG, "PC侧USB HID已连接");
        dual_status_led_set_pc_mounted(true);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        const int64_t now_us = esp_timer_get_time();
        if (s_usb_reconfigure_in_progress ||
            now_us < s_usb_reconfigure_guard_until_us) {
            ESP_LOGI(TAG, "忽略USB重枚举期间迟到的DETACHED事件");
            return;
        }
        s_pc_usb_attached = false;
        ESP_LOGW(TAG, "PC侧USB HID已断开，清理所有输入");
        dual_status_led_set_pc_mounted(false);
        dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
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
    diag_capture_report_complete(instance, report, len);
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
    diag_capture_report_failed(instance, (uint8_t)report_type, report,
                               xferred_bytes, xferred_bytes);
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
                  " balance=(%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ")"
                  " installed_transfer=%" PRIu32 " installed_crc=%08" PRIX32
                  " motion_skipped=%" PRIu32
                  " motion_merged=%" PRIu32
                  " motion_q_peak=%" PRIu32 " motion_lat_peak_us=%" PRId64
                   " cleanup_retry=%" PRIu32 " ack_retry=%" PRIu32
                   " motion_fwd_dx=%lld motion_fwd_dy=%lld"
                   " motion_fwd_ok=%" PRIu32 " motion_fwd_skip=%" PRIu32,
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
                  input_pan - s_output_pan - pending_pan,
                 s_reconfigure_installed_transfer_id, s_reconfigure_installed_crc32,
                 s_vendor_motion_skipped,
                 s_motion_merged_total,
                 s_motion_queue_peak, s_motion_latency_peak_us,
                  s_reconfigure_cleanup_retries, s_reconfigure_ack_retries,
                  (long long)__atomic_load_n(&s_motion_fwd_dx, __ATOMIC_RELAXED),
                  (long long)__atomic_load_n(&s_motion_fwd_dy, __ATOMIC_RELAXED),
                  (uint32_t)__atomic_load_n(&s_motion_fwd_ok, __ATOMIC_RELAXED),
                  (uint32_t)__atomic_load_n(&s_motion_fwd_skip, __ATOMIC_RELAXED));
        log_queue_metrics("pc_vendor_input", &s_vendor_input_queue_metrics);
        log_queue_metrics("pc_motion_input", &s_motion_input_queue_metrics);
        log_queue_metrics("pc_vendor_control", &s_vendor_control_queue_metrics);
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
        if (s_usb_reconfigure_in_progress || !tud_mounted()) {
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
            const bool report_queued = tud_hid_n_report(
                s_clone_mouse_instance, s_clone_mouse_layout.report_id,
                payload, payload_length);
            diag_capture_report_submit(report_queued, s_clone_mouse_instance,
                                       s_clone_mouse_layout.report_id,
                                       payload, payload_length);
            if (!report_queued) {
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
        const bool report_queued = tud_hid_report(
            REPORT_ID_MOUSE, payload, sizeof(payload));
        diag_capture_report_submit(report_queued, 0U, REPORT_ID_MOUSE,
                                   payload, sizeof(payload));
        if (!report_queued) {
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
    /*
     * Dynamic Profile replacement uninstalls TinyUSB without relying on the
     * DETACHED callback: that callback intentionally skips vendor cleanup
     * while reconfiguration is active.  Clear all HID++ queues and any
     * in-flight GET_REPORT before tearing down the old clone, otherwise a
     * transaction from the previous device session can be delivered to the
     * newly installed clone and make G HUB reject the device after a peer
     * (mouse-side) reset.
     */
    dual_pc_hid_vendor_link_fault();
    dual_pc_hid_release_all();
    vTaskDelay(pdMS_TO_TICKS(3));
    vendor_session_wait_idle();
    if (!s_installed) {
        clear_clone_session_state();
        s_pc_usb_attached = false;
        dual_status_led_set_pc_mounted(false);
        return ESP_OK;
    }
    esp_err_t result = dual_usb_cdc_control_stop();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "停止 CDC 控制失败(%s)，继续拆除 USB 栈", esp_err_to_name(result));
        result = ESP_OK;
    }
    const esp_err_t sender_result = dual_pc_hid_stop_sender();
    if (sender_result != ESP_OK) {
        ESP_LOGW(TAG, "停止发送任务失败(%s)，继续拆除 USB 栈",
                 esp_err_to_name(sender_result));
    }
    esp_err_t uninstall = tinyusb_driver_uninstall();
    if (uninstall == ESP_ERR_INVALID_STATE) {
        /*
         * esp_tinyusb 在任务已退出（例如启动期 del 路径）时会返回 INVALID_STATE：
         * 此时栈本来就不在，“拆除”这个目标已经达成。之前把它当失败，会让克隆门
         * 永久关闭并无限重试（实测：tinyusb_driver_uninstall -> Deinit TinyUSB
         * task failed -> P侧USB清理重试耗尽，接收端再也没有克隆设备）。
         */
        ESP_LOGW(TAG, "TinyUSB 栈已不在（任务已自行退出或未装入），按已拆除处理");
        uninstall = ESP_OK;
    }
    if (uninstall == ESP_OK) {
        s_installed = false;
        s_pc_usb_attached = false;
        dual_status_led_set_pc_mounted(false);
        clear_clone_session_state();
        s_usb_reconfigure_guard_until_us = esp_timer_get_time() +
            (int64_t)USB_RECONFIGURE_EVENT_GUARD_MS * 1000LL;
    } else {
        result = uninstall;
    }
    return result;
}

esp_err_t dual_pc_hid_prepare_for_profile(void){
    /* 启动板间接收前同步撤下探测用设备，防止已就绪的鼠标侧 Profile
     * 抢在异步 disconnect 之前到达，随后又被该 disconnect 覆盖。 */
    if (s_state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_usb_reconfigure_in_progress = true;
    dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
    const esp_err_t result = stop_installed_usb();
    s_usb_reconfigure_in_progress = false;
    return result;
}

static void reconfigure_task(void *argument)
{
    (void)argument;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            bool disconnect_requested = false;
            bool profile_pending = false;
            uint32_t operation_epoch = 0;
            uint32_t disconnect_peer_generation = 0;
            uint32_t disconnect_event_id = 0;
            uint8_t disconnect_ack_type = 0;
            uint32_t disconnect_ack_flow_id = 0;
            bool allow_profile_request = false;

            xSemaphoreTake(s_reconfigure_mutex, portMAX_DELAY);
            disconnect_requested = s_reconfigure_disconnect_requested;
            if (disconnect_requested) {
                operation_epoch = s_reconfigure_epoch;
                disconnect_peer_generation = s_reconfigure_disconnect_peer_generation;
                disconnect_event_id = s_reconfigure_disconnect_event_id;
                disconnect_ack_type = s_reconfigure_disconnect_ack_type;
                disconnect_ack_flow_id = s_reconfigure_disconnect_ack_flow_id;
                allow_profile_request = s_reconfigure_allow_profile_request;
            } else if (s_reconfigure_profile_pending &&
                       !s_reconfigure_disconnect_failed) {
                profile_pending = true;
                operation_epoch = s_reconfigure_profile_epoch;
                memcpy(&s_reconfigure_work_profile, &s_reconfigure_profile,
                       sizeof(s_reconfigure_work_profile));
                s_work_transfer_id = s_reconfigure_transfer_id;
                s_work_crc32 = s_reconfigure_crc32;
                s_reconfigure_profile_pending = false;
            }
            xSemaphoreGive(s_reconfigure_mutex);

            if (!disconnect_requested && !profile_pending) {
                break;
            }

            if (disconnect_requested) {
                ESP_LOGW(TAG, "开始P侧清理屏障：epoch=%" PRIu32
                         " ack_type=%02X flow=%" PRIu32 " cleanup_done=%u"
                         " cleanup_try=%u ack_try=%u",
                         operation_epoch, disconnect_ack_type,
                         disconnect_ack_flow_id, (unsigned)s_reconfigure_cleanup_done,
                         (unsigned)s_reconfigure_cleanup_attempts,
                         (unsigned)s_reconfigure_ack_attempts);
                esp_err_t cleanup_result = ESP_OK;
                if (!s_reconfigure_cleanup_done) {
                    s_usb_reconfigure_in_progress = true;
                    dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
                    cleanup_result = stop_installed_usb();
                }
                esp_err_t ack_result = ESP_OK;

                xSemaphoreTake(s_reconfigure_mutex, portMAX_DELAY);
                const bool still_current = operation_epoch == s_reconfigure_epoch &&
                    s_reconfigure_disconnect_requested;
                if (!still_current) {
                    xSemaphoreGive(s_reconfigure_mutex);
                    /* A newer peer session superseded this operation.  Its
                     * own cleanup request remains queued for the next loop. */
                    continue;
                }
                /* 同一 worker 持有的旧快照也必须清除；新的待装 Profile
                 * 保存在另一缓冲区，之后会重新复制到这里。 */
                memset(&s_reconfigure_work_profile, 0,
                       sizeof(s_reconfigure_work_profile));
                s_work_transfer_id = 0U;
                s_work_crc32 = 0U;

                if (cleanup_result != ESP_OK) {
                    /* 清理失败：保持克隆门关闭、保留同一事务身份，
                     * 在同一 generation 内做有界重试，不永久作废。 */
                    ++s_reconfigure_cleanup_attempts;
                    if (link_cleanup_should_retry(s_reconfigure_cleanup_attempts,
                                                  LINK_CLEANUP_MAX_ATTEMPTS)) {
                        ++s_reconfigure_cleanup_retries;
                        xSemaphoreGive(s_reconfigure_mutex);
                        dual_status_led_set_flow_error(true);
                        ESP_LOGE(TAG, "P侧USB清理失败(%s)，保持克隆门关闭并重试第%u次："
                                 "type=%02X flow=%" PRIu32,
                                 esp_err_to_name(cleanup_result),
                                 (unsigned)s_reconfigure_cleanup_attempts,
                                 disconnect_ack_type, disconnect_ack_flow_id);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        continue;
                    }
                    s_reconfigure_disconnect_requested = false;
                    s_reconfigure_disconnect_failed = true;
                    s_reconfigure_failed_peer_generation = disconnect_peer_generation;
                    s_reconfigure_cleanup_done = false;
                    xSemaphoreGive(s_reconfigure_mutex);
                    if (disconnect_ack_type != 0U) {
                        (void)dual_uart1_send_flow_ack_for_generation(
                            disconnect_ack_type, disconnect_ack_flow_id,
                            DUAL_FLOW_STATUS_FAILED, disconnect_peer_generation);
                    }
                    ESP_LOGE(TAG, "P侧USB清理重试耗尽，明确失败：%s",
                             esp_err_to_name(cleanup_result));
                    dual_uart1_set_profile_request_ready(false);
                    dual_status_led_set_flow_error(true);
                    s_usb_reconfigure_in_progress = true;
                    continue;
                }

                /* 清理成功：先登记事务结果，再尝试确认帧。这样即使确认帧
                 * 入队失败，重复请求也只会补发 ACK，不会重复卸载 USB。 */
                s_reconfigure_cleanup_done = true;
                if (disconnect_ack_type == DUAL_MESSAGE_DEVICE_GONE) {
                    s_reconfigure_last_gone_peer_generation =
                        disconnect_peer_generation;
                    s_reconfigure_last_gone_event_id = disconnect_event_id;
                } else if (disconnect_ack_type == DUAL_MESSAGE_PROFILE_OFFER) {
                    s_reconfigure_last_offer_peer_generation =
                        disconnect_peer_generation;
                    s_reconfigure_last_offer_flow_id = disconnect_ack_flow_id;
                }
                if (disconnect_ack_type != 0U) {
                    ack_result = dual_uart1_send_flow_ack_for_generation(
                        disconnect_ack_type, disconnect_ack_flow_id,
                        DUAL_FLOW_STATUS_ACCEPTED, disconnect_peer_generation);
                }
                if (ack_result != ESP_OK) {
                    /* USB 卸载已经成功：只重试排队原 ACK。 */
                    ++s_reconfigure_ack_attempts;
                    if (link_cleanup_should_retry(s_reconfigure_ack_attempts,
                                                  LINK_ACK_ENQUEUE_MAX_ATTEMPTS)) {
                        ++s_reconfigure_ack_retries;
                        xSemaphoreGive(s_reconfigure_mutex);
                        dual_status_led_set_flow_error(true);
                        ESP_LOGE(TAG, "清理已完成但确认帧无法排队(%s)，只补发ACK："
                                 "type=%02X flow=%" PRIu32 " try=%u",
                                 esp_err_to_name(ack_result), disconnect_ack_type,
                                 disconnect_ack_flow_id,
                                 (unsigned)s_reconfigure_ack_attempts);
                        vTaskDelay(pdMS_TO_TICKS(50));
                        continue;
                    }
                    s_reconfigure_disconnect_requested = false;
                    s_reconfigure_disconnect_failed = true;
                    s_reconfigure_failed_peer_generation = disconnect_peer_generation;
                    xSemaphoreGive(s_reconfigure_mutex);
                    ESP_LOGE(TAG, "清理已完成但确认帧补发耗尽；保持克隆门关闭");
                    dual_uart1_set_profile_request_ready(false);
                    dual_status_led_set_flow_error(true);
                    s_usb_reconfigure_in_progress = true;
                    continue;
                }

                s_reconfigure_disconnect_requested = false;
                s_reconfigure_disconnect_failed = false;
                s_reconfigure_cleanup_attempts = 0;
                s_reconfigure_ack_attempts = 0;
                xSemaphoreGive(s_reconfigure_mutex);

                ESP_LOGW(TAG, "P侧USB卸载及旧会话清理完成，已确认M的清理屏障：epoch=%" PRIu32
                         " cleanup_retry=%" PRIu32 " ack_retry=%" PRIu32,
                         operation_epoch, s_reconfigure_cleanup_retries,
                         s_reconfigure_ack_retries);
                s_usb_reconfigure_in_progress = false;
                if (allow_profile_request) {
                    dual_uart1_set_profile_request_ready(true);
                }
                dual_status_led_set_flow_error(false);
                continue;
            }

            if (!reconfigure_epoch_is_current(operation_epoch)) {
                continue;
            }
            dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
            s_usb_reconfigure_in_progress = true;
            esp_err_t result = stop_installed_usb();
            if (result == ESP_OK && reconfigure_epoch_is_current(operation_epoch)) {
                ESP_LOGI(TAG, "旧Profile、描述符、鼠标报告模板及厂商HID会话已清空；准备安装新Profile");
                vTaskDelay(pdMS_TO_TICKS(300));
                if (!reconfigure_epoch_is_current(operation_epoch)) {
                    continue;
                }
                if (!prepare_clone_descriptor_set(&s_reconfigure_work_profile)) {
                    result = ESP_ERR_INVALID_RESPONSE;
                }
            }
            if (result == ESP_OK && reconfigure_epoch_is_current(operation_epoch)) {
                memcpy(&s_active_profile, &s_reconfigure_work_profile,
                       sizeof(s_active_profile));
                s_clone_active = true;
                s_clone_mount_us = esp_timer_get_time();
                result = install_tinyusb(true);
                if (result == ESP_OK) {
                    result = restart_runtime_after_install();
                }
            }
            if (result == ESP_OK) {
                const TickType_t mount_deadline =
                    xTaskGetTickCount() + pdMS_TO_TICKS(3000);
                while (!tud_mounted() && xTaskGetTickCount() < mount_deadline &&
                       reconfigure_epoch_is_current(operation_epoch)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                if (!reconfigure_epoch_is_current(operation_epoch)) {
                    /* DEVICE_GONE/new peer epoch won the race.  Uninstall this
                     * stale candidate before acknowledging any flow. */
                    const esp_err_t stale_cleanup = stop_installed_usb();
                    if (stale_cleanup != ESP_OK) {
                        dual_status_led_set_flow_error(true);
                        ESP_LOGE(TAG, "已取消旧Profile，但临时USB实例卸载失败：%s",
                                 esp_err_to_name(stale_cleanup));
                        s_usb_reconfigure_in_progress = true;
                    }
                    continue;
                }
                if (!tud_mounted()) {
                    result = ESP_ERR_TIMEOUT;
                } else {
                    dual_uart1_set_usb_state(DUAL_USB_STATE_HID_CONNECTED);
                    xSemaphoreTake(s_reconfigure_mutex, portMAX_DELAY);
                    const bool current = dual_profile_operation_is_current(
                        operation_epoch, s_reconfigure_epoch,
                        s_reconfigure_disconnect_requested);
                    /* transfer=0 保留给 UART0 离线注入；没有 M 时无需板间 ACK。 */
                    const esp_err_t ack_result = !current ? ESP_ERR_INVALID_STATE :
                        s_work_transfer_id == 0U ? ESP_OK :
                        dual_uart1_send_profile_ack(s_work_transfer_id,
                                                    s_work_crc32, 0U);
                    xSemaphoreGive(s_reconfigure_mutex);
                    if (ack_result != ESP_OK) {
                        result = ack_result;
                    } else {
                        s_reconfigure_installed_transfer_id = s_work_transfer_id;
                        s_reconfigure_installed_crc32 = s_work_crc32;
                        ESP_LOGI(TAG, "%s：transfer=%" PRIu32 " crc=%08" PRIX32,
                                 s_work_transfer_id == 0U ?
                                     "手动Profile已配置并挂载" :
                                     "Profile已配置并挂载，最终ACK已排队",
                                 s_work_transfer_id, s_work_crc32);
                        dual_status_led_set_flow_error(false);
                        s_usb_reconfigure_guard_until_us = esp_timer_get_time() +
                            (int64_t)USB_RECONFIGURE_EVENT_GUARD_MS * 1000LL;
                    }
                }
            }

            if (result == ESP_OK) {
                s_usb_reconfigure_in_progress = false;
                ESP_LOGI(TAG,
                         "动态USB严格克隆已启用：VID:PID=%04X:%04X HID=%u mouse_instance=%u CDC=disabled",
                         s_clone_device_descriptor.idVendor,
                         s_clone_device_descriptor.idProduct,
                         s_clone_descriptors.hid_count,
                         s_clone_mouse_instance);
                continue;
            }

            if (!reconfigure_epoch_is_current(operation_epoch)) {
                /* A disconnect barrier will own cleanup and its matching ACK. */
                continue;
            }
            ESP_LOGE(TAG, "动态USB克隆启动失败：%s；旧设备不回退",
                     esp_err_to_name(result));
            dual_status_led_set_flow_error(true);
            if (s_work_transfer_id != 0U) {
                (void)dual_uart1_send_profile_ack(s_work_transfer_id, s_work_crc32, 1U);
            }
            const esp_err_t failure_cleanup = stop_installed_usb();
            if (failure_cleanup == ESP_OK) {
                s_usb_reconfigure_in_progress = false;
            } else {
                s_usb_reconfigure_in_progress = true;
                ESP_LOGE(TAG, "克隆失败后的TinyUSB清理也失败：%s",
                         esp_err_to_name(failure_cleanup));
            }
        }
    }
}

esp_err_t dual_pc_hid_install_device(void)
{
    if (s_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_pc_usb_attached = false;
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
        /* Role discovery may lock to PC_DEVICE after this probe install fails.
         * Keep the initialized state mutex for the later strict-clone install. */
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
    s_reconfigure_profile_pending = false;
    s_reconfigure_disconnect_failed = false;
    s_reconfigure_cleanup_done = false;
    s_reconfigure_cleanup_attempts = 0;
    s_reconfigure_ack_attempts = 0;
    s_reconfigure_cleanup_retries = 0;
    s_reconfigure_ack_retries = 0;
    s_reconfigure_installed_transfer_id = 0U;
    s_reconfigure_installed_crc32 = 0U;
    s_reconfigure_epoch = 1U;
    s_reconfigure_peer_generation = 0U;
    s_reconfigure_last_gone_peer_generation = 0U;
    s_reconfigure_last_gone_event_id = 0U;
    s_reconfigure_last_offer_peer_generation = 0U;
    s_reconfigure_last_offer_flow_id = 0U;
}

esp_err_t dual_pc_hid_schedule_reconfigure(
    const hid_device_profile_t *profile, uint32_t transfer_id, uint32_t crc32)
{
    if (!s_reconfigure_enabled || s_reconfigure_mutex == NULL ||
        s_reconfigure_task == NULL || profile == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_reconfigure_mutex, portMAX_DELAY);
    if (s_reconfigure_disconnect_requested || s_reconfigure_disconnect_failed) {
        xSemaphoreGive(s_reconfigure_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t operation_epoch = next_reconfigure_epoch_locked();
    s_usb_reconfigure_in_progress = true;
    memcpy(&s_reconfigure_profile, profile, sizeof(s_reconfigure_profile));
    s_reconfigure_transfer_id = transfer_id;
    s_reconfigure_crc32 = crc32;
    s_reconfigure_profile_epoch = operation_epoch;
    s_reconfigure_profile_pending = true;
    xSemaphoreGive(s_reconfigure_mutex);
    /* 使队列及已 dequeue 的旧厂商事务失效；物理报告热路径不等待此锁。 */
    dual_pc_hid_vendor_link_fault();
    dual_pc_hid_release_all();
    ESP_LOGI(TAG,
             "收到完整物理HID Profile，已建立重配置epoch并使旧厂商会话失效：transfer=%" PRIu32
             " epoch=%" PRIu32,
             transfer_id, operation_epoch);
    xTaskNotifyGive(s_reconfigure_task);
    return ESP_OK;
}

esp_err_t dual_pc_hid_schedule_disconnect(
    uint32_t sender_generation,
    uint32_t event_id,
    uint8_t ack_type,
    uint32_t ack_flow_id)
{
    if (!s_reconfigure_enabled || s_reconfigure_mutex == NULL ||
        s_reconfigure_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((ack_type == DUAL_MESSAGE_DEVICE_GONE &&
         (sender_generation == 0U || event_id == 0U || ack_flow_id != event_id)) ||
        (ack_type == DUAL_MESSAGE_PROFILE_OFFER &&
         (sender_generation == 0U || event_id != 0U || ack_flow_id == 0U)) ||
        (ack_type == 0U && (event_id != 0U || ack_flow_id != 0U)) ||
        (ack_type != 0U && ack_type != DUAL_MESSAGE_DEVICE_GONE &&
         ack_type != DUAL_MESSAGE_PROFILE_OFFER)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_reconfigure_mutex, portMAX_DELAY);
    if (sender_generation != 0U &&
        sender_generation != s_reconfigure_peer_generation) {
        s_reconfigure_peer_generation = sender_generation;
        s_reconfigure_disconnect_failed = false;
        s_reconfigure_last_gone_peer_generation = 0U;
        s_reconfigure_last_gone_event_id = 0U;
        s_reconfigure_last_offer_peer_generation = 0U;
        s_reconfigure_last_offer_flow_id = 0U;
    }

    bool duplicate_completed = false;
    if (ack_type == DUAL_MESSAGE_DEVICE_GONE) {
        duplicate_completed =
            s_reconfigure_last_gone_peer_generation == sender_generation &&
            s_reconfigure_last_gone_event_id != 0U &&
            !dual_flow_id_is_newer(event_id, s_reconfigure_last_gone_event_id);
    } else if (ack_type == DUAL_MESSAGE_PROFILE_OFFER) {
        duplicate_completed =
            s_reconfigure_last_offer_peer_generation == sender_generation &&
            s_reconfigure_last_offer_flow_id != 0U &&
            !dual_flow_id_is_newer(ack_flow_id, s_reconfigure_last_offer_flow_id);
    }
    if (duplicate_completed) {
        xSemaphoreGive(s_reconfigure_mutex);
        const esp_err_t ack_result = dual_uart1_send_flow_ack_for_generation(
            ack_type, ack_flow_id, DUAL_FLOW_STATUS_ACCEPTED, sender_generation);
        if (ack_result != ESP_OK) {
            dual_status_led_set_flow_error(true);
        } else if (s_reconfigure_disconnect_failed) {
            /* 清理早已完成、只是确认帧没排上队：补发成功后重新开放克隆门。 */
            xSemaphoreTake(s_reconfigure_mutex, portMAX_DELAY);
            s_reconfigure_disconnect_failed = false;
            s_reconfigure_ack_attempts = 0;
            s_reconfigure_cleanup_attempts = 0;
            xSemaphoreGive(s_reconfigure_mutex);
            dual_status_led_set_flow_error(false);
        }
        ++s_reconfigure_duplicate_requests;
        if (!s_reconfigure_dup_logged_valid ||
            s_reconfigure_dup_logged_type != ack_type ||
            s_reconfigure_dup_logged_flow_id != ack_flow_id) {
            s_reconfigure_dup_logged_valid = true;
            s_reconfigure_dup_logged_type = ack_type;
            s_reconfigure_dup_logged_flow_id = ack_flow_id;
            ESP_LOGW(TAG, "忽略已完成的迟到/重复清理请求，不触碰新Profile：type=%02X"
                     " flow=%" PRIu32 " duplicates=%" PRIu32,
                     ack_type, ack_flow_id, s_reconfigure_duplicate_requests);
        }
        return ack_result;
    }
    if (s_reconfigure_disconnect_requested) {
        const bool same_request =
            s_reconfigure_disconnect_ack_type == ack_type &&
            s_reconfigure_disconnect_peer_generation == sender_generation &&
            s_reconfigure_disconnect_event_id == event_id &&
            s_reconfigure_disconnect_ack_flow_id == ack_flow_id;
        if (same_request) {
            xSemaphoreGive(s_reconfigure_mutex);
            return ESP_OK;
        }
        const bool current_is_gone =
            s_reconfigure_disconnect_ack_type == DUAL_MESSAGE_DEVICE_GONE;
        const bool incoming_is_gone = ack_type == DUAL_MESSAGE_DEVICE_GONE;
        if (current_is_gone && !incoming_is_gone) {
            const bool new_peer_session = ack_type == 0U && sender_generation != 0U &&
                sender_generation != s_reconfigure_disconnect_peer_generation;
            if (!new_peer_session) {
                const esp_err_t coalesced = ack_type == 0U ? ESP_OK : ESP_ERR_INVALID_STATE;
                xSemaphoreGive(s_reconfigure_mutex);
                return coalesced;
            }
        }
        if (!current_is_gone && !incoming_is_gone && ack_type == 0U &&
            sender_generation == s_reconfigure_disconnect_peer_generation) {
            xSemaphoreGive(s_reconfigure_mutex);
            return ESP_OK;
        }
        if (current_is_gone && incoming_is_gone &&
            s_reconfigure_disconnect_peer_generation == sender_generation) {
            /* M has a single GONE event in flight.  Do not replace its id. */
            xSemaphoreGive(s_reconfigure_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_reconfigure_disconnect_failed) {
        /*
         * 清理失败不能永久拒绝同一 generation 的后续恢复；重新排队同一
         * 会话的清理请求，克隆门在本轮清理成功前始终保持关闭。
         */
        ESP_LOGW(TAG, "清理失败后按同一会话重新排队：type=%02X flow=%" PRIu32
                 " peer_generation=%" PRIu32 " failed_generation=%" PRIu32,
                 ack_type, ack_flow_id, sender_generation,
                 s_reconfigure_failed_peer_generation);
        s_reconfigure_disconnect_failed = false;
        s_reconfigure_cleanup_attempts = 0;
        s_reconfigure_ack_attempts = 0;
        s_reconfigure_cleanup_done = false;
    }
    s_reconfigure_dup_logged_valid = false;
    s_reconfigure_dup_logged_type = 0U;
    s_reconfigure_dup_logged_flow_id = 0U;
    s_reconfigure_duplicate_requests = 0U;
    const uint32_t operation_epoch = next_reconfigure_epoch_locked();
    s_usb_reconfigure_in_progress = true;
    s_reconfigure_disconnect_requested = true;
    s_reconfigure_disconnect_failed = false;
    s_reconfigure_cleanup_done = false;
    s_reconfigure_disconnect_peer_generation = sender_generation;
    s_reconfigure_disconnect_event_id = event_id;
    s_reconfigure_disconnect_ack_type = ack_type;
    s_reconfigure_disconnect_ack_flow_id = ack_flow_id;
    s_reconfigure_allow_profile_request = ack_type == 0U;
    /* A pending old Profile is canceled.  An in-flight worker's snapshot is
     * invalidated by operation_epoch and must not install or ACK it. */
    s_reconfigure_profile_pending = false;
    s_reconfigure_profile_epoch = 0U;
    s_reconfigure_transfer_id = 0U;
    s_reconfigure_crc32 = 0U;
    memset(&s_reconfigure_profile, 0, sizeof(s_reconfigure_profile));
    xSemaphoreGive(s_reconfigure_mutex);
    dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
    dual_uart1_set_profile_request_ready(false);
    dual_pc_hid_vendor_link_fault();
    dual_pc_hid_release_all();
    ESP_LOGW(TAG, "排队P侧清理屏障：type=%02X flow=%" PRIu32
             " peer_generation=%" PRIu32 " epoch=%" PRIu32,
             ack_type, ack_flow_id, sender_generation, operation_epoch);
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

bool dual_pc_hid_usb_attached(void)
{
    return s_pc_usb_attached;
}
