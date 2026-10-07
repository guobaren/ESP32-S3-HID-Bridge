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
/* 旧 V3 ASCII 鼠标子集仅在显式启用时构建。 */
#ifndef DUAL_PROXY_ENABLE_MAKCU_ASCII_API
#define DUAL_PROXY_ENABLE_MAKCU_ASCII_API 0
#endif
/* Makcu V4 鼠标 ASCII + MAK_API，当前双板构建默认启用。 */
#ifndef DUAL_PROXY_ENABLE_MAKCU_V4_API
#define DUAL_PROXY_ENABLE_MAKCU_V4_API 1
#endif
/* 统计只通过UART0主动快照查询，不提供运行时周期输出开关。 */
#define DUAL_PROXY_ENABLE_PERIODIC_STATS_LOG 0

/* A TX notification is a wake-up hint; queued data is the source of truth. */
static inline bool dual_proxy_link_should_wait_for_notification(bool has_pending_items)
{
    return !has_pending_items;
}
