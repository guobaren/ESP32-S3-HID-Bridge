#pragma once

#include <stdbool.h>
#include <stdint.h>

#define USB_STALL_POLL_MS 20U
#define USB_STALL_INPUT_US 300000LL
#define USB_STALL_EP0_US 250000LL
#define USB_STALL_TARGET_US 500000LL
#define USB_STALL_WARMUP_US 10000000LL
#define USB_STALL_COOLDOWN_US 30000000LL

/* 只对已建立真实输入基线的会话判定；静止和单纯控制失败均不足以触发。 */
static inline bool usb_stall_evidence_ready(int64_t now, int64_t ready,
                                          int64_t last_input, int64_t oldest_ep0,
                                          bool active, bool stopping)
{
    return active && !stopping && ready > 0 && last_input > 0 && oldest_ep0 > 0 &&
           now - ready >= USB_STALL_WARMUP_US &&
           now - last_input >= USB_STALL_INPUT_US &&
           now - oldest_ep0 >= USB_STALL_EP0_US;
}

static inline bool usb_stall_cooldown_ready(int64_t now, int64_t last_cycle)
{
    return last_cycle == 0 || now - last_cycle >= USB_STALL_COOLDOWN_US;
}

/* 观测起点为两个必要条件中较晚的一项；不是物理总线故障时刻。 */
static inline int64_t usb_stall_observed_delay(int64_t now, int64_t last_input,
                                              int64_t oldest_ep0)
{
    return now - (last_input > oldest_ep0 ? last_input : oldest_ep0);
}
