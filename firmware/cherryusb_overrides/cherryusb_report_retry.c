#include "cherryusb_report_retry.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_errno.h"

static const uint32_t s_retry_delays_ms[CHERRYUSB_REPORT_RETRY_MAX_ATTEMPTS - 1U] = {
    2U, 5U, 10U,
};

const char *cherryusb_report_retry_reason_name(cherryusb_report_retry_reason_t reason)
{
    switch (reason) {
    case CHERRYUSB_REPORT_RETRY_SUCCESS: return "success";
    case CHERRYUSB_REPORT_RETRY_STOPPED: return "stopped";
    case CHERRYUSB_REPORT_RETRY_DEADLINE: return "deadline";
    case CHERRYUSB_REPORT_RETRY_SHORT_PAYLOAD: return "short_payload";
    case CHERRYUSB_REPORT_RETRY_CHANNEL_BUSY: return "channel_busy";
    case CHERRYUSB_REPORT_RETRY_NON_NAK: return "non_nak";
    case CHERRYUSB_REPORT_RETRY_EXHAUSTED: return "attempts_exhausted";
    case CHERRYUSB_REPORT_RETRY_INVALID: return "invalid";
    default: return "unknown";
    }
}

cherryusb_report_retry_reason_t cherryusb_report_descriptor_read_retry(
    void *context,
    size_t requested_payload,
    cherryusb_report_attempt_fn attempt_fn,
    cherryusb_report_stopping_fn stopping_fn,
    cherryusb_report_retry_result_t *result)
{
    if (result == NULL) {
        return CHERRYUSB_REPORT_RETRY_INVALID;
    }
    *result = (cherryusb_report_retry_result_t){
        .channel_idle = true,
        .reason = CHERRYUSB_REPORT_RETRY_INVALID,
    };
    if (requested_payload == 0U || attempt_fn == NULL || stopping_fn == NULL) {
        return result->reason;
    }

    const int64_t started_us = esp_timer_get_time();
    const int64_t deadline_us = started_us +
        ((int64_t)CHERRYUSB_REPORT_RETRY_BUDGET_MS * 1000LL);
    for (uint32_t attempt = 1U; attempt <= CHERRYUSB_REPORT_RETRY_MAX_ATTEMPTS; ++attempt) {
        int64_t now_us = esp_timer_get_time();
        if (stopping_fn(context)) {
            result->reason = CHERRYUSB_REPORT_RETRY_STOPPED;
            break;
        }
        if (now_us >= deadline_us) {
            result->reason = CHERRYUSB_REPORT_RETRY_DEADLINE;
            break;
        }

        int raw_result = 0;
        bool raw_result_valid = false;
        bool channel_idle = false;
        size_t actual_payload = 0U;
        const int status = attempt_fn(context, attempt, deadline_us,
                                      &raw_result, &raw_result_valid,
                                      &actual_payload, &channel_idle);
        now_us = esp_timer_get_time();
        result->attempts = attempt;
        result->status = status;
        result->raw_result = raw_result;
        result->raw_result_valid = raw_result_valid;
        result->channel_idle = channel_idle;
        result->actual_payload = actual_payload;

        if (now_us > deadline_us) {
            result->reason = CHERRYUSB_REPORT_RETRY_DEADLINE;
            break;
        }
        if (stopping_fn(context)) {
            result->reason = CHERRYUSB_REPORT_RETRY_STOPPED;
            break;
        }
        if (status == 0) {
            result->reason = actual_payload == requested_payload
                ? CHERRYUSB_REPORT_RETRY_SUCCESS
                : CHERRYUSB_REPORT_RETRY_SHORT_PAYLOAD;
            break;
        }
        if (!raw_result_valid || raw_result != -USB_ERR_NAK) {
            result->reason = CHERRYUSB_REPORT_RETRY_NON_NAK;
            break;
        }
        if (!channel_idle) {
            result->reason = CHERRYUSB_REPORT_RETRY_CHANNEL_BUSY;
            break;
        }
        if (attempt == CHERRYUSB_REPORT_RETRY_MAX_ATTEMPTS) {
            result->reason = CHERRYUSB_REPORT_RETRY_EXHAUSTED;
            break;
        }
        if (stopping_fn(context)) {
            result->reason = CHERRYUSB_REPORT_RETRY_STOPPED;
            break;
        }

        const uint32_t delay_ms = s_retry_delays_ms[attempt - 1U];
        const int64_t delay_us = (int64_t)delay_ms * 1000LL;
        now_us = esp_timer_get_time();
        if (now_us >= deadline_us || deadline_us - now_us <= delay_us) {
            result->reason = CHERRYUSB_REPORT_RETRY_DEADLINE;
            break;
        }
        TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
        if (delay_ticks == 0U) {
            delay_ticks = 1U;
        }
        vTaskDelay(delay_ticks);
    }

    result->elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);
    return result->reason;
}
