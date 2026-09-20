#pragma once

#include <stdbool.h>

/* These values are deliberately shared with the native regression test. */
#define DUAL_PROXY_REQUIRED_FREERTOS_HZ 1000U
#define DUAL_PROXY_LINK_TX_BATCH_LIMIT 8U
#define DUAL_PROXY_HID_PERIOD_US 1000U

/* A TX notification is a wake-up hint; queued data is the source of truth. */
static inline bool dual_proxy_link_should_wait_for_notification(bool has_pending_items)
{
    return !has_pending_items;
}
