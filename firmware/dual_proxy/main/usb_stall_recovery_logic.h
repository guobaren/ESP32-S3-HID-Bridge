#pragma once

#include <stdbool.h>
#include <stdint.h>

#define USB_STALL_POLL_MS 20U
#define USB_STALL_HEARTBEAT_US 250000LL
#define USB_STALL_TARGET_US 500000LL
#define USB_STALL_WARMUP_US 10000000LL
#define USB_STALL_COOLDOWN_US 30000000LL

/* 只对已建立会话且标准心跳仍未完成的设备判定，不要求先产生物理输入报告。 */
static inline bool usb_stall_evidence_ready(int64_t now, int64_t ready,
                                          int64_t oldest_heartbeat,
                                          bool active, bool stopping)
{
    return active && !stopping && ready > 0 && oldest_heartbeat > 0 &&
           now - ready >= USB_STALL_WARMUP_US &&
           now - oldest_heartbeat >= USB_STALL_HEARTBEAT_US;
}

static inline bool usb_stall_cooldown_ready(int64_t now, int64_t last_cycle)
{
    return last_cycle == 0 || now - last_cycle >= USB_STALL_COOLDOWN_US;
}

/* 观测起点为心跳 URB 提交时刻；不是物理总线故障时刻。 */
static inline int64_t usb_stall_observed_delay(int64_t now, int64_t oldest_heartbeat)
{
    return now - oldest_heartbeat;
}
