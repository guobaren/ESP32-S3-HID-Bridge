#include "uart1_link.h"

#include "onboard_log.h"

#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "dual_proxy_runtime_config.h"
#include "dual_status_led.h"
#include "hid_device_profile.h"
#include "link_recovery_logic.h"

#define LINK_UART UART_NUM_1
#define LINK_TX_GPIO 17
#define LINK_RX_GPIO 18
#define LINK_BAUD 921600
#define LINK_RX_BUFFER_SIZE 16384
/* 溢出只丢弃并重同步，不再判链路故障；计数在统计行里暴露。 */
static volatile uint32_t s_rx_overflows;
/* UART 接收环里待处理字节数的峰值：应用队列为空但仍有残留移动时，
 * 积压就在这个环里（移动约 14 KB/s，卡住 100 ms 就能攒下上千字节旧报文）。 */
static volatile uint32_t s_rx_pending_peak;
/* 心跳（空闲帧）发送间隔峰值：用来判断"鼠标侧板是否真的被卡住"，以及卡多久。 */
static volatile uint32_t s_heartbeat_gap_peak_ms;
static volatile int64_t s_last_heartbeat_us;
/* 原始输入帧（移动）从入队到发出的停留峰值，单位微秒。 */
static volatile int64_t s_raw_tx_latency_peak_us;
#define LINK_TX_BUFFER_SIZE 4096
#define LINK_EVENT_QUEUE_LENGTH 32
#define LINK_TX_QUEUE_LENGTH 128
/* 实体报文与厂商/控制报文各自 128 槽：移动（1 kHz）不再因为浅队列被大量丢弃，
 * 两条队列等长也避免一边满一边空造成的丢弃偏差。 */
#define LINK_SAFETY_QUEUE_LENGTH 8
#define LINK_SOFTWARE_QUEUE_LENGTH 128
#define LINK_VENDOR_QUEUE_LENGTH 128
#define LINK_RX_CHUNK_SIZE 128
/* 等待对端枚举物理鼠标期间，周期性打印一次“仍在等待”（不判失败、不红闪）。 */
#define LINK_WAITING_DEVICE_LOG_INTERVAL_US 10000000LL
#define LINK_TASK_PRIORITY 6
#define LINK_PERIOD_MS 250
/*
 * 对端静默判定阈值。原为 750 ms，但空闲心跳间隔是 250 ms——鼠标侧板只要被卡住
 * 3 个心跳周期（实测现场：每 10~20 秒就发生一次），P 就判定链路故障并拆掉克隆，
 * 随后再重建，用户实际感受到的是"每十几秒断一次、每次 1~2 秒"。放宽到 3 秒后，
 * 短暂卡顿不再引发拆卸；真正掉线只是晚 2 秒被发现（输入本来就已中断）。
 */
#define LINK_TIMEOUT_MS 3000
#define LINK_HELLO_LENGTH 12
#define LINK_PING_LENGTH 4
#define LINK_STATS_PERIOD_US 5000000LL
#define LINK_PROFILE_FAIRNESS_LIMIT 8U
#define LINK_PROFILE_PHASE_BEGIN 0U
#define LINK_PROFILE_PHASE_CHUNK 1U
#define LINK_PROFILE_PHASE_COMMIT 2U
#define LINK_PROFILE_PHASE_IDLE 3U
#define LINK_FLOW_ACK_TIMEOUT_US LINK_PROFILE_STAGE_TIMEOUT_US
/* Profile 整份重传与 COMMIT 重放的次数上限（同一 transfer ID）。 */
#define LINK_PROFILE_RETRANSMIT_MAX 2U

_Static_assert(CONFIG_FREERTOS_HZ == DUAL_PROXY_REQUIRED_FREERTOS_HZ,
               "dual_proxy要求CONFIG_FREERTOS_HZ=1000");
_Static_assert(pdMS_TO_TICKS(1) == 1, "1ms必须正好折算为1 tick");

static const char *TAG = "dual_uart1";

typedef struct {
    uint8_t type;
    uint8_t length;
    uint32_t vendor_session_generation;
    /* 入队时刻：测量"产生→发出"的发送侧停留时间（延迟定位）。 */
    int64_t enqueued_us;
    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD];
} tx_item_t;

static volatile uint8_t s_role;
static volatile uint8_t s_peer_role;
static volatile uint8_t s_usb_state;
static uint8_t s_node_id[6];
static uint32_t s_generation;
static uint16_t s_next_sequence;
static uint16_t s_peer_last_sequence;
static bool s_peer_sequence_initialized;
static uint32_t s_peer_generation;
static bool s_peer_generation_initialized;
static volatile bool s_peer_online;
static int64_t s_last_peer_rx_us;
static uint32_t s_tx_count;
static uint32_t s_rx_count;
static uint32_t s_tx_physical_count;
static uint32_t s_rx_physical_count;
static uint32_t s_tx_software_count;
static uint32_t s_rx_software_count;
static uint32_t s_crc_or_frame_errors;
static uint32_t s_rx_bytes;
static uint32_t s_peer_silence_ms;
/* 对端超时后一次性抓取原始字节，用来区分“字节没到”和“字节到了但解析不出来”。 */
static volatile uint32_t s_raw_dump_budget;
static uint32_t s_rejected_peers;
/* 对端未上电时串回自身的帧计数（只统计，不打日志、不计入 reject）。 */
static uint32_t s_self_frames;
static volatile uint32_t s_tx_queue_current;
static volatile uint32_t s_tx_queue_peak;
static volatile uint32_t s_tx_queue_overflows;
static volatile uint32_t s_tx_queue_drops;
static volatile uint32_t s_tx_write_failures;
static QueueHandle_t s_uart_event_queue;
static QueueHandle_t s_tx_queue;
static QueueHandle_t s_safety_tx_queue;
static QueueHandle_t s_software_tx_queue;
static QueueHandle_t s_vendor_tx_queue;
static TaskHandle_t s_rx_task;
static TaskHandle_t s_tx_task;
static dual_link_frame_callback_t s_frame_callback;
static dual_link_fault_callback_t s_fault_callback;
static volatile bool s_fault_in_progress;
static uint8_t s_profile_buffers[2][HID_PROFILE_MAX_BLOB];
static uint8_t s_profile_active_buffer;
static uint32_t s_profile_length;
static uint32_t s_profile_crc32;
static uint32_t s_profile_transfer_id;
static uint32_t s_profile_offset;
static uint8_t s_profile_phase;
static bool s_profile_pending;
static volatile uint8_t s_peer_usb_state;
static uint32_t s_profile_peer_generation;
static bool s_profile_peer_generation_valid;
static int64_t s_profile_last_commit_us;
static uint8_t s_profile_motion_since_send;
static portMUX_TYPE s_profile_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_profile_buffer_mutex;
static uint32_t s_profile_starts;
static uint32_t s_profile_chunks;
static uint32_t s_profile_commits;
static uint32_t s_profile_restarts;
static uint32_t s_profile_failures;
static volatile uint32_t s_vendor_queue_overflows;
static volatile uint32_t s_vendor_queue_drops;
static uint32_t s_vendor_hid_session_generation = 1U;
static bool s_role_ack_logged;
/* P：本会话唯一一次 PROFILE_REQUEST 的事务状态。 */
static link_flow_t s_request_flow;
static volatile bool s_profile_request_cleanup_ready;
static bool s_profile_request_waiting_offer;
/*
 * M 明确回了 “已受理但在等物理鼠标”：P 保持等待，既不重放申请也不判失败，
 * 直到对端 usb_state 变成 HID_CONNECTED 或收到 OFFER。
 */
static bool s_profile_request_waiting_device;
static bool s_profile_request_offer_seen;
static int64_t s_profile_request_accepted_us;
static uint8_t s_request_offer_retries;
static uint32_t s_next_flow_id;
/* M：Profile 克隆提议的事务状态。 */
static link_flow_t s_offer_flow;
/* M：COMMIT 已发出，等待 P 的数据接收确认与最终挂载结果。 */
static link_flow_t s_commit_flow;
static bool s_profile_commit_receipt_seen;
static uint8_t s_profile_retransmit_attempts;
/* M：鼠标拔出清理屏障（同一事件有界重发）。 */
static link_flow_t s_gone_flow;
static uint8_t s_device_gone_reason;
static bool s_gone_completed_valid;
static bool s_gone_completed_accepted;
static uint32_t s_gone_completed_event_id;
static uint32_t s_gone_completed_peer_generation;
/* M：同一启动会话内的物理鼠标连接周期代号，只用于诊断绑定。 */
static uint32_t s_mouse_connection_id = 1U;
/* 本轮恢复计时起点：各阶段共用同一份 10 秒预算。 */
static int64_t s_recovery_started_us;
static uint32_t s_gone_retries;
static uint32_t s_gone_failures;
static uint32_t s_request_retries;
static uint32_t s_offer_retries;
static uint32_t s_commit_replays;
static uint32_t s_budget_exhausted;

static esp_err_t enqueue_item(uint8_t type, const uint8_t *payload, uint8_t length);
static bool send_status_frame(uint8_t type, const uint8_t *payload, uint8_t length);
static bool send_profile_commit_frame(void);

static uint32_t next_flow_id(void)
{
    uint32_t value = __atomic_add_fetch(&s_next_flow_id, 1U, __ATOMIC_RELAXED);
    if (value == 0U) {
        value = __atomic_add_fetch(&s_next_flow_id, 1U, __ATOMIC_RELAXED);
    }
    return value;
}

/* 清理屏障：QUEUED（受门控未发出）与 PENDING 都算未完成。 */
static bool device_gone_barrier_pending(void)
{
    return link_flow_queued(&s_gone_flow) || link_flow_pending(&s_gone_flow);
}

static bool device_gone_barrier_failed(void)
{
    return s_gone_flow.state == LINK_FLOW_FAILED;
}

/* 触发一次新的恢复流程计时；唤醒词是物理/会话事件，不是轮询。 */
static void recovery_note_trigger(int64_t now_us)
{
    s_recovery_started_us = now_us;
}

static int64_t recovery_stage_timeout(int64_t now_us, int64_t desired_us)
{
    return link_recovery_stage_timeout_us(s_recovery_started_us, now_us, desired_us);
}

/*
 * 低频事务日志：只在状态转移时打印，不记录高频鼠标帧。
 * 字段顺序固定，便于现场按 generation/event/flow/transfer ID 对齐两板。
 */
static void log_recovery(const char *stage, const char *detail)
{
    ESP_LOGW(TAG, "恢复事务[%s] role=%u gen=%" PRIu32 " peer_gen=%" PRIu32
             " conn=%" PRIu32 " event=%" PRIu32 " flow=%" PRIu32
             " transfer=%" PRIu32 " budget_left_ms=%" PRId64 " %s",
             stage, (unsigned)s_role, s_generation,
             s_peer_generation_initialized ? s_peer_generation : 0U,
             s_mouse_connection_id,
             s_gone_flow.id, s_request_flow.id != 0U ? s_request_flow.id :
                 (s_offer_flow.id != 0U ? s_offer_flow.id : s_commit_flow.id),
             s_profile_transfer_id,
             link_recovery_stage_timeout_us(
                 s_recovery_started_us, esp_timer_get_time(), LINK_RECOVERY_BUDGET_US) / 1000,
             detail != NULL ? detail : "");
}

static uint32_t read_u32_le(const uint8_t *value)
{
    return (uint32_t)value[0] |
        ((uint32_t)value[1] << 8) |
        ((uint32_t)value[2] << 16) |
        ((uint32_t)value[3] << 24);
}

static void write_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static bool is_vendor_hid_message(uint8_t type)
{
    return type == DUAL_MESSAGE_RAW_HID_INPUT ||
        type == DUAL_MESSAGE_HID_SET_REPORT ||
        type == DUAL_MESSAGE_HID_GET_REPORT_REQUEST ||
        type == DUAL_MESSAGE_HID_GET_REPORT_RESPONSE;
}

static uint32_t vendor_hid_session_generation(void)
{
    return __atomic_load_n(&s_vendor_hid_session_generation, __ATOMIC_ACQUIRE);
}

void dual_uart1_cancel_vendor_hid_session(void)
{
    uint32_t current = vendor_hid_session_generation();
    while (true) {
        uint32_t next = current + 1U;
        if (next == 0U) {
            next = 1U;
        }
        if (__atomic_compare_exchange_n(&s_vendor_hid_session_generation,
                                        &current, next, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return;
        }
    }
}

static bool send_tx_item(const tx_item_t *item)
{
    if (item != NULL && item->type == DUAL_MESSAGE_RAW_HID_INPUT &&
        item->enqueued_us != 0) {
        const int64_t residence = esp_timer_get_time() - item->enqueued_us;
        if (residence > s_raw_tx_latency_peak_us) {
            s_raw_tx_latency_peak_us = residence;
        }
    }
    if (item == NULL) {
        return false;
    }
    if (is_vendor_hid_message(item->type) &&
        item->vendor_session_generation != vendor_hid_session_generation()) {
        ++s_vendor_queue_drops;
        return false;
    }
    return send_status_frame(item->type, item->payload, item->length);
}

static void update_tx_queue_metrics(void)
{
    UBaseType_t current = 0;
    if (s_tx_queue != NULL) {
        current += uxQueueMessagesWaiting(s_tx_queue);
    }
    if (s_safety_tx_queue != NULL) {
        current += uxQueueMessagesWaiting(s_safety_tx_queue);
    }
    if (s_software_tx_queue != NULL) {
        current += uxQueueMessagesWaiting(s_software_tx_queue);
    }
    if (s_vendor_tx_queue != NULL) {
        current += uxQueueMessagesWaiting(s_vendor_tx_queue);
    }
    s_tx_queue_current = (uint32_t)current;
    if (s_tx_queue_current > s_tx_queue_peak) {
        s_tx_queue_peak = s_tx_queue_current;
    }
}

static void reset_tx_queues(void)
{
    if (s_tx_queue != NULL) {
        xQueueReset(s_tx_queue);
    }
    if (s_safety_tx_queue != NULL) {
        xQueueReset(s_safety_tx_queue);
    }
    if (s_software_tx_queue != NULL) {
        xQueueReset(s_software_tx_queue);
    }
    if (s_vendor_tx_queue != NULL) {
        xQueueReset(s_vendor_tx_queue);
    }
    update_tx_queue_metrics();
}

static void notify_tx_task(void)
{
    if (s_tx_task != NULL) {
        xTaskNotifyGive(s_tx_task);
    }
}

static void queue_fault(void)
{
    /* Drop stale motion, then give the callback one chance to enqueue release. */
    if (s_fault_in_progress) {
        return;
    }
    s_fault_in_progress = true;
    reset_tx_queues();
    if (s_fault_callback != NULL) {
        s_fault_callback();
    }
    s_fault_in_progress = false;
}

static bool peer_sequence_is_new(uint16_t sequence)
{
    if (!s_peer_sequence_initialized) {
        s_peer_last_sequence = sequence;
        s_peer_sequence_initialized = true;
        return true;
    }
    const uint16_t distance = (uint16_t)(sequence - s_peer_last_sequence);
    if (distance == 0 || distance >= 0x8000U) {
        return false;
    }
    s_peer_last_sequence = sequence;
    return true;
}

static bool accept_peer_frame(const dual_frame_t *frame)
{
    if (frame->type == DUAL_MESSAGE_LINK_HELLO) {
        if (frame->payload_length != LINK_HELLO_LENGTH) {
            ++s_crc_or_frame_errors;
            return false;
        }
        const uint8_t peer_role = frame->payload[0];
        if (peer_role > DUAL_ROLE_MOUSE_HOST) {
            ++s_rejected_peers;
            return false;
        }
        if (frame->payload[1] > DUAL_USB_STATE_ERROR) {
            ++s_rejected_peers;
            return false;
        }
        /*
         * 对端未上电时它的 TX 是高阻，本板 TX 会串回自己的 RX，于是收到携带
         * 本机 node ID 的“自己的帧”。这类帧必须静默丢弃：既不算对端拒绝，
         * 也不打日志——否则每 250 ms 刷一条“拒绝UART1对端”，把 reject 计数
         * 和故障诊断全带偏（2026-09-25 曾据此误判为两板锁成同一角色）。
         */
        if (memcmp(&frame->payload[2], s_node_id, sizeof(s_node_id)) == 0) {
            ++s_self_frames;
            return false;
        }
        if (peer_role != DUAL_ROLE_UNRESOLVED &&
            s_role != DUAL_ROLE_UNRESOLVED && peer_role == s_role) {
            ++s_rejected_peers;
            ESP_LOGW(TAG, "拒绝UART1对端：相同角色 role=%u", (unsigned)s_role);
            return false;
        }
        const uint32_t peer_generation = read_u32_le(&frame->payload[8]);
        const bool had_peer_generation = s_peer_generation_initialized;
        const bool peer_generation_changed = !had_peer_generation ||
            peer_generation != s_peer_generation;
        const uint8_t previous_peer_usb_state = s_peer_usb_state;
        s_peer_role = peer_role;
        s_peer_usb_state = frame->payload[1];
        /*
         * 对端报告物理 HID 已连上：此前“在等鼠标”的等待可以结束，恢复常规的
         * OFFER 等待（重放申请由 M 的 OFFER 或下一次超时推进）。
         */
        if (s_role == DUAL_ROLE_PC_DEVICE && s_profile_request_waiting_device &&
            frame->payload[1] == DUAL_USB_STATE_HID_CONNECTED &&
            previous_peer_usb_state != DUAL_USB_STATE_HID_CONNECTED) {
            taskENTER_CRITICAL(&s_profile_mux);
            s_profile_request_waiting_device = false;
            s_profile_request_waiting_offer = !s_profile_request_offer_seen;
            s_profile_request_accepted_us = esp_timer_get_time();
            s_request_offer_retries = 0;
            taskEXIT_CRITICAL(&s_profile_mux);
            ESP_LOGI(TAG, "鼠标侧已枚举到物理鼠标：结束等待，继续等配置提议");
        }
        const bool new_generation = !s_peer_online || peer_generation_changed;
        if (new_generation) {
            /* P must re-run its local USB teardown before requesting a Profile
             * from a genuinely new M session.  The first peer is covered by
             * start_pc_role()'s synchronous probe-device teardown. */
            if (s_role == DUAL_ROLE_PC_DEVICE && had_peer_generation &&
                peer_generation_changed) {
                s_profile_request_cleanup_ready = false;
            }
            dual_status_led_set_flow_error(false);
            s_role_ack_logged = false;
            s_peer_sequence_initialized = false;
            s_peer_generation = peer_generation;
            s_peer_generation_initialized = true;
            taskENTER_CRITICAL(&s_profile_mux);
            /* 对端新会话：所有绑定旧会话的事务一律作废，并重新开始计时。 */
            link_flow_reset(&s_request_flow);
            s_profile_request_waiting_offer = false;
            s_profile_request_waiting_device = false;
            s_profile_request_offer_seen = false;
            s_profile_request_accepted_us = 0;
            s_request_offer_retries = 0;
            link_flow_reset(&s_commit_flow);
            s_profile_commit_receipt_seen = false;
            s_profile_retransmit_attempts = 0;
            if (s_role == DUAL_ROLE_MOUSE_HOST) {
                if (peer_generation_changed) {
                    /* P 已重启并发布新 generation；它的旧 USB 会话已消失，
                     * 旧 GONE 屏障不再适用于这个接收端会话。 */
                    link_flow_reset(&s_gone_flow);
                    s_gone_completed_valid = false;
                    s_gone_completed_accepted = false;
                    s_gone_completed_event_id = 0U;
                    s_gone_completed_peer_generation = 0U;
                } else if (device_gone_barrier_pending()) {
                    /* 同一 P 会话只是 UART 暂时离线，幂等重发同一 GONE。 */
                    link_flow_request_resend(&s_gone_flow);
                }
            }
            /* 新会话由 P 的一次 REQUEST 主动触发重新采集。旧会话未确认的
             * 缓存 Profile 不能自行 OFFER，否则会与 REQUEST 形成双克隆。 */
            link_flow_reset(&s_offer_flow);
            s_profile_pending = false;
            s_profile_phase = LINK_PROFILE_PHASE_IDLE;
            s_profile_offset = 0;
            s_profile_peer_generation_valid = false;
            s_profile_last_commit_us = 0;
            taskEXIT_CRITICAL(&s_profile_mux);
            recovery_note_trigger(esp_timer_get_time());
        }
        s_peer_online = true;
        s_last_peer_rx_us = esp_timer_get_time();
        dual_status_led_set_peer_connected(true);
        if (peer_role != DUAL_ROLE_UNRESOLVED && s_role != DUAL_ROLE_UNRESOLVED) {
            uint8_t ack[5] = {peer_role};
            write_u32_le(&ack[1], peer_generation);
            (void)enqueue_item(DUAL_MESSAGE_ROLE_ACK, ack, sizeof(ack));
        }
        return true;
    }
    if (!s_peer_online || !peer_sequence_is_new(frame->sequence)) {
        return false;
    }
    if (frame->type == DUAL_MESSAGE_LINK_PING &&
        (frame->payload_length != LINK_PING_LENGTH ||
         !s_peer_generation_initialized ||
         read_u32_le(frame->payload) != s_peer_generation)) {
        ++s_rejected_peers;
        return false;
    }
    s_last_peer_rx_us = esp_timer_get_time();
    if (s_role == DUAL_ROLE_PC_DEVICE &&
            (frame->type == DUAL_MESSAGE_PROFILE_OFFER ||
         frame->type == DUAL_MESSAGE_PROFILE_BEGIN ||
         frame->type == DUAL_MESSAGE_DEVICE_GONE)) {
        s_profile_request_cleanup_ready = false;
    }
    if (s_role == DUAL_ROLE_PC_DEVICE &&
        (frame->type == DUAL_MESSAGE_PROFILE_OFFER ||
         frame->type == DUAL_MESSAGE_PROFILE_BEGIN)) {
        /* OFFER/BEGIN 就是本次申请的结果：同一 flow 不再重放申请。 */
        taskENTER_CRITICAL(&s_profile_mux);
        s_profile_request_offer_seen = true;
        s_profile_request_waiting_offer = false;
        s_profile_request_waiting_device = false;
        s_request_offer_retries = 0;
        if (link_flow_incomplete(&s_request_flow)) {
            s_request_flow.state = LINK_FLOW_ACCEPTED;
            s_request_flow.resend_requested = false;
        }
        taskEXIT_CRITICAL(&s_profile_mux);
    }
    if (frame->type == DUAL_MESSAGE_PROFILE_ACK &&
        frame->payload_length == DUAL_LINK_PROFILE_ACK_LENGTH &&
        s_role == DUAL_ROLE_MOUSE_HOST) {
        const uint32_t transfer_id = read_u32_le(
            &frame->payload[DUAL_LINK_PROFILE_ACK_TRANSFER_ID_OFFSET]);
        const uint32_t crc32 = read_u32_le(
            &frame->payload[DUAL_LINK_PROFILE_ACK_CRC32_OFFSET]);
        const uint8_t status =
            frame->payload[DUAL_LINK_PROFILE_ACK_STATUS_OFFSET];
        const uint32_t recipient_generation = read_u32_le(
            &frame->payload[DUAL_LINK_PROFILE_ACK_RECIPIENT_GENERATION_OFFSET]);
        const uint32_t sender_generation = read_u32_le(
            &frame->payload[DUAL_LINK_PROFILE_ACK_SENDER_GENERATION_OFFSET]);
        bool matched = false;
        bool accepted = false;
        bool stale_session = false;
        bool repeated = false;
        const int64_t now_us = esp_timer_get_time();
        taskENTER_CRITICAL(&s_profile_mux);
        matched = link_profile_ack_is_for_session(
            recipient_generation, sender_generation, s_generation,
            s_peer_generation_initialized, s_peer_generation,
            transfer_id, crc32, s_profile_transfer_id, s_profile_crc32);
        /* 重复到达的同一确认只刷新 LED，不再重复更新状态或打印。 */
        repeated = matched && s_commit_flow.state == LINK_FLOW_ACCEPTED;
        stale_session = !matched && transfer_id == s_profile_transfer_id &&
            s_profile_transfer_id != 0U;
        if (matched && !repeated) {
            accepted = status == 0U;
            if (accepted) {
                (void)link_flow_ack(&s_commit_flow, true);
                s_profile_pending = false;
                s_profile_commit_receipt_seen = false;
                s_profile_phase = LINK_PROFILE_PHASE_IDLE;
                s_profile_last_commit_us = now_us;
                s_profile_retransmit_attempts = 0;
            } else if (status == 1U) {
                /* 不可克隆/挂载失败属于终态：不重试，明确终止本轮。 */
                s_commit_flow.state = LINK_FLOW_FAILED;
                s_profile_pending = false;
                s_profile_commit_receipt_seen = false;
                s_profile_phase = LINK_PROFILE_PHASE_IDLE;
                s_profile_last_commit_us = 0;
            } else {
                /* 未知状态按可重试处理：重放同一 transfer 的 COMMIT。 */
                link_flow_request_resend(&s_commit_flow);
            }
        }
        taskEXIT_CRITICAL(&s_profile_mux);
        if (matched && !repeated) {
            dual_status_led_set_flow_error(!accepted);
            if (link_profile_ack_updates_hid_state(true, accepted)) {
                /* M 侧只有绑定当前活动传输的成功确认才能推进 HID 状态。 */
                dual_uart1_set_usb_state(DUAL_USB_STATE_HID_CONNECTED);
                ESP_LOGI(TAG, "收到Profile %s：transfer=%" PRIu32 " crc=%08" PRIX32
                         " conn=%" PRIu32 "；本端HID状态更新为已连接",
                         "ACK", transfer_id, crc32, s_mouse_connection_id);
            } else {
                ESP_LOGE(TAG, "收到Profile NACK：transfer=%" PRIu32 " crc=%08" PRIX32
                         " status=%u；终止本轮克隆", transfer_id, crc32, (unsigned)status);
            }
            log_recovery(accepted ? "profile_ack" : "profile_nack", NULL);
        } else if (repeated) {
            dual_status_led_set_flow_error(false);
        } else if (stale_session) {
            ESP_LOGW(TAG, "丢弃旧会话的Profile确认：transfer=%" PRIu32
                     " recipient=%" PRIu32 " sender=%" PRIu32,
                     transfer_id, recipient_generation, sender_generation);
        }
        return true;
    }
    if (frame->type == DUAL_MESSAGE_ROLE_ACK && frame->payload_length == 5U) {
        const uint8_t acknowledged_role = frame->payload[0];
        const uint32_t acknowledged_generation = read_u32_le(&frame->payload[1]);
        if (!s_role_ack_logged && acknowledged_role == s_role &&
            acknowledged_generation == s_generation) {
            s_role_ack_logged = true;
            ESP_LOGI(TAG, "对端已确认本板身份：role=%u generation=%" PRIu32,
                     (unsigned)s_role, s_generation);
            notify_tx_task();
        }
        return true;
    }
    if (frame->type == DUAL_MESSAGE_FLOW_ACK &&
        frame->payload_length == DUAL_LINK_FLOW_ACK_LENGTH) {
        const uint8_t acknowledged_type =
            frame->payload[DUAL_LINK_FLOW_ACK_TYPE_OFFSET];
        const uint32_t flow_id =
            read_u32_le(&frame->payload[DUAL_LINK_FLOW_ACK_FLOW_ID_OFFSET]);
        const uint8_t status =
            frame->payload[DUAL_LINK_FLOW_ACK_STATUS_OFFSET];
        const uint32_t recipient_generation = read_u32_le(
            &frame->payload[DUAL_LINK_FLOW_ACK_RECIPIENT_GENERATION_OFFSET]);
        const uint32_t sender_generation = read_u32_le(
            &frame->payload[DUAL_LINK_FLOW_ACK_SENDER_GENERATION_OFFSET]);
        if (recipient_generation != s_generation ||
            !s_peer_generation_initialized ||
            sender_generation != s_peer_generation) {
            ++s_rejected_peers;
            ESP_LOGW(TAG, "拒收确认帧：type=%02X flow=%" PRIu32
                     " target_generation=%" PRIu32 " local_generation=%" PRIu32
                     " sender_generation=%" PRIu32 " peer_generation=%" PRIu32,
                     acknowledged_type, flow_id, recipient_generation, s_generation,
                     sender_generation, s_peer_generation);
            return false;
        }
        if (acknowledged_type == DUAL_MESSAGE_PROFILE_REQUEST &&
            s_role == DUAL_ROLE_PC_DEVICE) {
            bool matched = false;
            bool accepted = false;
            bool waiting_device = false;
            bool duplicate = false;
            const int64_t now_us = esp_timer_get_time();
            taskENTER_CRITICAL(&s_profile_mux);
            matched = link_flow_matches(&s_request_flow,
                DUAL_MESSAGE_PROFILE_REQUEST, flow_id, sender_generation);
            /* 已接受事务的重复确认不再改变状态，也不再重复打印。 */
            duplicate = matched && link_flow_completed(&s_request_flow);
            if (matched && !duplicate) {
                waiting_device = status == DUAL_FLOW_STATUS_WAITING_DEVICE;
                accepted = status == DUAL_FLOW_STATUS_ACCEPTED || waiting_device;
                if (accepted) {
                    if (link_flow_pending(&s_request_flow)) {
                        (void)link_flow_ack(&s_request_flow, true);
                    } else {
                        /* 迟到确认：本轮申请已经放弃，但仍然接受这次受理。 */
                        s_request_flow.state = LINK_FLOW_ACCEPTED;
                        s_request_flow.resend_requested = false;
                    }
                    /*
                     * “已受理但在等物理鼠标”不该进入重放循环：M 会在鼠标到达后
                     * 主动发 OFFER，P 这边重放只会让 M 反复回同一条确认。
                     */
                    s_profile_request_waiting_device = waiting_device;
                    s_profile_request_waiting_offer =
                        !waiting_device && !s_profile_request_offer_seen;
                    s_profile_request_accepted_us = now_us;
                    s_request_offer_retries = 0;
                } else {
                    /* M 本轮无法受理：保留同一 flow 身份，按预算重放申请。 */
                    link_flow_request_resend(&s_request_flow);
                }
            }
            taskEXIT_CRITICAL(&s_profile_mux);
            if (matched && !duplicate) {
                dual_status_led_set_flow_error(!accepted);
                if (waiting_device) {
                    ESP_LOGW(TAG, "鼠标侧已受理申请但物理鼠标尚未枚举：保持等待，"
                             "不再重放也不再判失败；flow=%" PRIu32, flow_id);
                    log_recovery("request_waiting_device", NULL);
                } else if (accepted) {
                    ESP_LOGI(TAG, "Profile请求接收确认：flow=%" PRIu32 " status=%u",
                             flow_id, (unsigned)status);
                } else {
                    ++s_request_retries;
                    ESP_LOGW(TAG, "Profile请求被拒，按同一flow重试：flow=%" PRIu32,
                             flow_id);
                }
                log_recovery(accepted ? "request_accepted" : "request_nacked", NULL);
                notify_tx_task();
            }
        } else if (acknowledged_type == DUAL_MESSAGE_PROFILE_COMMIT &&
                   s_role == DUAL_ROLE_MOUSE_HOST) {
            bool matched = false;
            bool receipt_ok = false;
            bool duplicate = false;
            taskENTER_CRITICAL(&s_profile_mux);
            matched = link_flow_matches(&s_commit_flow,
                DUAL_MESSAGE_PROFILE_COMMIT, flow_id, sender_generation);
            duplicate = matched && s_commit_flow.state == LINK_FLOW_ACCEPTED;
            if (matched && !duplicate) {
                /* 数据接收确认只解除“等价重放”的一半；最终挂载结果仍要等
                 * PROFILE_ACK，因此事务继续 PENDING。 */
                receipt_ok = status == DUAL_FLOW_STATUS_ACCEPTED;
                s_profile_commit_receipt_seen = receipt_ok;
                if (!receipt_ok) {
                    link_flow_request_resend(&s_commit_flow);
                }
            }
            taskEXIT_CRITICAL(&s_profile_mux);
            if (matched && !duplicate) {
                dual_status_led_set_flow_error(!receipt_ok);
                ESP_LOGI(TAG, "Profile传输接收确认：transfer=%" PRIu32 " status=%u",
                         flow_id, (unsigned)status);
                log_recovery(receipt_ok ? "commit_accepted" : "commit_nacked", NULL);
                notify_tx_task();
            }
        } else if (acknowledged_type == DUAL_MESSAGE_PROFILE_OFFER &&
                   s_role == DUAL_ROLE_MOUSE_HOST) {
            bool matched = false;
            bool accepted = false;
            bool retry = false;
            bool duplicate = false;
            taskENTER_CRITICAL(&s_profile_mux);
            matched = link_flow_matches(&s_offer_flow,
                DUAL_MESSAGE_PROFILE_OFFER, flow_id, sender_generation);
            /* 已接受事务的重复确认不能让传输重新从头开始。 */
            duplicate = matched && s_offer_flow.state == LINK_FLOW_ACCEPTED;
            if (matched && !duplicate) {
                accepted = status == DUAL_FLOW_STATUS_ACCEPTED &&
                    dual_disconnect_barrier_allows_profile_offer(
                        device_gone_barrier_pending(),
                        device_gone_barrier_failed());
                if (accepted) {
                    s_offer_flow.state = LINK_FLOW_ACCEPTED;
                    s_offer_flow.resend_requested = false;
                    /* 清理确认已经到位：现在才允许发送 Profile 分片。 */
                    s_profile_pending = true;
                    s_profile_phase = LINK_PROFILE_PHASE_BEGIN;
                    s_profile_offset = 0;
                    s_profile_peer_generation_valid = false;
                } else {
                    /* P 清理失败或屏障未完成：保持克隆门关闭，同一事务有界重试。 */
                    link_flow_request_resend(&s_offer_flow);
                    retry = true;
                }
            }
            taskEXIT_CRITICAL(&s_profile_mux);
            if (matched && !duplicate) {
                dual_status_led_set_flow_error(!accepted);
                if (retry) {
                    ++s_offer_retries;
                    ESP_LOGW(TAG, "Profile提议未获接受，按同一flow重试：flow=%" PRIu32
                             " status=%u", flow_id, (unsigned)status);
                    log_recovery("offer_retry", NULL);
                } else {
                    ESP_LOGI(TAG, "Profile提议接收确认：flow=%" PRIu32 " status=%u",
                             flow_id, (unsigned)status);
                    log_recovery("offer_accepted", NULL);
                }
                notify_tx_task();
            } else if (duplicate) {
                /* 对端重复确认同一提议：只刷新 LED，不重启传输。 */
                dual_status_led_set_flow_error(false);
            }
        } else if (acknowledged_type == DUAL_MESSAGE_DEVICE_GONE &&
                   s_role == DUAL_ROLE_MOUSE_HOST) {
            bool matched = false;
            bool accepted = false;
            bool retry = false;
            bool revived = false;
            bool duplicate = false;
            taskENTER_CRITICAL(&s_profile_mux);
            matched = link_flow_matches(&s_gone_flow,
                DUAL_MESSAGE_DEVICE_GONE, flow_id, sender_generation);
            duplicate = matched && s_gone_flow.state == LINK_FLOW_ACCEPTED;
            if (matched && !duplicate) {
                accepted = status == DUAL_FLOW_STATUS_ACCEPTED;
                if (accepted) {
                    (void)link_flow_ack(&s_gone_flow, true);
                    s_gone_completed_valid = true;
                    s_gone_completed_accepted = true;
                    s_gone_completed_event_id = flow_id;
                    s_gone_completed_peer_generation = sender_generation;
                } else {
                    /* P 清理失败：保留同一 event 身份并安排有界重发，
                     * 不永久作废、也不拒绝同 generation 的后续恢复。 */
                    link_flow_request_resend(&s_gone_flow);
                    retry = true;
                }
            } else if (link_late_ack_matches_completed(
                           s_gone_completed_valid, s_gone_completed_peer_generation,
                           s_gone_completed_event_id, s_gone_completed_accepted,
                           sender_generation, flow_id) &&
                       status == DUAL_FLOW_STATUS_ACCEPTED) {
                /* 迟到的有效确认：本地一次等待截止不能让它永久失效。 */
                s_gone_completed_accepted = true;
                if (s_gone_flow.state == LINK_FLOW_FAILED) {
                    s_gone_flow.state = LINK_FLOW_ACCEPTED;
                    s_gone_flow.resend_requested = false;
                    revived = true;
                }
                matched = true;
                accepted = true;
            }
            taskEXIT_CRITICAL(&s_profile_mux);
            if (matched && !duplicate) {
                dual_status_led_set_flow_error(!accepted);
                if (retry) {
                    ++s_gone_failures;
                    ESP_LOGW(TAG, "鼠标拔出清理被拒，按同一event重试：event=%" PRIu32,
                             flow_id);
                    log_recovery("gone_retry", NULL);
                } else {
                    ESP_LOGI(TAG, "鼠标拔出清理确认：event=%" PRIu32 " status=%u%s",
                             flow_id, (unsigned)status,
                             revived ? "（迟到确认重新开放克隆门）" : "");
                    log_recovery(revived ? "gone_late_ack" : "gone_accepted", NULL);
                }
                notify_tx_task();
            } else if (duplicate) {
                dual_status_led_set_flow_error(false);
            }
        }
        return true;
    }
    return true;
}

static void on_link_frame(const dual_frame_t *frame, void *context)
{
    (void)context;
    const bool trace_flow = frame->type == DUAL_MESSAGE_PROFILE_REQUEST ||
        frame->type == DUAL_MESSAGE_PROFILE_OFFER ||
        frame->type == DUAL_MESSAGE_FLOW_ACK;
    if (trace_flow) {
        ESP_LOGI(TAG, "接收克隆控制帧：type=%02X seq=%u peer_online=%u peer_role=%u peer_generation=%" PRIu32,
                 frame->type, frame->sequence, s_peer_online, s_peer_role,
                 s_peer_generation);
    }
    if (!accept_peer_frame(frame)) {
        if (trace_flow) {
            ESP_LOGW(TAG, "拒收克隆控制帧：type=%02X seq=%u peer_seq=%u initialized=%u",
                     frame->type, frame->sequence, s_peer_last_sequence,
                     s_peer_sequence_initialized);
        }
        return;
    }
    ++s_rx_count;
    if (frame->type == DUAL_MESSAGE_PHYSICAL_MOUSE) {
        ++s_rx_physical_count;
    } else if (frame->type == DUAL_MESSAGE_SOFTWARE_MOUSE) {
        ++s_rx_software_count;
    }
    if (s_frame_callback != NULL) {
        s_frame_callback(frame);
    }
}

static esp_err_t enqueue_item(uint8_t type, const uint8_t *payload, uint8_t length)
{
    if (s_tx_queue == NULL || s_safety_tx_queue == NULL ||
        s_software_tx_queue == NULL || s_vendor_tx_queue == NULL ||
        length > DUAL_PROXY_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_STATE;
    }
    tx_item_t item = {
        .type = type,
        .length = length,
        .enqueued_us = esp_timer_get_time(),
        .vendor_session_generation = is_vendor_hid_message(type) ?
            vendor_hid_session_generation() : 0U,
    };
    if (length > 0 && payload != NULL) {
        memcpy(item.payload, payload, length);
    }

    const bool safety = type == DUAL_MESSAGE_PHYSICAL_RELEASE ||
        type == DUAL_MESSAGE_SOFTWARE_RELEASE ||
        type == DUAL_MESSAGE_DEVICE_GONE ||
        type == DUAL_MESSAGE_FLOW_ACK;
    const bool software = type == DUAL_MESSAGE_SOFTWARE_MOUSE;
    const bool vendor = type == DUAL_MESSAGE_RAW_HID_INPUT ||
        type == DUAL_MESSAGE_HID_SET_REPORT ||
        type == DUAL_MESSAGE_HID_GET_REPORT_REQUEST ||
        type == DUAL_MESSAGE_HID_GET_REPORT_RESPONSE ||
        type == DUAL_MESSAGE_PROFILE_ACK ||
        type == DUAL_MESSAGE_ROLE_ACK;
    QueueHandle_t target = safety ? s_safety_tx_queue :
        software ? s_software_tx_queue :
        vendor ? s_vendor_tx_queue : s_tx_queue;
    if (xQueueSend(target, &item, 0) != pdTRUE) {
        ++s_tx_queue_overflows;
        if (safety) {
            /* Preserve the newest release even if stale releases accumulated. */
            const UBaseType_t discarded = uxQueueMessagesWaiting(s_tx_queue) +
                uxQueueMessagesWaiting(s_safety_tx_queue);
            reset_tx_queues();
            s_tx_queue_drops += (uint32_t)discarded;
            if (xQueueSend(s_safety_tx_queue, &item, 0) != pdTRUE) {
                ++s_tx_queue_drops;
            }
        } else if (software) {
            const UBaseType_t discarded = uxQueueMessagesWaiting(s_software_tx_queue);
            xQueueReset(s_software_tx_queue);
            s_tx_queue_drops += (uint32_t)discarded + 1U;
            const tx_item_t release = {
                .type = DUAL_MESSAGE_SOFTWARE_RELEASE,
                .length = 0,
            };
            if (xQueueSend(s_safety_tx_queue, &release, 0) != pdTRUE) {
                ++s_tx_queue_drops;
                queue_fault();
            }
        } else if (vendor) {
            ++s_vendor_queue_overflows;
            ++s_vendor_queue_drops;
        } else {
            ++s_tx_queue_drops;
            queue_fault();
        }
        update_tx_queue_metrics();
        notify_tx_task();
        return ESP_ERR_TIMEOUT;
    }
    update_tx_queue_metrics();
    notify_tx_task();
    return ESP_OK;
}

static bool send_status_frame(uint8_t type, const uint8_t *payload, uint8_t length)
{
    dual_frame_t frame = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = type,
        .sequence = s_next_sequence++,
        .payload_length = length,
    };
    if (length > 0) {
        memcpy(frame.payload, payload, length);
    }
    uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
    size_t serialized_length = 0;
    if (dual_frame_serialize(&frame, serialized, sizeof(serialized), &serialized_length) != ESP_OK) {
        return false;
    }
    if (uart_write_bytes(LINK_UART, serialized, serialized_length) == (int)serialized_length) {
        ++s_tx_count;
        if (type == DUAL_MESSAGE_PROFILE_REQUEST ||
            type == DUAL_MESSAGE_PROFILE_OFFER ||
            type == DUAL_MESSAGE_FLOW_ACK) {
            ESP_LOGI(TAG, "发送克隆控制帧：type=%02X seq=%u length=%u",
                     type, frame.sequence, length);
        }
        if (type == DUAL_MESSAGE_PHYSICAL_MOUSE) {
            ++s_tx_physical_count;
        } else if (type == DUAL_MESSAGE_SOFTWARE_MOUSE) {
            ++s_tx_software_count;
        }
        return true;
    } else {
        ++s_tx_write_failures;
        return false;
    }
}

/*
 * 重放同一 transfer 的 COMMIT。最终确认丢失时使用这条路径：
 * 电脑侧对同一 transfer 的重复 COMMIT 只补发结果，不会重新枚举 USB。
 */
static bool send_profile_commit_frame(void)
{
    uint32_t transfer_id;
    uint32_t total_length;
    uint32_t crc32;
    taskENTER_CRITICAL(&s_profile_mux);
    transfer_id = s_profile_transfer_id;
    total_length = s_profile_length;
    crc32 = s_profile_crc32;
    taskEXIT_CRITICAL(&s_profile_mux);
    if (transfer_id == 0U || total_length == 0U) {
        return false;
    }
    uint8_t payload[12];
    write_u32_le(&payload[0], transfer_id);
    write_u32_le(&payload[4], total_length);
    write_u32_le(&payload[8], crc32);
    if (!send_status_frame(DUAL_MESSAGE_PROFILE_COMMIT, payload, sizeof(payload))) {
        ++s_profile_failures;
        return false;
    }
    const int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_profile_mux);
    s_commit_flow.peer_generation = s_peer_generation;
    link_flow_mark_sent(&s_commit_flow, now_us);
    taskEXIT_CRITICAL(&s_profile_mux);
    return true;
}

static bool profile_stream_send_one(void)
{
    uint32_t transfer_id;
    uint32_t total_length;
    uint32_t crc32;
    uint32_t offset;
    uint8_t phase;
    uint8_t active_buffer;
    taskENTER_CRITICAL(&s_profile_mux);
    if (s_role != DUAL_ROLE_MOUSE_HOST || !s_profile_pending || !s_peer_online ||
        !dual_disconnect_barrier_allows_profile_offer(
            device_gone_barrier_pending(), device_gone_barrier_failed())) {
        taskEXIT_CRITICAL(&s_profile_mux);
        return false;
    }
    if (hid_profile_stream_needs_restart(
            s_profile_peer_generation_valid,
            s_profile_peer_generation,
            s_peer_generation)) {
        s_profile_peer_generation = s_peer_generation;
        s_profile_peer_generation_valid = true;
        s_profile_phase = LINK_PROFILE_PHASE_BEGIN;
        s_profile_offset = 0;
        ++s_profile_restarts;
    }
    transfer_id = s_profile_transfer_id;
    total_length = s_profile_length;
    crc32 = s_profile_crc32;
    offset = s_profile_offset;
    phase = s_profile_phase;
    active_buffer = s_profile_active_buffer;
    taskEXIT_CRITICAL(&s_profile_mux);

    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD] = {0};
    uint8_t payload_length = 0;
    if (phase == LINK_PROFILE_PHASE_BEGIN) {
        write_u32_le(&payload[0], transfer_id);
        write_u32_le(&payload[4], total_length);
        write_u32_le(&payload[8], crc32);
        payload_length = 12;
    } else if (phase == LINK_PROFILE_PHASE_CHUNK) {
        if (offset >= total_length) {
            taskENTER_CRITICAL(&s_profile_mux);
            if (s_profile_pending && s_profile_transfer_id == transfer_id) {
                s_profile_phase = LINK_PROFILE_PHASE_COMMIT;
            }
            taskEXIT_CRITICAL(&s_profile_mux);
            return false;
        }
        const size_t chunk_length = (total_length - offset) < HID_PROFILE_MAX_CHUNK_DATA
            ? (size_t)(total_length - offset) : HID_PROFILE_MAX_CHUNK_DATA;
        write_u32_le(&payload[0], transfer_id);
        write_u32_le(&payload[4], offset);
        xSemaphoreTake(s_profile_buffer_mutex, portMAX_DELAY);
        taskENTER_CRITICAL(&s_profile_mux);
        if (!s_profile_pending || s_profile_transfer_id != transfer_id ||
            s_profile_active_buffer != active_buffer || s_profile_offset != offset) {
            taskEXIT_CRITICAL(&s_profile_mux);
            xSemaphoreGive(s_profile_buffer_mutex);
            return false;
        }
        memcpy(&payload[HID_PROFILE_FRAME_CHUNK_HEADER],
               &s_profile_buffers[active_buffer][offset], chunk_length);
        taskEXIT_CRITICAL(&s_profile_mux);
        xSemaphoreGive(s_profile_buffer_mutex);
        payload_length = (uint8_t)(HID_PROFILE_FRAME_CHUNK_HEADER + chunk_length);
    } else if (phase == LINK_PROFILE_PHASE_COMMIT) {
        write_u32_le(&payload[0], transfer_id);
        write_u32_le(&payload[4], total_length);
        write_u32_le(&payload[8], crc32);
        payload_length = 12;
    } else {
        return false;
    }

    if (phase == LINK_PROFILE_PHASE_COMMIT) {
        /* COMMIT 发出即开启“最终确认”事务：FLOW_ACK(COMMIT) 与 PROFILE_ACK
         * 都落在这一个事务上，超时只做等价重放，不重新采集。 */
        const int64_t commit_us = esp_timer_get_time();
        link_flow_start(&s_commit_flow, DUAL_MESSAGE_PROFILE_COMMIT, transfer_id,
                        commit_us, LINK_COMMIT_MAX_ATTEMPTS,
                        LINK_COMMIT_RETRY_INTERVAL_US,
                        recovery_stage_timeout(commit_us, LINK_FLOW_ACK_TIMEOUT_US));
        s_commit_flow.peer_generation = s_peer_generation;
        s_profile_commit_receipt_seen = false;
        link_flow_mark_sent(&s_commit_flow, commit_us);
    }
    if (!send_status_frame(
            phase == LINK_PROFILE_PHASE_BEGIN ? DUAL_MESSAGE_PROFILE_BEGIN :
            phase == LINK_PROFILE_PHASE_CHUNK ? DUAL_MESSAGE_PROFILE_CHUNK :
            DUAL_MESSAGE_PROFILE_COMMIT,
            payload, payload_length)) {
        if (phase == LINK_PROFILE_PHASE_COMMIT) {
            taskENTER_CRITICAL(&s_profile_mux);
            s_commit_flow.state = LINK_FLOW_FAILED;
            s_profile_commit_receipt_seen = false;
            taskEXIT_CRITICAL(&s_profile_mux);
        }
        ++s_profile_failures;
        return false;
    }

    taskENTER_CRITICAL(&s_profile_mux);
    if (s_profile_pending && s_profile_transfer_id == transfer_id) {
        if (phase == LINK_PROFILE_PHASE_BEGIN) {
            s_profile_phase = LINK_PROFILE_PHASE_CHUNK;
            s_profile_offset = 0;
            ++s_profile_starts;
        } else if (phase == LINK_PROFILE_PHASE_CHUNK) {
            s_profile_offset += payload_length - HID_PROFILE_FRAME_CHUNK_HEADER;
            if (s_profile_offset >= s_profile_length) {
                s_profile_phase = LINK_PROFILE_PHASE_COMMIT;
            }
            ++s_profile_chunks;
        } else {
            s_profile_pending = false;
            s_profile_phase = LINK_PROFILE_PHASE_IDLE;
            s_profile_last_commit_us = esp_timer_get_time();
            ++s_profile_commits;
        }
    }
    taskEXIT_CRITICAL(&s_profile_mux);
    return true;
}

static void send_link_status(void)
{
    uint8_t hello[LINK_HELLO_LENGTH] = {0};
    hello[0] = s_role;
    hello[1] = s_usb_state;
    memcpy(&hello[2], s_node_id, sizeof(s_node_id));
    write_u32_le(&hello[8], s_generation);
    send_status_frame(DUAL_MESSAGE_LINK_HELLO, hello, sizeof(hello));

    uint8_t ping[LINK_PING_LENGTH] = {0};
    write_u32_le(ping, s_generation);
    /*
     * 记录心跳间隔峰值：对端（P）按静默超时判故障，所以"本端被卡多久"决定了
     * 会不会被误判成掉线。这里统计两次心跳之间的最大间隔，用于区分
     * "链路真断" 与 "本端任务被饿住"。
     */
    const int64_t ping_now_us = esp_timer_get_time();
    if (s_last_heartbeat_us != 0) {
        const uint32_t gap_ms = (uint32_t)((ping_now_us - s_last_heartbeat_us) / 1000);
        if (gap_ms > s_heartbeat_gap_peak_ms) {
            s_heartbeat_gap_peak_ms = gap_ms;
        }
    }
    s_last_heartbeat_us = ping_now_us;
    send_status_frame(DUAL_MESSAGE_LINK_PING, ping, sizeof(ping));
}

static bool tx_queues_have_items(void)
{
    return (s_safety_tx_queue != NULL && uxQueueMessagesWaiting(s_safety_tx_queue) > 0) ||
        (s_tx_queue != NULL && uxQueueMessagesWaiting(s_tx_queue) > 0) ||
        (s_software_tx_queue != NULL && uxQueueMessagesWaiting(s_software_tx_queue) > 0) ||
        (s_vendor_tx_queue != NULL && uxQueueMessagesWaiting(s_vendor_tx_queue) > 0);
}

static void link_tx_task(void *argument)
{
    (void)argument;
    int64_t last_status_us = 0;
    int64_t last_summary_us = 0;
    uint32_t last_tx_physical = 0;
    uint32_t last_rx_physical = 0;
    uint32_t last_tx_software = 0;
    uint32_t last_rx_software = 0;
    uint8_t vendor_turn = 0;
    while (true) {
        const int64_t now_us = esp_timer_get_time();
        /*
         * Only an empty pair of queues may enter the heartbeat wait.  If a
         * producer races this check, its notification is retained by the
         * counting task notification and the take returns immediately.
         * 未完成的事务（含重发窗口）同样视为有待办工作，不能睡过去。
         */
        bool profile_pending = false;
        bool gone_due = false;
        bool offer_due = false;
        bool commit_due = false;
        bool request_due = false;
        bool waiting_offer_due = false;
        taskENTER_CRITICAL(&s_profile_mux);
        profile_pending = s_role == DUAL_ROLE_MOUSE_HOST && s_profile_pending &&
            s_peer_online && dual_disconnect_barrier_allows_profile_offer(
                device_gone_barrier_pending(), device_gone_barrier_failed());
        const bool peer_ready_for_flow = s_peer_online && s_role_ack_logged &&
            s_peer_role == DUAL_ROLE_PC_DEVICE;
        gone_due = s_role == DUAL_ROLE_MOUSE_HOST && peer_ready_for_flow &&
            link_flow_action_due(&s_gone_flow, now_us);
        offer_due = s_role == DUAL_ROLE_MOUSE_HOST && peer_ready_for_flow &&
            link_flow_action_due(&s_offer_flow, now_us) &&
            dual_disconnect_barrier_allows_profile_offer(
                device_gone_barrier_pending(), device_gone_barrier_failed());
        commit_due = s_role == DUAL_ROLE_MOUSE_HOST && s_peer_online &&
            link_flow_action_due(&s_commit_flow, now_us);
        const bool request_completed = link_flow_completed(&s_request_flow) ||
            s_request_flow.state == LINK_FLOW_FAILED;
        request_due = s_role == DUAL_ROLE_PC_DEVICE &&
            s_peer_role == DUAL_ROLE_MOUSE_HOST && s_peer_online &&
            (link_flow_incomplete(&s_request_flow) ?
                link_flow_action_due(&s_request_flow, now_us) :
                dual_profile_request_may_send(
                    s_role, s_peer_online, s_role_ack_logged,
                    s_profile_request_cleanup_ready, false, request_completed));
        waiting_offer_due = s_role == DUAL_ROLE_PC_DEVICE && s_peer_online &&
            s_profile_request_waiting_offer && s_profile_request_accepted_us != 0 &&
            now_us - s_profile_request_accepted_us >= LINK_FLOW_RETRY_INTERVAL_US;
        taskEXIT_CRITICAL(&s_profile_mux);
        const bool tx_work_pending = tx_queues_have_items() || profile_pending ||
            gone_due || offer_due || commit_due || request_due || waiting_offer_due;
        if (dual_proxy_link_should_wait_for_notification(tx_work_pending)) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LINK_PERIOD_MS));
        }

        /* ---------- 鼠标拔出清理屏障：同一 event 有界重发 ---------- */
        bool send_device_gone = false;
        bool gone_give_up = false;
        bool gone_was_retry = false;
        uint32_t gone_event_id = 0;
        uint8_t device_gone[DUAL_LINK_DEVICE_GONE_LENGTH] = {0};
        taskENTER_CRITICAL(&s_profile_mux);
        if (s_role == DUAL_ROLE_MOUSE_HOST && peer_ready_for_flow) {
            const link_flow_action_t action = link_flow_poll(&s_gone_flow, now_us);
            if (action == LINK_FLOW_ACTION_SEND || action == LINK_FLOW_ACTION_RESEND) {
                gone_was_retry = action == LINK_FLOW_ACTION_RESEND;
                s_gone_flow.peer_generation = s_peer_generation;
                gone_event_id = s_gone_flow.id;
                write_u32_le(
                    &device_gone[DUAL_LINK_DEVICE_GONE_SENDER_GENERATION_OFFSET],
                    s_generation);
                write_u32_le(
                    &device_gone[DUAL_LINK_DEVICE_GONE_TARGET_GENERATION_OFFSET],
                    s_peer_generation);
                write_u32_le(
                    &device_gone[DUAL_LINK_DEVICE_GONE_EVENT_ID_OFFSET],
                    s_gone_flow.id);
                device_gone[DUAL_LINK_DEVICE_GONE_REASON_OFFSET] =
                    s_device_gone_reason;
                link_flow_mark_sent(&s_gone_flow, now_us);
                if (gone_was_retry) {
                    ++s_gone_retries;
                }
                send_device_gone = true;
            } else if (action == LINK_FLOW_ACTION_GIVE_UP) {
                s_gone_flow.state = LINK_FLOW_FAILED;
                s_gone_completed_valid = true;
                s_gone_completed_accepted = false;
                s_gone_completed_event_id = s_gone_flow.id;
                s_gone_completed_peer_generation = s_gone_flow.peer_generation;
                gone_event_id = s_gone_flow.id;
                gone_give_up = true;
            }
        }
        taskEXIT_CRITICAL(&s_profile_mux);
        if (send_device_gone && send_status_frame(
                DUAL_MESSAGE_DEVICE_GONE, device_gone, sizeof(device_gone))) {
            ESP_LOGW(TAG, "发送鼠标拔出清理屏障：event=%" PRIu32
                     " peer_generation=%" PRIu32 " attempt=%u",
                     gone_event_id,
                     read_u32_le(&device_gone[
                         DUAL_LINK_DEVICE_GONE_TARGET_GENERATION_OFFSET]),
                     (unsigned)s_gone_flow.attempts);
            log_recovery(gone_was_retry ? "gone_resend" : "gone_sent", NULL);
        } else if (send_device_gone) {
            taskENTER_CRITICAL(&s_profile_mux);
            s_gone_flow.state = LINK_FLOW_FAILED;
            s_gone_completed_valid = true;
            s_gone_completed_accepted = false;
            s_gone_completed_event_id = gone_event_id;
            s_gone_completed_peer_generation = s_gone_flow.peer_generation;
            taskEXIT_CRITICAL(&s_profile_mux);
            dual_status_led_set_flow_error(true);
            ESP_LOGE(TAG, "鼠标拔出清理屏障发送失败，停止克隆");
        }
        if (gone_give_up) {
            dual_status_led_set_flow_error(true);
            ++s_gone_failures;
            const bool exhausted =
                link_recovery_budget_exhausted(s_recovery_started_us, now_us);
            if (exhausted) {
                ++s_budget_exhausted;
            }
            ESP_LOGE(TAG, "鼠标拔出清理屏障在预算内未获确认，停止克隆：event=%" PRIu32
                     " attempts=%u", gone_event_id, (unsigned)s_gone_flow.attempts);
            log_recovery("gone_give_up",
                         exhausted ? "budget_exhausted" : "retry_exhausted");
        }

        /* ---------- P 侧：唯一一次 Profile 申请（同一 flow 有界重放） ---------- */
        if (s_role == DUAL_ROLE_PC_DEVICE && s_peer_role == DUAL_ROLE_MOUSE_HOST &&
            s_peer_online) {
            bool send_request = false;
            bool start_request = false;
            bool request_give_up = false;
            uint32_t request_flow_id = 0;
            taskENTER_CRITICAL(&s_profile_mux);
            if (link_flow_incomplete(&s_request_flow)) {
                const link_flow_action_t action = link_flow_poll(&s_request_flow, now_us);
                if (action == LINK_FLOW_ACTION_SEND ||
                    action == LINK_FLOW_ACTION_RESEND) {
                    request_flow_id = s_request_flow.id;
                    s_request_flow.peer_generation = s_peer_generation;
                    if (action == LINK_FLOW_ACTION_RESEND) {
                        ++s_request_retries;
                    }
                    link_flow_mark_sent(&s_request_flow, now_us);
                    send_request = true;
                } else if (action == LINK_FLOW_ACTION_GIVE_UP) {
                    s_request_flow.state = LINK_FLOW_FAILED;
                    request_give_up = true;
                }
            } else if (dual_profile_request_may_send(
                           s_role, s_peer_online, s_role_ack_logged,
                           s_profile_request_cleanup_ready, false,
                           link_flow_completed(&s_request_flow) ||
                               s_request_flow.state == LINK_FLOW_FAILED)) {
                request_flow_id = next_flow_id();
                link_flow_start(&s_request_flow, DUAL_MESSAGE_PROFILE_REQUEST,
                                request_flow_id, now_us, LINK_FLOW_MAX_ATTEMPTS,
                                LINK_FLOW_RETRY_INTERVAL_US,
                                recovery_stage_timeout(now_us, LINK_FLOW_ACK_TIMEOUT_US));
                s_request_flow.peer_generation = s_peer_generation;
                s_profile_request_offer_seen = false;
                s_profile_request_waiting_offer = false;
                s_request_offer_retries = 0;
                recovery_note_trigger(now_us);
                link_flow_mark_sent(&s_request_flow, now_us);
                send_request = true;
                start_request = true;
            }
            taskEXIT_CRITICAL(&s_profile_mux);
            if (send_request) {
                uint8_t request[DUAL_LINK_PROFILE_REQUEST_LENGTH];
                write_u32_le(&request[0], s_generation);
                write_u32_le(&request[4], request_flow_id);
                if (send_status_frame(DUAL_MESSAGE_PROFILE_REQUEST,
                                      request, sizeof(request))) {
                    ESP_LOGI(TAG, "发送Profile获取申请：flow=%" PRIu32 " attempt=%u",
                             request_flow_id, (unsigned)s_request_flow.attempts);
                    log_recovery(start_request ? "request_sent" : "request_resend",
                                 NULL);
                } else {
                    taskENTER_CRITICAL(&s_profile_mux);
                    s_request_flow.state = LINK_FLOW_FAILED;
                    taskEXIT_CRITICAL(&s_profile_mux);
                    dual_status_led_set_flow_error(true);
                    ESP_LOGE(TAG, "Profile获取申请无法发出：flow=%" PRIu32,
                             request_flow_id);
                }
            }
            if (request_give_up) {
                dual_status_led_set_flow_error(true);
                ESP_LOGE(TAG, "Profile获取申请在预算内未获接收确认：flow=%" PRIu32,
                         s_request_flow.id);
                log_recovery("request_give_up", NULL);
            }
        }

        /* 申请已被受理但迟迟没有 OFFER：按同一 flow 重放申请，
         * M 侧按 flow ID 去重，不会重复采集。
         * 例外：M 明确回了“在等物理鼠标”，此时重放没有意义（M 只有在鼠标
         * 到达后才会发 OFFER），因此保持等待、不重放也不判失败。 */
        if (s_peer_online && s_profile_request_waiting_device &&
            s_profile_request_accepted_us != 0 &&
            now_us - s_profile_request_accepted_us >= LINK_WAITING_DEVICE_LOG_INTERVAL_US) {
            s_profile_request_accepted_us = now_us;
            ESP_LOGW(TAG, "仍在等待鼠标侧枚举物理鼠标：flow=%" PRIu32
                     " 已等待=%" PRId64 "ms（不判失败，鼠标到达后由 OFFER 推进）",
                     s_request_flow.id,
                     (now_us - s_recovery_started_us) / 1000);
        }
        if (s_peer_online && s_profile_request_waiting_offer &&
            !s_profile_request_waiting_device &&
            s_profile_request_accepted_us != 0 &&
            now_us - s_profile_request_accepted_us >= LINK_FLOW_RETRY_INTERVAL_US) {
            bool replay = false;
            bool waiting_give_up = false;
            taskENTER_CRITICAL(&s_profile_mux);
            if (s_request_offer_retries < LINK_FLOW_MAX_ATTEMPTS &&
                !link_recovery_budget_exhausted(s_recovery_started_us, now_us)) {
                ++s_request_offer_retries;
                ++s_request_retries;
                s_request_flow.state = LINK_FLOW_PENDING;
                s_request_flow.resend_requested = true;
                replay = true;
            } else {
                s_request_flow.state = LINK_FLOW_FAILED;
                waiting_give_up = true;
            }
            s_profile_request_waiting_offer = false;
            s_profile_request_offer_seen = false;
            taskEXIT_CRITICAL(&s_profile_mux);
            if (replay) {
                ESP_LOGW(TAG, "申请已受理但未收到提议，重放同一申请：flow=%" PRIu32
                         " retry=%u", s_request_flow.id,
                         (unsigned)s_request_offer_retries);
                log_recovery("request_replay", NULL);
                notify_tx_task();
            } else if (waiting_give_up) {
                dual_status_led_set_flow_error(true);
                ESP_LOGE(TAG, "Profile申请已受理但始终未收到鼠标侧的配置提议");
                log_recovery("request_no_offer", "retry_exhausted");
            }
        }

        /* ---------- M 侧：Profile 克隆提议（同一 flow 有界重发） ---------- */
        bool send_offer = false;
        bool offer_give_up = false;
        bool offer_was_retry = false;
        uint32_t offer_flow_id = 0;
        uint32_t offer_transfer_id = 0;
        uint32_t offer_crc32 = 0;
        taskENTER_CRITICAL(&s_profile_mux);
        if (s_role == DUAL_ROLE_MOUSE_HOST && peer_ready_for_flow &&
            dual_disconnect_barrier_allows_profile_offer(
                device_gone_barrier_pending(), device_gone_barrier_failed())) {
            const link_flow_action_t action = link_flow_poll(&s_offer_flow, now_us);
            if (action == LINK_FLOW_ACTION_SEND || action == LINK_FLOW_ACTION_RESEND) {
                offer_was_retry = action == LINK_FLOW_ACTION_RESEND;
                offer_flow_id = s_offer_flow.id;
                offer_transfer_id = s_profile_transfer_id;
                offer_crc32 = s_profile_crc32;
                s_offer_flow.peer_generation = s_peer_generation;
                link_flow_mark_sent(&s_offer_flow, now_us);
                if (offer_was_retry) {
                    ++s_offer_retries;
                }
                send_offer = true;
            } else if (action == LINK_FLOW_ACTION_GIVE_UP) {
                s_offer_flow.state = LINK_FLOW_FAILED;
                s_profile_pending = false;
                s_profile_phase = LINK_PROFILE_PHASE_IDLE;
                offer_give_up = true;
            }
        }
        taskEXIT_CRITICAL(&s_profile_mux);
        if (send_offer) {
            uint8_t offer[DUAL_LINK_PROFILE_OFFER_LENGTH];
            write_u32_le(&offer[0], offer_flow_id);
            write_u32_le(&offer[4], offer_transfer_id);
            write_u32_le(&offer[8], offer_crc32);
            if (send_status_frame(DUAL_MESSAGE_PROFILE_OFFER,
                                  offer, sizeof(offer))) {
                ESP_LOGI(TAG, "发送Profile克隆提议：flow=%" PRIu32 " attempt=%u",
                         offer_flow_id, (unsigned)s_offer_flow.attempts);
                log_recovery(offer_was_retry ? "offer_resend" : "offer_sent", NULL);
            } else {
                taskENTER_CRITICAL(&s_profile_mux);
                if (s_offer_flow.id == offer_flow_id) {
                    s_offer_flow.state = LINK_FLOW_FAILED;
                }
                taskEXIT_CRITICAL(&s_profile_mux);
                dual_status_led_set_flow_error(true);
                ESP_LOGE(TAG, "Profile克隆提议无法发出：flow=%" PRIu32, offer_flow_id);
            }
        }
        if (offer_give_up) {
            dual_status_led_set_flow_error(true);
            ESP_LOGE(TAG, "Profile克隆提议在预算内未获确认：flow=%" PRIu32,
                     s_offer_flow.id);
            log_recovery("offer_give_up", NULL);
        }

        /* ---------- M 侧：COMMIT 确认与最终挂载结果（等价重放） ---------- */
        link_flow_action_t commit_action = LINK_FLOW_ACTION_NONE;
        taskENTER_CRITICAL(&s_profile_mux);
        commit_action = link_flow_poll(&s_commit_flow, now_us);
        taskEXIT_CRITICAL(&s_profile_mux);
        if (commit_action == LINK_FLOW_ACTION_RESEND) {
            if (send_profile_commit_frame()) {
                ++s_commit_replays;
                ESP_LOGW(TAG, "Profile确认缺失，重放同一transfer的COMMIT：transfer=%" PRIu32
                         " attempt=%u", s_profile_transfer_id,
                         (unsigned)s_commit_flow.attempts);
                log_recovery("commit_replay", NULL);
            }
        } else if (commit_action == LINK_FLOW_ACTION_GIVE_UP) {
            if (s_profile_retransmit_attempts < LINK_PROFILE_RETRANSMIT_MAX) {
                ++s_profile_retransmit_attempts;
                ++s_profile_restarts;
                taskENTER_CRITICAL(&s_profile_mux);
                s_commit_flow.state = LINK_FLOW_IDLE;
                s_profile_commit_receipt_seen = false;
                if (s_role == DUAL_ROLE_MOUSE_HOST && s_peer_online) {
                    s_profile_pending = true;
                    s_profile_phase = LINK_PROFILE_PHASE_BEGIN;
                    s_profile_offset = 0;
                    s_profile_peer_generation_valid = false;
                }
                taskEXIT_CRITICAL(&s_profile_mux);
                /* 不重置恢复计时：重传属于同一轮恢复，必须共用剩余预算。 */
                dual_status_led_set_flow_error(true);
                ESP_LOGW(TAG, "Profile未获最终确认，按同一transfer整份重传：transfer=%" PRIu32
                         " retransmit=%u", s_profile_transfer_id,
                         (unsigned)s_profile_retransmit_attempts);
                log_recovery("profile_retransmit", NULL);
                notify_tx_task();
            } else {
                taskENTER_CRITICAL(&s_profile_mux);
                s_commit_flow.state = LINK_FLOW_FAILED;
                s_profile_pending = false;
                s_profile_phase = LINK_PROFILE_PHASE_IDLE;
                taskEXIT_CRITICAL(&s_profile_mux);
                ++s_profile_failures;
                dual_status_led_set_flow_error(true);
                ESP_LOGE(TAG, "Profile最终确认预算耗尽，终止本轮克隆：transfer=%" PRIu32,
                         s_profile_transfer_id);
                log_recovery("profile_give_up", "retry_exhausted");
            }
        }

        unsigned processed = 0;
        tx_item_t item;

        unsigned motion_processed = 0;
        while (processed < DUAL_PROXY_LINK_TX_BATCH_LIMIT &&
               xQueueReceive(s_safety_tx_queue, &item, 0) == pdTRUE) {
            (void)send_tx_item(&item);
            ++processed;
        }

        /* 实体输入先于软件输入；每批至少给两者各一个机会。 */
        if (processed < DUAL_PROXY_LINK_TX_BATCH_LIMIT &&
            xQueueReceive(s_tx_queue, &item, 0) == pdTRUE) {
            (void)send_tx_item(&item);
            if (item.type == DUAL_MESSAGE_PHYSICAL_MOUSE) {
                ++motion_processed;
            }
            ++processed;
        }
        if (processed < DUAL_PROXY_LINK_TX_BATCH_LIMIT &&
            xQueueReceive(s_software_tx_queue, &item, 0) == pdTRUE) {
            (void)send_tx_item(&item);
            ++processed;
        }

        /* Reserve a bounded slot for vendor/control traffic.  It has its own
         * queue, so a vendor burst can neither block nor evict motion, while
         * the turn counter prevents a continuously full motion queue from
         * starving HID++ transactions. */
        if ((vendor_turn++ & 0x03U) == 0U && processed < DUAL_PROXY_LINK_TX_BATCH_LIMIT &&
            xQueueReceive(s_vendor_tx_queue, &item, 0) == pdTRUE) {
            (void)send_tx_item(&item);
            ++processed;
        }

        if (last_status_us == 0 || now_us - last_status_us >= LINK_PERIOD_MS * 1000LL) {
            send_link_status();
            last_status_us = now_us;
        }

        while (processed < DUAL_PROXY_LINK_TX_BATCH_LIMIT &&
               xQueueReceive(s_tx_queue, &item, 0) == pdTRUE) {
            (void)send_tx_item(&item);
            if (item.type == DUAL_MESSAGE_PHYSICAL_MOUSE) {
                ++motion_processed;
            }
            ++processed;
            if (processed < DUAL_PROXY_LINK_TX_BATCH_LIMIT &&
                xQueueReceive(s_software_tx_queue, &item, 0) == pdTRUE) {
                (void)send_tx_item(&item);
                ++processed;
            }
        }

        while (processed < DUAL_PROXY_LINK_TX_BATCH_LIMIT &&
               xQueueReceive(s_software_tx_queue, &item, 0) == pdTRUE) {
            (void)send_tx_item(&item);
            ++processed;
        }

        if (processed < DUAL_PROXY_LINK_TX_BATCH_LIMIT &&
            xQueueReceive(s_vendor_tx_queue, &item, 0) == pdTRUE) {
            (void)send_tx_item(&item);
            ++processed;
        }

        if (motion_processed > 0) {
            const unsigned total = s_profile_motion_since_send + motion_processed;
            s_profile_motion_since_send = (uint8_t)(total > UINT8_MAX ? UINT8_MAX : total);
        }
        const bool safety_pending = s_safety_tx_queue != NULL &&
            uxQueueMessagesWaiting(s_safety_tx_queue) > 0;
        const bool motion_pending = s_tx_queue != NULL &&
            (uxQueueMessagesWaiting(s_tx_queue) > 0 ||
             uxQueueMessagesWaiting(s_software_tx_queue) > 0);
        if (hid_profile_stream_can_send(
                s_peer_online, safety_pending, motion_pending,
                s_profile_motion_since_send, LINK_PROFILE_FAIRNESS_LIMIT)) {
            if (profile_stream_send_one()) {
                s_profile_motion_since_send = 0;
            }
        }

        if (s_peer_online && now_us - s_last_peer_rx_us > LINK_TIMEOUT_MS * 1000LL) {
            s_peer_online = false;
            s_peer_role = DUAL_ROLE_UNRESOLVED;
            s_role_ack_logged = false;
            s_peer_sequence_initialized = false;
            /* 对端刚重启：清掉可能残留的半帧，并抓取接下来 96 个字节用于定位。 */
            uart_flush_input(LINK_UART);
            s_raw_dump_budget = 96U;
            taskENTER_CRITICAL(&s_profile_mux);
            /* 对端离线：所有在途事务停发但不丢身份，等新 HELLO 重新计时。 */
            if (link_flow_queued(&s_request_flow) || link_flow_pending(&s_request_flow)) {
                s_request_flow.resend_requested = false;
            }
            s_profile_request_waiting_offer = false;
            if (link_flow_pending(&s_offer_flow)) {
                s_offer_flow.resend_requested = false;
            }
            if (link_flow_pending(&s_commit_flow)) {
                s_commit_flow.resend_requested = false;
            }
            if (link_flow_pending(&s_gone_flow)) {
                s_gone_flow.resend_requested = false;
            }
            taskEXIT_CRITICAL(&s_profile_mux);
            dual_status_led_set_peer_connected(false);
            ESP_LOGW(TAG, "UART1对端超时，清理实体输入");
            log_recovery("peer_timeout", NULL);
            queue_fault();
        }

        update_tx_queue_metrics();
        if (last_summary_us == 0 || now_us - last_summary_us >= 1000000LL) {
            const uint32_t tx_physical_hz = s_tx_physical_count - last_tx_physical;
            const uint32_t rx_physical_hz = s_rx_physical_count - last_rx_physical;
            const uint32_t tx_software_hz = s_tx_software_count - last_tx_software;
            const uint32_t rx_software_hz = s_rx_software_count - last_rx_software;
            /* 对端静默时长用来说明“收不到帧”是真丢包还是对端本来就安静。 */
            if (s_last_peer_rx_us == 0 || now_us <= s_last_peer_rx_us) {
                s_peer_silence_ms = 0U;
            } else {
                s_peer_silence_ms = (uint32_t)((now_us - s_last_peer_rx_us) / 1000);
            }
            size_t rx_pending = 0U;
            if (uart_get_buffered_data_len(LINK_UART, &rx_pending) == ESP_OK) {
                if ((uint32_t)rx_pending > s_rx_pending_peak) {
                    s_rx_pending_peak = (uint32_t)rx_pending;
                }
            }
            ESP_LOGI(TAG, "UART1统计 tx=%" PRIu32 " rx=%" PRIu32
                     " rx_bytes=%" PRIu32 " frame_err=%" PRIu32
                     " peer_silence_ms=%" PRIu32
                     " phys_tx/rx_hz=%" PRIu32 "/%" PRIu32
                     " soft_tx/rx_hz=%" PRIu32 "/%" PRIu32
                     " reject=%" PRIu32 " self=%" PRIu32 " rx_ovf=%" PRIu32
                     " rx_pend_peak=%" PRIu32 " hb_gap_peak_ms=%" PRIu32 " raw_lat_peak_us=%" PRId64 " q=%" PRIu32 " peak=%" PRIu32
                     " overflow=%" PRIu32 " drop=%" PRIu32 " write_fail=%" PRIu32
                     " vendor_overflow=%" PRIu32 " vendor_drop=%" PRIu32
                     " peer=%s profile=%" PRIu32 "/%" PRIu32 "/%" PRIu32
                     " restart=%" PRIu32 " fail=%" PRIu32
                     " conn=%" PRIu32 " gone_retry=%" PRIu32 " gone_fail=%" PRIu32
                     " req_retry=%" PRIu32 " offer_retry=%" PRIu32
                     " commit_replay=%" PRIu32 " budget_exhausted=%" PRIu32,
                     s_tx_count, s_rx_count,
                     s_rx_bytes, s_crc_or_frame_errors, s_peer_silence_ms,
                     tx_physical_hz, rx_physical_hz,
                     tx_software_hz, rx_software_hz,
                     s_rejected_peers, s_self_frames, s_rx_overflows,
                     s_rx_pending_peak, s_heartbeat_gap_peak_ms, s_raw_tx_latency_peak_us, s_tx_queue_current,
                      s_tx_queue_peak, s_tx_queue_overflows, s_tx_queue_drops,
                      s_tx_write_failures, s_vendor_queue_overflows, s_vendor_queue_drops,
                      s_peer_online ? "online" : "offline",
                      s_profile_starts, s_profile_chunks, s_profile_commits,
                      s_profile_restarts, s_profile_failures,
                      s_mouse_connection_id, s_gone_retries, s_gone_failures,
                      s_request_retries, s_offer_retries, s_commit_replays,
                      s_budget_exhausted);
            last_tx_physical = s_tx_physical_count;
            last_rx_physical = s_rx_physical_count;
            last_tx_software = s_tx_software_count;
            last_rx_software = s_rx_software_count;
            last_summary_us = now_us;
        }

        /* A continuously populated queue gets a 1 ms budget break at 1 kHz. */
        if (tx_queues_have_items() || profile_pending) {
            vTaskDelay(1);
            /* Do not re-enter the 250 ms heartbeat wait while backlog remains. */
            continue;
        }
    }
}

static void link_rx_task(void *argument)
{
    (void)argument;
    dual_parser_t parser;
    dual_parser_init(&parser, on_link_frame, NULL);
    uart_event_t event;
    while (true) {
        if (xQueueReceive(s_uart_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.type == UART_DATA) {
            size_t remaining = event.size;
            while (remaining > 0) {
                uint8_t receive_buffer[LINK_RX_CHUNK_SIZE];
                const size_t requested = remaining < sizeof(receive_buffer) ?
                    remaining : sizeof(receive_buffer);
                const int received = uart_read_bytes(LINK_UART, receive_buffer, requested, 0);
                if (received <= 0) {
                    ++s_crc_or_frame_errors;
                    break;
                }
                s_rx_bytes += (uint32_t)received;
                /* 输入路径活跃：让板载写盘让路（flash 停顿会饿到接收任务）。 */
                dual_onboard_log_note_input_activity();
                if (s_raw_dump_budget > 0U) {
                    /* 只在对端刚被判离线时抓取少量字节，避免刷屏。 */
                    char hex[3 * LINK_RX_CHUNK_SIZE + 1];
                    size_t used = 0;
                    uint32_t remaining = s_raw_dump_budget;
                    for (int index = 0; index < received && remaining > 0U; ++index) {
                        const int written = snprintf(&hex[used], sizeof(hex) - used,
                                                     "%02X ", receive_buffer[index]);
                        if (written <= 0) {
                            break;
                        }
                        used += (size_t)written;
                        --remaining;
                    }
                    s_raw_dump_budget = remaining;
                    if (used > 0U) {
                        hex[used] = '\0';
                        ESP_LOGW(TAG, "UART1原始字节(%d)：%s", received, hex);
                    }
                }
                dual_parser_feed(&parser, receive_buffer, (size_t)received);
                remaining -= (size_t)received;
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            /*
             * 接收缓冲溢出只意味着“这一小段字节丢了”，绝不该把整条链路判成故障：
             * 之前这里调用 queue_fault()，上层随即撤掉接收端 USB——表现就是
             * “一动鼠标就断开”（实测现场：P 板 `UART1接收缓冲溢出，清理输入和
             * 实体状态` 紧跟 `TinyUSB Driver installed`，随后克隆消失）。
             * 现在只清缓冲、重同步解析器并计数，克隆保持在线。
             */
            ++s_rx_overflows;
            ESP_LOGW(TAG, "UART1接收缓冲溢出：丢弃并重同步（累计=%" PRIu32 "），不断开接收端",
                     s_rx_overflows);
            uart_flush_input(LINK_UART);
            xQueueReset(s_uart_event_queue);
            dual_parser_init(&parser, on_link_frame, NULL);
        }

        /* Avoid spinning if a burst left more hardware events queued. */
        if (uxQueueMessagesWaiting(s_uart_event_queue) > 0) {
            vTaskDelay(1);
        }
    }
}

esp_err_t dual_uart1_start(
    uint8_t role,
    const uint8_t node_id[6],
    dual_link_frame_callback_t frame_callback,
    dual_link_fault_callback_t fault_callback)
{
    if (role > DUAL_ROLE_MOUSE_HOST || node_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_role = role;
    s_peer_role = DUAL_ROLE_UNRESOLVED;
    s_usb_state = DUAL_USB_STATE_WAITING;
    memcpy(s_node_id, node_id, sizeof(s_node_id));
    /* 每次启动必须产生新的会话标识。旧实现截取启动时间低 16 位，
     * 相同时序的物理复位会重复 generation，使对端把新会话的低序号
     * Profile 当成旧帧持续拒绝。32 位硬件随机数将碰撞概率降至可忽略。 */
    do {
        s_generation = esp_random();
    } while (s_generation == 0U);
    s_next_sequence = 0;
    s_peer_sequence_initialized = false;
    s_peer_generation_initialized = false;
    s_peer_online = false;
    s_peer_usb_state = DUAL_USB_STATE_WAITING;
    s_role_ack_logged = false;
    s_last_peer_rx_us = 0;
    s_tx_count = 0;
    s_rx_count = 0;
    s_tx_physical_count = 0;
    s_rx_physical_count = 0;
    s_tx_software_count = 0;
    s_rx_software_count = 0;
    s_crc_or_frame_errors = 0;
    s_rx_bytes = 0;
    s_peer_silence_ms = 0;
    s_raw_dump_budget = 0;
    s_rejected_peers = 0;
    s_tx_queue_current = 0;
    s_tx_queue_peak = 0;
    s_tx_queue_overflows = 0;
    s_tx_queue_drops = 0;
    s_tx_write_failures = 0;
    s_vendor_queue_overflows = 0;
    s_vendor_queue_drops = 0;
    s_frame_callback = frame_callback;
    s_fault_callback = fault_callback;
    s_fault_in_progress = false;
    s_profile_active_buffer = 0;
    s_profile_length = 0;
    s_profile_crc32 = 0;
    s_profile_transfer_id = 0;
    s_profile_offset = 0;
    s_profile_phase = LINK_PROFILE_PHASE_IDLE;
    s_profile_pending = false;
    link_flow_reset(&s_request_flow);
    s_profile_request_cleanup_ready = false;
    s_profile_request_waiting_offer = false;
    s_profile_request_offer_seen = false;
    s_profile_request_accepted_us = 0;
    s_request_offer_retries = 0;
    s_next_flow_id = s_generation;
    link_flow_reset(&s_offer_flow);
    link_flow_reset(&s_commit_flow);
    s_profile_commit_receipt_seen = false;
    s_profile_retransmit_attempts = 0;
    link_flow_reset(&s_gone_flow);
    s_gone_completed_valid = false;
    s_gone_completed_accepted = false;
    s_gone_completed_event_id = 0U;
    s_gone_completed_peer_generation = 0U;
    s_mouse_connection_id = 1U;
    s_recovery_started_us = 0;
    s_gone_retries = 0;
    s_gone_failures = 0;
    s_request_retries = 0;
    s_offer_retries = 0;
    s_commit_replays = 0;
    s_budget_exhausted = 0;
    s_device_gone_reason = 0;
    s_profile_last_commit_us = 0;
    s_profile_peer_generation_valid = false;
    s_profile_motion_since_send = 0;
    s_profile_starts = 0;
    s_profile_chunks = 0;
    s_profile_commits = 0;
    s_profile_restarts = 0;
    s_profile_failures = 0;
    s_profile_buffer_mutex = xSemaphoreCreateMutex();
    if (s_profile_buffer_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t config = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t result = uart_param_config(LINK_UART, &config);
    if (result != ESP_OK) {
        return result;
    }
    result = uart_set_pin(LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (result != ESP_OK) {
        return result;
    }
    result = uart_driver_install(LINK_UART, LINK_RX_BUFFER_SIZE, LINK_TX_BUFFER_SIZE,
                                 LINK_EVENT_QUEUE_LENGTH, &s_uart_event_queue, 0);
    if (result != ESP_OK) {
        return result;
    }
    s_tx_queue = xQueueCreate(LINK_TX_QUEUE_LENGTH, sizeof(tx_item_t));
    s_safety_tx_queue = xQueueCreate(LINK_SAFETY_QUEUE_LENGTH, sizeof(tx_item_t));
    s_software_tx_queue = xQueueCreate(LINK_SOFTWARE_QUEUE_LENGTH, sizeof(tx_item_t));
    s_vendor_tx_queue = xQueueCreate(LINK_VENDOR_QUEUE_LENGTH, sizeof(tx_item_t));
    if (s_tx_queue == NULL || s_safety_tx_queue == NULL ||
        s_software_tx_queue == NULL || s_vendor_tx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(link_rx_task, "dual_uart1_rx", 4096, NULL, LINK_TASK_PRIORITY, &s_rx_task) != pdPASS ||
        xTaskCreate(link_tx_task, "dual_uart1_tx", 4096, NULL, LINK_TASK_PRIORITY, &s_tx_task) != pdPASS) {
        if (s_rx_task != NULL) {
            vTaskDelete(s_rx_task);
            s_rx_task = NULL;
        }
        if (s_tx_task != NULL) {
            vTaskDelete(s_tx_task);
            s_tx_task = NULL;
        }
        vQueueDelete(s_tx_queue);
        vQueueDelete(s_safety_tx_queue);
        vQueueDelete(s_software_tx_queue);
        vQueueDelete(s_vendor_tx_queue);
        s_tx_queue = NULL;
        s_safety_tx_queue = NULL;
        s_software_tx_queue = NULL;
        s_vendor_tx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t dual_uart1_lock_role(uint8_t role)
{
    if (role != DUAL_ROLE_PC_DEVICE && role != DUAL_ROLE_MOUSE_HOST) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_role == role) {
        return ESP_OK;
    }
    if (s_role != DUAL_ROLE_UNRESOLVED) {
        return ESP_ERR_INVALID_STATE;
    }
    s_role = role;
    dual_status_led_set_peer_connected(s_peer_online);
    notify_tx_task();
    return ESP_OK;
}

void dual_uart1_set_usb_state(uint8_t usb_state)
{
    if (s_usb_state == usb_state) {
        return;
    }
    s_usb_state = usb_state;
    notify_tx_task();
}

bool dual_uart1_peer_online(void)
{
    return s_peer_online;
}

uint32_t dual_uart1_generation(void)
{
    return s_generation;
}

uint32_t dual_uart1_peer_generation(void)
{
    return s_peer_generation_initialized ? s_peer_generation : 0U;
}

void dual_uart1_set_profile_request_ready(bool ready)
{
    if (s_role != DUAL_ROLE_PC_DEVICE) {
        return;
    }
    s_profile_request_cleanup_ready = ready;
    taskENTER_CRITICAL(&s_profile_mux);
    if (!ready) {
        /* 已经发出的申请收不回来，但关门后不再重放，避免清理期间逃逸。 */
        if (link_flow_incomplete(&s_request_flow)) {
            s_request_flow.state = LINK_FLOW_FAILED;
            s_request_flow.resend_requested = false;
        }
        s_profile_request_waiting_offer = false;
    } else {
        /* 清理成功：开放新一代申请。 */
        link_flow_reset(&s_request_flow);
        s_profile_request_waiting_offer = false;
        s_profile_request_offer_seen = false;
        s_request_offer_retries = 0;
    }
    taskEXIT_CRITICAL(&s_profile_mux);
    if (ready) {
        notify_tx_task();
    }
}

esp_err_t dual_uart1_send_mouse(
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan)
{
    uint8_t payload[9] = {
        interface_number,
        report_id,
        (uint8_t)(buttons & 0x1FU),
        (uint8_t)x,
        (uint8_t)((uint16_t)x >> 8),
        (uint8_t)y,
        (uint8_t)((uint16_t)y >> 8),
        (uint8_t)wheel,
        (uint8_t)pan,
    };
    return enqueue_item(DUAL_MESSAGE_PHYSICAL_MOUSE, payload, sizeof(payload));
}

esp_err_t dual_uart1_send_release(uint8_t reason)
{
    return enqueue_item(DUAL_MESSAGE_PHYSICAL_RELEASE, &reason, 1);
}

esp_err_t dual_uart1_send_software_mouse(const uint8_t *payload, size_t length)
{
    if (payload == NULL || (length != 7U && length != 8U) ||
        (length == 8U && payload[7] != 0U && payload[7] != 5U)) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_item(DUAL_MESSAGE_SOFTWARE_MOUSE, payload, (uint8_t)length);
}

esp_err_t dual_uart1_send_software_release(void)
{
    return enqueue_item(DUAL_MESSAGE_SOFTWARE_RELEASE, NULL, 0);
}

esp_err_t dual_uart1_send_device_gone(uint8_t reason)
{
    if (s_role != DUAL_ROLE_MOUSE_HOST) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_profile_mux);
    if (link_gone_keeps_existing_event(device_gone_barrier_pending(),
                                       device_gone_barrier_failed(),
                                       s_gone_flow.id)) {
        /* 同一事件仍在途：沿用同一 event ID 与目标 generation，只请求重发。 */
        const uint32_t event_id = s_gone_flow.id;
        link_flow_request_resend(&s_gone_flow);
        taskEXIT_CRITICAL(&s_profile_mux);
        ++s_gone_retries;
        notify_tx_task();
        ESP_LOGW(TAG, "鼠标拔出屏障已在途，保持同一事件：event=%" PRIu32,
                 event_id);
        return ESP_OK;
    }
    const uint32_t event_id = next_flow_id();
    dual_uart1_cancel_vendor_hid_session();
    /* 新的物理拔出事件重新开始本轮恢复计时。 */
    recovery_note_trigger(now_us);
    s_gone_completed_valid = false;
    s_gone_completed_accepted = false;
    s_gone_completed_event_id = 0U;
    s_gone_completed_peer_generation = 0U;
    link_flow_start(&s_gone_flow, DUAL_MESSAGE_DEVICE_GONE, event_id, now_us,
                    LINK_GONE_MAX_ATTEMPTS, LINK_GONE_RETRY_INTERVAL_US,
                    recovery_stage_timeout(now_us, LINK_GONE_STAGE_TIMEOUT_US));
    s_device_gone_reason = reason;
    /* Drop old bytes and in-flight retransmission state. A new Profile queued
     * after this point remains cached, but the TX task cannot OFFER it until
     * this event has been acknowledged by the same P generation. */
    link_flow_reset(&s_offer_flow);
    link_flow_reset(&s_commit_flow);
    s_profile_commit_receipt_seen = false;
    s_profile_retransmit_attempts = 0;
    s_profile_pending = false;
    s_profile_length = 0;
    s_profile_offset = 0;
    s_profile_phase = LINK_PROFILE_PHASE_IDLE;
    s_profile_last_commit_us = 0;
    s_profile_peer_generation_valid = false;
    taskEXIT_CRITICAL(&s_profile_mux);
    notify_tx_task();
    ESP_LOGW(TAG, "建立鼠标拔出清理屏障：event=%" PRIu32 " reason=%u conn=%" PRIu32,
             event_id, reason, s_mouse_connection_id);
    log_recovery("gone_start", NULL);
    return ESP_OK;
}

esp_err_t dual_uart1_send_profile_ack_for_generation(
    uint32_t transfer_id,
    uint32_t crc32,
    uint8_t status,
    uint32_t expected_peer_generation)
{
    if (expected_peer_generation == 0U || !s_peer_generation_initialized ||
        expected_peer_generation != s_peer_generation ||
        transfer_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 同时带上双方 generation：M 只会接受绑定自己当前会话与本端会话的确认。 */
    uint8_t payload[DUAL_LINK_PROFILE_ACK_LENGTH] = {0};
    write_u32_le(&payload[DUAL_LINK_PROFILE_ACK_TRANSFER_ID_OFFSET], transfer_id);
    write_u32_le(&payload[DUAL_LINK_PROFILE_ACK_CRC32_OFFSET], crc32);
    payload[DUAL_LINK_PROFILE_ACK_STATUS_OFFSET] = status;
    write_u32_le(&payload[DUAL_LINK_PROFILE_ACK_RECIPIENT_GENERATION_OFFSET],
                 expected_peer_generation);
    write_u32_le(&payload[DUAL_LINK_PROFILE_ACK_SENDER_GENERATION_OFFSET],
                 s_generation);
    const esp_err_t result = enqueue_item(
        DUAL_MESSAGE_PROFILE_ACK, payload, sizeof(payload));
    ESP_LOGI(TAG, "Profile确认排队：transfer=%" PRIu32 " crc=%08" PRIX32
             " status=%u target_generation=%" PRIu32 " sender_generation=%" PRIu32
             " result=%s",
             transfer_id, crc32, status, expected_peer_generation, s_generation,
             esp_err_to_name(result));
    return result;
}

esp_err_t dual_uart1_send_profile_ack(uint32_t transfer_id, uint32_t crc32, uint8_t status)
{
    return dual_uart1_send_profile_ack_for_generation(
        transfer_id, crc32, status, s_peer_generation);
}

esp_err_t dual_uart1_send_flow_ack(
    uint8_t acknowledged_type, uint32_t flow_id, uint8_t status)
{
    return dual_uart1_send_flow_ack_for_generation(
        acknowledged_type, flow_id, status, s_peer_generation);
}

esp_err_t dual_uart1_send_flow_ack_for_generation(
    uint8_t acknowledged_type,
    uint32_t flow_id,
    uint8_t status,
    uint32_t expected_peer_generation)
{
    if (acknowledged_type != DUAL_MESSAGE_PROFILE_REQUEST &&
        acknowledged_type != DUAL_MESSAGE_PROFILE_OFFER &&
        acknowledged_type != DUAL_MESSAGE_PROFILE_COMMIT &&
        acknowledged_type != DUAL_MESSAGE_DEVICE_GONE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (expected_peer_generation == 0U || !s_peer_generation_initialized ||
        expected_peer_generation != s_peer_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t payload[DUAL_LINK_FLOW_ACK_LENGTH] = {0};
    payload[DUAL_LINK_FLOW_ACK_TYPE_OFFSET] = acknowledged_type;
    write_u32_le(&payload[DUAL_LINK_FLOW_ACK_FLOW_ID_OFFSET], flow_id);
    payload[DUAL_LINK_FLOW_ACK_STATUS_OFFSET] = status;
    write_u32_le(
        &payload[DUAL_LINK_FLOW_ACK_RECIPIENT_GENERATION_OFFSET],
        expected_peer_generation);
    write_u32_le(
        &payload[DUAL_LINK_FLOW_ACK_SENDER_GENERATION_OFFSET], s_generation);
    const esp_err_t result = enqueue_item(DUAL_MESSAGE_FLOW_ACK, payload, sizeof(payload));
    ESP_LOGI(TAG, "确认帧排队：type=%02X flow=%" PRIu32
             " status=%u target_generation=%" PRIu32 " sender_generation=%" PRIu32
             " result=%s",
             acknowledged_type, flow_id, status, s_peer_generation, s_generation,
              esp_err_to_name(result));
    return result;
}

esp_err_t dual_uart1_send_raw_hid_input(
    uint8_t interface_number,
    uint8_t report_id,
    const uint8_t *data,
    size_t data_length)
{
    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD] = {0};
    uint8_t payload_length = 0;
    if (!dual_hid_raw_input_encode(interface_number, report_id, data, data_length,
                                   payload, sizeof(payload), &payload_length)) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_item(DUAL_MESSAGE_RAW_HID_INPUT, payload, payload_length);
}

esp_err_t dual_uart1_send_hid_set_report(
    uint16_t transaction_id,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    const uint8_t *data,
    size_t data_length)
{
    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD] = {0};
    uint8_t payload_length = 0;
    if (!dual_hid_set_report_encode(transaction_id, interface_number, report_id,
                                     report_type, data, data_length, payload,
                                     sizeof(payload), &payload_length)) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_item(DUAL_MESSAGE_HID_SET_REPORT, payload, payload_length);
}

esp_err_t dual_uart1_send_hid_get_request(
    uint16_t transaction_id,
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t report_type,
    uint8_t requested_length)
{
    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD] = {0};
    uint8_t payload_length = 0;
    if (!dual_hid_get_request_encode(transaction_id, interface_number, report_id,
                                      report_type, requested_length, payload,
                                      sizeof(payload), &payload_length)) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_item(DUAL_MESSAGE_HID_GET_REPORT_REQUEST, payload, payload_length);
}

esp_err_t dual_uart1_send_hid_get_response(
    uint16_t transaction_id,
    uint8_t status,
    uint8_t interface_number,
    uint8_t report_id,
    const uint8_t *data,
    size_t data_length)
{
    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD] = {0};
    uint8_t payload_length = 0;
    if (!dual_hid_get_response_encode(transaction_id, status, interface_number,
                                      report_id, data, data_length, payload,
                                      sizeof(payload), &payload_length)) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_item(DUAL_MESSAGE_HID_GET_REPORT_RESPONSE, payload, payload_length);
}

esp_err_t dual_uart1_queue_profile(
    const uint8_t *blob,
    size_t length,
    uint32_t crc32)
{
    if (blob == NULL || length == 0 || length > HID_PROFILE_MAX_BLOB) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_profile_buffer_mutex, portMAX_DELAY);
    uint8_t inactive_buffer;
    taskENTER_CRITICAL(&s_profile_mux);
    inactive_buffer = (uint8_t)(s_profile_active_buffer ^ 1U);
    taskEXIT_CRITICAL(&s_profile_mux);
    memcpy(s_profile_buffers[inactive_buffer], blob, length);
    taskENTER_CRITICAL(&s_profile_mux);
    s_profile_active_buffer = inactive_buffer;
    s_profile_length = (uint32_t)length;
    s_profile_crc32 = crc32;
    ++s_profile_transfer_id;
    if (s_profile_transfer_id == 0) {
        s_profile_transfer_id = 1;
    }
    s_profile_offset = 0;
    s_profile_phase = LINK_PROFILE_PHASE_BEGIN;
    s_profile_peer_generation_valid = false;
    s_profile_last_commit_us = 0;
    s_profile_motion_since_send = 0;
    s_profile_pending = false;
    link_flow_reset(&s_commit_flow);
    s_profile_commit_receipt_seen = false;
    s_profile_retransmit_attempts = 0;
    /* 每采集到一份新 Profile 即进入新的物理鼠标连接周期。 */
    ++s_mouse_connection_id;
    if (s_mouse_connection_id == 0U) {
        s_mouse_connection_id = 1U;
    }
    const int64_t queue_us = esp_timer_get_time();
    link_flow_start(&s_offer_flow, DUAL_MESSAGE_PROFILE_OFFER, next_flow_id(),
                    queue_us, LINK_FLOW_MAX_ATTEMPTS, LINK_FLOW_RETRY_INTERVAL_US,
                    recovery_stage_timeout(queue_us, LINK_FLOW_ACK_TIMEOUT_US));
    taskEXIT_CRITICAL(&s_profile_mux);
    xSemaphoreGive(s_profile_buffer_mutex);
    notify_tx_task();
    ESP_LOGI(TAG, "Profile已缓存并排队克隆提议：conn=%" PRIu32 " transfer=%" PRIu32
             " len=%u", s_mouse_connection_id, s_profile_transfer_id,
             (unsigned)length);
    log_recovery("profile_queued", NULL);
    return ESP_OK;
}

void dual_uart1_cancel_profile(void)
{
    /* 拔出后不能在下一轮心跳或对端复位时重播旧鼠标描述符。 */
    taskENTER_CRITICAL(&s_profile_mux);
    s_profile_pending = false;
    link_flow_reset(&s_offer_flow);
    link_flow_reset(&s_commit_flow);
    s_profile_commit_receipt_seen = false;
    s_profile_retransmit_attempts = 0;
    s_profile_length = 0;
    s_profile_offset = 0;
    s_profile_phase = LINK_PROFILE_PHASE_IDLE;
    s_profile_last_commit_us = 0;
    s_profile_peer_generation_valid = false;
    taskEXIT_CRITICAL(&s_profile_mux);
}

void dual_uart1_deferred_refresh(void)
{
    /* P 的请求要求重新采集：旧提议与旧传输一律作废，由新采集重新排队。 */
    taskENTER_CRITICAL(&s_profile_mux);
    s_profile_pending = false;
    link_flow_reset(&s_offer_flow);
    link_flow_reset(&s_commit_flow);
    s_profile_commit_receipt_seen = false;
    s_profile_retransmit_attempts = 0;
    s_profile_phase = LINK_PROFILE_PHASE_IDLE;
    taskEXIT_CRITICAL(&s_profile_mux);
}

/*
 * 克隆是否已就绪（鼠标侧板判定）：只有收到绑定当前传输的成功 Profile ACK 之后
 * 才算完成。用户要求：克隆完成前完全不转发鼠标移动，避免干扰克隆建立流程。
 */
bool dual_uart1_clone_ready(void)
{
    return s_usb_state == DUAL_USB_STATE_HID_CONNECTED;
}

bool dual_uart1_profile_transfer_in_flight(void)
{
    bool in_flight = false;
    taskENTER_CRITICAL(&s_profile_mux);
    in_flight = s_profile_pending ||
        s_profile_phase != LINK_PROFILE_PHASE_IDLE ||
        link_flow_incomplete(&s_offer_flow) ||
        link_flow_incomplete(&s_commit_flow);
    taskEXIT_CRITICAL(&s_profile_mux);
    return in_flight;
}
