#pragma once

#include "bridge_protocol.h"

typedef struct {
    int64_t absent_since_us;
    bool usb_seen;
} dual_role_watch_t;

/* UART 保持供电时，拔出数据 USB 可能只表现为 suspend；不能只信旧 mounted。 */
static inline bool dual_role_device_usb_present(bool installed, bool connected, bool suspended)
{
    return installed && connected && !suspended;
}

/* 初次克隆保留 15 秒预算；稳定连接掉线后使用 3 秒去抖。
 * 主动重枚举期间暂停计时，等待电脑挂载则允许重新探测。 */
static inline bool dual_role_watch_poll(dual_role_watch_t *state, uint8_t role,
                                        bool connected, bool can_probe, int64_t now_us)
{
    if (connected) {
        state->usb_seen = true;
        state->absent_since_us = 0;
        return false;
    }
    if (!can_probe) {
        state->absent_since_us = 0;
        return false;
    }
    if (state->absent_since_us == 0) {
        state->absent_since_us = now_us;
        return false;
    }
    const int64_t grace = role == DUAL_ROLE_PC_DEVICE && !state->usb_seen
        ? 15000000LL : 3000000LL;
    return now_us - state->absent_since_us >= grace;
}

static inline uint8_t dual_role_fresh_peer_claim(uint8_t claim, uint32_t age_ms, bool online)
{
    return online && age_ms <= 750U ? claim : DUAL_ROLE_UNRESOLVED;
}

static inline bool dual_role_hello_valid(const uint8_t *payload, size_t length)
{
    return payload != NULL && length == DUAL_LINK_HELLO_LENGTH &&
        payload[0] <= DUAL_ROLE_MOUSE_HOST && payload[1] <= 4U &&
        (payload[DUAL_LINK_HELLO_USB_ROLE_OFFSET] == DUAL_ROLE_UNRESOLVED ||
         payload[DUAL_LINK_HELLO_USB_ROLE_OFFSET] == payload[0]);
}
