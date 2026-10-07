#pragma once

#include <stdbool.h>

/* 心跳专用快照排除尚未成功提交的 URB，以及所有普通 EP0 请求。 */
static inline bool vendor_urb_pending_matches_snapshot(
    bool submitted, bool heartbeat, bool heartbeat_only)
{
    return submitted && (!heartbeat_only || heartbeat);
}
