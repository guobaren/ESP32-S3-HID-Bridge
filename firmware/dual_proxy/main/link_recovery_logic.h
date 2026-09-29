#ifndef LINK_RECOVERY_LOGIC_H
#define LINK_RECOVERY_LOGIC_H

/*
 * 连接恢复的纯逻辑状态模型。
 *
 * 这一层只描述“该不该重发、这一帧算新事务还是重复、这个失败还能不能重试、
 * 剩余预算够不够”，不碰 FreeRTOS、UART 或 TinyUSB。UART1 任务与电脑侧
 * 重配置任务都按这里的判定执行，逻辑测试也直接喂这组函数做故障注入。
 */

#include <stdbool.h>
#include <stdint.h>

#include "bridge_protocol.h"

/* 新 Profile 的恢复预算须覆盖 P 侧两次最长 3 秒的 USB 挂载尝试。 */
#define LINK_RECOVERY_BUDGET_US 14000000LL
/* 单个事务阶段的期望等待窗口；实际等待会被剩余总预算裁剪。 */
#define LINK_PROFILE_STAGE_TIMEOUT_US 5000000LL

/* 单次事务的有界重试参数（初值待实机标定；现场实测对端在 4 秒级才回应，
 * 因此通用事务按 1.2 秒间隔×5 次覆盖整个 5 秒阶段窗口）。 */
#define LINK_FLOW_MAX_ATTEMPTS 5U
#define LINK_FLOW_RETRY_INTERVAL_US 1200000LL
#define LINK_GONE_MAX_ATTEMPTS 8U
#define LINK_GONE_RETRY_INTERVAL_US 700000LL
#define LINK_GONE_STAGE_TIMEOUT_US 5000000LL
/* COMMIT 发出后等待“数据接收确认 + 最终挂载结果”的事务参数。 */
#define LINK_COMMIT_MAX_ATTEMPTS 10U
#define LINK_COMMIT_RETRY_INTERVAL_US 1000000LL
#define LINK_COMMIT_STAGE_TIMEOUT_US 8000000LL
#define LINK_PROFILE_MOUNT_MAX_ATTEMPTS 2U

static inline bool link_profile_mount_should_retry(
    bool timed_out, uint8_t attempt, bool epoch_current)
{
    return timed_out && epoch_current && attempt > 0U &&
        attempt < LINK_PROFILE_MOUNT_MAX_ATTEMPTS;
}
/* 电脑侧本地清理失败后的重试次数（同一事务、同一 generation）。 */
#define LINK_CLEANUP_MAX_ATTEMPTS 3U
/* 清理已成功、只有确认帧入队失败时的补发次数。 */
#define LINK_ACK_ENQUEUE_MAX_ATTEMPTS 3U

typedef enum {
    LINK_PROFILE_REPLAY_PENDING = 0,
    LINK_PROFILE_REPLAY_MOUNTED,
    LINK_PROFILE_REPLAY_FAILED,
} link_profile_replay_result_t;

/* COMMIT 的数据发布不是 USB 挂载；重复帧只可回放真实的最终结果。 */
static inline link_profile_replay_result_t link_profile_replay_result(
    uint32_t transfer_id, uint32_t crc32,
    uint32_t installed_transfer_id, uint32_t installed_crc32,
    uint32_t failed_transfer_id, uint32_t failed_crc32,
    bool clone_active, bool installed, bool mounted, bool reconfiguring)
{
    if (transfer_id == 0U) {
        return LINK_PROFILE_REPLAY_PENDING;
    }
    if (installed_transfer_id == transfer_id && installed_crc32 == crc32 &&
        clone_active && installed && mounted && !reconfiguring) {
        return LINK_PROFILE_REPLAY_MOUNTED;
    }
    if (failed_transfer_id == transfer_id && failed_crc32 == crc32) {
        return LINK_PROFILE_REPLAY_FAILED;
    }
    return LINK_PROFILE_REPLAY_PENDING;
}

typedef enum {
    LINK_FLOW_IDLE = 0,
    /* 已创建但受门控（清理屏障/对端离线）暂未发送。 */
    LINK_FLOW_QUEUED,
    /* 已发送，等待对端确认。 */
    LINK_FLOW_PENDING,
    LINK_FLOW_ACCEPTED,
    LINK_FLOW_FAILED,
} link_flow_state_t;

typedef enum {
    LINK_FLOW_ACTION_NONE = 0,
    LINK_FLOW_ACTION_SEND,
    LINK_FLOW_ACTION_RESEND,
    LINK_FLOW_ACTION_GIVE_UP,
} link_flow_action_t;

typedef struct {
    uint8_t type;             /* 期望的 FLOW_ACK acknowledged_type；0 表示无活动事务 */
    uint32_t id;              /* flow_id / event_id / transfer_id */
    uint32_t peer_generation; /* 该事务绑定的对端 generation */
    link_flow_state_t state;
    uint8_t attempts;         /* 已实际发送次数 */
    uint8_t max_attempts;
    int64_t started_us;
    int64_t last_send_us;
    int64_t retry_interval_us;
    int64_t deadline_us;      /* 0 表示不限时（只用次数预算） */
    /* 首次实际发出后才开始计算的阶段窗口：受门控排队的时间不计入。 */
    int64_t stage_timeout_us;
    bool resend_requested;
} link_flow_t;

static inline void link_flow_reset(link_flow_t *flow)
{
    if (flow == NULL) {
        return;
    }
    flow->type = 0U;
    flow->id = 0U;
    flow->peer_generation = 0U;
    flow->state = LINK_FLOW_IDLE;
    flow->attempts = 0U;
    flow->max_attempts = 0U;
    flow->started_us = 0;
    flow->last_send_us = 0;
    flow->retry_interval_us = 0;
    flow->deadline_us = 0;
    flow->stage_timeout_us = 0;
    flow->resend_requested = false;
}

static inline bool link_flow_pending(const link_flow_t *flow)
{
    return flow != NULL && flow->state == LINK_FLOW_PENDING;
}

static inline bool link_flow_queued(const link_flow_t *flow)
{
    return flow != NULL && flow->state == LINK_FLOW_QUEUED;
}

/* QUEUED 也算未完成：受门控还没有发出去的事务同样占用这个身份。 */
static inline bool link_flow_incomplete(const link_flow_t *flow)
{
    return link_flow_pending(flow) || link_flow_queued(flow);
}

static inline bool link_flow_completed(const link_flow_t *flow)
{
    return flow != NULL && flow->state == LINK_FLOW_ACCEPTED;
}

/*
 * 用剩余总预算裁剪本阶段的等待窗口：各阶段共用同一份预算，
 * 不允许串联多个完整的 5 秒等待。
 */
static inline int64_t link_recovery_stage_timeout_us(
    int64_t recovery_start_us, int64_t now_us, int64_t desired_us)
{
    if (recovery_start_us <= 0) {
        return desired_us;
    }
    const int64_t budget_deadline = recovery_start_us + LINK_RECOVERY_BUDGET_US;
    const int64_t stage_deadline = now_us + desired_us;
    const int64_t deadline = budget_deadline < stage_deadline ?
        budget_deadline : stage_deadline;
    const int64_t remaining = deadline - now_us;
    return remaining > 0 ? remaining : 0;
}

static inline bool link_recovery_budget_exhausted(int64_t recovery_start_us, int64_t now_us)
{
    if (recovery_start_us <= 0) {
        return false;
    }
    return now_us - recovery_start_us >= LINK_RECOVERY_BUDGET_US;
}

static inline void link_flow_start(
    link_flow_t *flow,
    uint8_t type,
    uint32_t id,
    int64_t now_us,
    uint8_t max_attempts,
    int64_t retry_interval_us,
    int64_t stage_timeout_us)
{
    if (flow == NULL) {
        return;
    }
    flow->type = type;
    flow->id = id;
    flow->peer_generation = 0U;
    flow->state = LINK_FLOW_QUEUED;
    flow->attempts = 0U;
    flow->max_attempts = max_attempts;
    flow->started_us = now_us;
    flow->last_send_us = 0;
    flow->retry_interval_us = retry_interval_us;
    flow->deadline_us = 0;
    flow->stage_timeout_us = stage_timeout_us;
    flow->resend_requested = true;
}

static inline void link_flow_mark_sent(link_flow_t *flow, int64_t now_us)
{
    if (flow == NULL) {
        return;
    }
    if (flow->attempts == 0U && flow->stage_timeout_us > 0) {
        /* 第一次真正发出时才开始计时：等待门控打开不算超时。 */
        flow->deadline_us = now_us + flow->stage_timeout_us;
    }
    if (flow->attempts < UINT8_MAX) {
        ++flow->attempts;
    }
    flow->last_send_us = now_us;
    flow->resend_requested = false;
    if (flow->state == LINK_FLOW_QUEUED || flow->state == LINK_FLOW_PENDING) {
        flow->state = LINK_FLOW_PENDING;
    }
}

/* 只匹配同一事务：type、ID 与对端 generation 三者都必须一致。 */
static inline bool link_flow_matches(
    const link_flow_t *flow, uint8_t type, uint32_t id, uint32_t peer_generation)
{
    if (flow == NULL || flow->type != type || flow->id != id || id == 0U) {
        return false;
    }
    return flow->peer_generation == 0U || flow->peer_generation == peer_generation;
}

static inline link_flow_action_t link_flow_poll(link_flow_t *flow, int64_t now_us)
{
    if (flow == NULL) {
        return LINK_FLOW_ACTION_NONE;
    }
    if (flow->state == LINK_FLOW_QUEUED) {
        return LINK_FLOW_ACTION_SEND;
    }
    if (flow->state != LINK_FLOW_PENDING) {
        return LINK_FLOW_ACTION_NONE;
    }
    if (flow->resend_requested) {
        return LINK_FLOW_ACTION_RESEND;
    }
    if (flow->deadline_us != 0 && now_us >= flow->deadline_us) {
        return LINK_FLOW_ACTION_GIVE_UP;
    }
    if (flow->last_send_us != 0 && flow->attempts >= flow->max_attempts) {
        return LINK_FLOW_ACTION_GIVE_UP;
    }
    if (flow->last_send_us != 0 && flow->retry_interval_us > 0 &&
        now_us - flow->last_send_us >= flow->retry_interval_us) {
        return LINK_FLOW_ACTION_RESEND;
    }
    return LINK_FLOW_ACTION_NONE;
}

/*
 * 只读版本：任务在决定是否进入心跳等待前用它判断“这个事务现在需不需要动作”，
 * 避免把重发窗口睡过去。语义必须与 link_flow_poll 保持一致。
 */
static inline bool link_flow_action_due(const link_flow_t *flow, int64_t now_us)
{
    if (flow == NULL) {
        return false;
    }
    if (flow->state == LINK_FLOW_QUEUED) {
        return true;
    }
    if (flow->state != LINK_FLOW_PENDING) {
        return false;
    }
    if (flow->resend_requested) {
        return true;
    }
    if (flow->deadline_us != 0 && now_us >= flow->deadline_us) {
        return true;
    }
    if (flow->last_send_us != 0 && flow->attempts >= flow->max_attempts) {
        return true;
    }
    return flow->last_send_us != 0 && flow->retry_interval_us > 0 &&
        now_us - flow->last_send_us >= flow->retry_interval_us;
}

/* 收到匹配确认：返回 true 表示这次确认改变了事务状态（首次确认）。 */
static inline bool link_flow_ack(link_flow_t *flow, bool accepted){
    if (flow == NULL || flow->state != LINK_FLOW_PENDING) {
        return false;
    }
    flow->state = accepted ? LINK_FLOW_ACCEPTED : LINK_FLOW_FAILED;
    flow->resend_requested = false;
    return true;
}

/* 对端报告清理失败：保留同一事务身份，安排有界重试而不是永久作废。 */
static inline void link_flow_request_resend(link_flow_t *flow)
{
    if (flow == NULL) {
        return;
    }
    if (flow->state == LINK_FLOW_QUEUED || flow->state == LINK_FLOW_PENDING) {
        flow->resend_requested = true;
    }
}

typedef enum {
    LINK_DUP_NEW = 0,          /* 全新事务，需要真正执行副作用 */
    LINK_DUP_IN_FLIGHT,        /* 同一事务正在进行，只重发当前结果 */
    LINK_DUP_COMPLETED,        /* 同一事务已完成，返回已有结果，不重复副作用 */
    LINK_DUP_SUPERSEDED,       /* 更旧的 ID，直接忽略 */
} link_dup_class_t;

static inline link_dup_class_t link_transaction_classify(
    bool have_history,
    uint32_t history_generation,
    uint32_t history_id,
    bool history_completed,
    uint32_t generation,
    uint32_t id)
{
    if (!have_history || history_id == 0U || generation != history_generation) {
        /* 对端 generation 已变，旧 ID 不再可比，一律当作新事务。 */
        return LINK_DUP_NEW;
    }
    if (id == history_id) {
        return history_completed ? LINK_DUP_COMPLETED : LINK_DUP_IN_FLIGHT;
    }
    return dual_flow_id_is_newer(id, history_id) ? LINK_DUP_NEW : LINK_DUP_SUPERSEDED;
}

/* 迟到的有效确认仍然可以落到已经完成的同一事件上。 */
static inline bool link_late_ack_matches_completed(
    bool have_completed,
    uint32_t completed_generation,
    uint32_t completed_id,
    bool completed_accepted,
    uint32_t generation,
    uint32_t id)
{
    if (!have_completed || completed_id == 0U) {
        return false;
    }
    return generation == completed_generation && id == completed_id && !completed_accepted;
}

/* 同一非零 event 仍在途时，新的拔出只能沿用原事件，不能换 ID。 */
static inline bool link_gone_keeps_existing_event(
    bool barrier_pending, bool barrier_failed, uint32_t event_id)
{
    return barrier_pending && !barrier_failed && event_id != 0U;
}

typedef enum {
    LINK_CAUSE_NONE = 0,
    LINK_CAUSE_NOT_ENUMERATED,       /* 物理鼠标尚未枚举，等待而不是永久失败 */
    LINK_CAUSE_QUEUE_BUSY,           /* 队列暂时不可用 */
    LINK_CAUSE_USB_TEARDOWN_FAILED,  /* TinyUSB 卸载失败 */
    LINK_CAUSE_USB_INSTALL_FAILED,   /* TinyUSB 安装失败 */
    LINK_CAUSE_ACK_TIMEOUT,          /* 确认超时 */
    LINK_CAUSE_UNSUPPORTED_DESCRIPTOR,
    LINK_CAUSE_INVALID_STATE,
    LINK_CAUSE_UNKNOWN,
} link_failure_cause_t;

static inline bool link_failure_is_retryable(link_failure_cause_t cause)
{
    switch (cause) {
    case LINK_CAUSE_NOT_ENUMERATED:
    case LINK_CAUSE_QUEUE_BUSY:
    case LINK_CAUSE_USB_TEARDOWN_FAILED:
    case LINK_CAUSE_USB_INSTALL_FAILED:
    case LINK_CAUSE_ACK_TIMEOUT:
        return true;
    default:
        return false;
    }
}

static inline const char *link_failure_cause_name(link_failure_cause_t cause)
{
    switch (cause) {
    case LINK_CAUSE_NOT_ENUMERATED:
        return "not_enumerated";
    case LINK_CAUSE_QUEUE_BUSY:
        return "queue_busy";
    case LINK_CAUSE_USB_TEARDOWN_FAILED:
        return "usb_teardown_failed";
    case LINK_CAUSE_USB_INSTALL_FAILED:
        return "usb_install_failed";
    case LINK_CAUSE_ACK_TIMEOUT:
        return "ack_timeout";
    case LINK_CAUSE_UNSUPPORTED_DESCRIPTOR:
        return "unsupported_descriptor";
    case LINK_CAUSE_INVALID_STATE:
        return "invalid_state";
    case LINK_CAUSE_NONE:
        return "none";
    default:
        return "unknown";
    }
}

/* 清理失败后保持克隆门关闭，同时允许同一 generation/事件的有限重试。 */
static inline bool link_cleanup_should_retry(uint8_t attempts, uint8_t max_attempts)
{
    return attempts < max_attempts;
}

/* 清理已经成功、只有确认帧入队失败时，只能补发 ACK，不得重复副作用。 */
static inline bool link_ack_retry_skips_side_effects(bool cleanup_succeeded, bool ack_failed)
{
    return cleanup_succeeded && ack_failed;
}

/* 只有清理实际成功后才允许发成功确认。 */
static inline bool link_cleanup_result_is_accepted(link_failure_cause_t cause)
{
    return cause == LINK_CAUSE_NONE;
}

/* 只有绑定当前活动传输的成功 PROFILE_ACK 才能推进 M 的 HID 状态。 */
static inline bool link_profile_ack_updates_hid_state(bool matched, bool accepted)
{
    return matched && accepted;
}

/*
 * Profile CRC 复用判定（2026-09-28，纯逻辑）。
 *
 * 背景（实测，见 docs/交接.md Next Steps 第 0 条）：板间 Profile 数据段只有 12~30 ms，
 * 而 P 侧"卸载 → 安装 → Windows 重新枚举"要 609~962 ms（典型）、尾部 2.7 s。
 * 因此真正省时间的一步是**跳过卸载+重装**，而不是跳过数据。
 *
 * 只有下列条件全部成立才允许复用当前已挂载的克隆：
 *   - 重配置通道已启用，且克隆处于活动 + 真正挂载状态；
 *   - 没有在途的卸载请求、卸载失败、安装请求或安装任务；
 *   - 本次提议的 CRC32 与"已安装 Profile"的 CRC32 完全一致且非 0。
 * 任何一条不成立都必须退回完整路径（卸载 + 重装），宁可慢也不把陈旧克隆留给接收端。
 * CRC32 为 0 专门表示"当前没有已安装 Profile"或离线注入（transfer=0），一律不复用。
 */
static inline bool link_profile_reuse_allowed(
    bool reconfigure_enabled,
    bool clone_active,
    bool usb_mounted,
    bool operation_in_progress,
    bool profile_pending,
    bool disconnect_requested,
    bool disconnect_failed,
    uint32_t offered_crc32,
    uint32_t installed_crc32)
{
    if (!reconfigure_enabled || !clone_active || !usb_mounted ||
        operation_in_progress || profile_pending ||
        disconnect_requested || disconnect_failed) {
        return false;
    }
    return offered_crc32 != 0U && offered_crc32 == installed_crc32;
}

typedef struct {
    bool release_inputs;         /* 释放按键/按钮 */
    bool record_input_error;     /* 记为输入错误，供诊断 */
    bool mark_usb_disconnected;  /* 改写本端 UART1 usb_state */
    bool cancel_cached_profile;  /* 丢弃缓存的旧 Profile */
    bool start_device_gone;      /* 进入真实物理断开事务 */
} link_release_plan_t;

/*
 * device_gone=false 表示报告/传输异常：只释放按钮并记录输入错误，
 * 不得升级为设备断开、Profile 清理或 USB 重枚举。
 */
static inline link_release_plan_t link_release_plan(bool device_gone)
{
    link_release_plan_t plan = {
        .release_inputs = true,
        .record_input_error = !device_gone,
        .mark_usb_disconnected = device_gone,
        .cancel_cached_profile = device_gone,
        .start_device_gone = device_gone,
    };
    return plan;
}

/* PROFILE_ACK 必须同时匹配本地 generation、对端 generation 和活动传输。 */
static inline bool link_profile_ack_is_for_session(
    uint32_t recipient_generation,
    uint32_t sender_generation,
    uint32_t local_generation,
    bool peer_generation_initialized,
    uint32_t peer_generation,
    uint32_t transfer_id,
    uint32_t crc32,
    uint32_t active_transfer_id,
    uint32_t active_crc32)
{
    return recipient_generation == local_generation &&
        peer_generation_initialized &&
        sender_generation == peer_generation &&
        transfer_id != 0U &&
        transfer_id == active_transfer_id &&
        crc32 == active_crc32;
}

#endif
