#pragma once

#include <stdbool.h>

/* These values are deliberately shared with the native regression test. */
#define DUAL_PROXY_REQUIRED_FREERTOS_HZ 1000U
#define DUAL_PROXY_LINK_TX_BATCH_LIMIT 8U
#define DUAL_PROXY_HID_PERIOD_US 1000U

/* 现场 A/B 诊断开关：默认关闭，改为 1 后重新构建即可临时启用。 */
#ifndef DUAL_PROXY_ENABLE_HIDPP_TIMEOUT_DIAGNOSTIC
#define DUAL_PROXY_ENABLE_HIDPP_TIMEOUT_DIAGNOSTIC 1
#endif
#ifndef DUAL_PROXY_ENABLE_LINK_GONE_RETRY_DIAGNOSTIC
#define DUAL_PROXY_ENABLE_LINK_GONE_RETRY_DIAGNOSTIC 1
#endif
#ifndef DUAL_PROXY_ENABLE_PERIODIC_STATS_LOG
#define DUAL_PROXY_ENABLE_PERIODIC_STATS_LOG 0
#endif

bool dual_proxy_periodic_stats_enabled(void);
void dual_proxy_set_periodic_stats_enabled(bool enabled);

/* A TX notification is a wake-up hint; queued data is the source of truth. */
static inline bool dual_proxy_link_should_wait_for_notification(bool has_pending_items)
{
    return !has_pending_items;
}
