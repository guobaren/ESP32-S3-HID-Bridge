#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHERRYUSB_REPORT_RETRY_MAX_ATTEMPTS 4U
#define CHERRYUSB_REPORT_RETRY_BUDGET_MS 500U

typedef enum {
    CHERRYUSB_REPORT_RETRY_SUCCESS = 0,
    CHERRYUSB_REPORT_RETRY_STOPPED,
    CHERRYUSB_REPORT_RETRY_DEADLINE,
    CHERRYUSB_REPORT_RETRY_SHORT_PAYLOAD,
    CHERRYUSB_REPORT_RETRY_CHANNEL_BUSY,
    CHERRYUSB_REPORT_RETRY_NON_NAK,
    CHERRYUSB_REPORT_RETRY_EXHAUSTED,
    CHERRYUSB_REPORT_RETRY_INVALID,
} cherryusb_report_retry_reason_t;

typedef struct {
    int status;
    int raw_result;
    bool raw_result_valid;
    bool channel_idle;
    size_t actual_payload;
    uint32_t attempts;
    uint32_t elapsed_us;
    cherryusb_report_retry_reason_t reason;
} cherryusb_report_retry_result_t;

typedef int (*cherryusb_report_attempt_fn)(
    void *context,
    uint32_t attempt,
    int64_t deadline_us,
    int *raw_result,
    bool *raw_result_valid,
    size_t *actual_payload,
    bool *channel_idle);
typedef bool (*cherryusb_report_stopping_fn)(void *context);

/* 仅供 Report Descriptor GET 使用；每次回调创建新 SETUP，锁与 URB 共用绝对期限。 */
cherryusb_report_retry_reason_t cherryusb_report_descriptor_read_retry(
    void *context,
    size_t requested_payload,
    cherryusb_report_attempt_fn attempt_fn,
    cherryusb_report_stopping_fn stopping_fn,
    cherryusb_report_retry_result_t *result);

const char *cherryusb_report_retry_reason_name(cherryusb_report_retry_reason_t reason);
