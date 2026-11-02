#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cherryusb_hid_control.h"
#include "cherryusb_report_retry.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_errno.h"
#include "usbh_core.h"

static int s_failures;
static int64_t s_now_us;
static uint32_t s_delay_count;
static uint32_t s_delays_ms[8];
static bool s_stopping;
static bool s_take_succeeds = true;
static bool s_lock_held;
static uint32_t s_take_count;
static uint32_t s_give_count;
static uint32_t s_submit_count;
static int s_submit_results[8];
static uint32_t s_submit_actual_lengths[8];
static bool s_submit_busy[8];
static uint8_t *s_expected_buffer;
static uint16_t s_expected_length;
static uint8_t s_expected_request_type;
static uint8_t s_expected_request;
static uint16_t s_expected_value;
static uint16_t s_expected_index;
static bool s_check_setup_in_submit;
static bool s_payload_was_zeroed;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++s_failures; \
    } \
} while (0)

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

void vTaskDelay(TickType_t ticks)
{
    if (s_delay_count < sizeof(s_delays_ms) / sizeof(s_delays_ms[0])) {
        s_delays_ms[s_delay_count] = ticks;
    }
    ++s_delay_count;
    s_now_us += (int64_t)ticks * 1000LL;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    (void)semaphore;
    (void)ticks;
    ++s_take_count;
    if (!s_take_succeeds || s_lock_held) {
        return pdFALSE;
    }
    s_lock_held = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    CHECK(s_lock_held);
    s_lock_held = false;
    ++s_give_count;
    return pdTRUE;
}

bool dual_cherryusb_hub_is_stopping(struct usbh_bus *bus)
{
    return bus == NULL || s_stopping;
}

void usbh_control_urb_fill(struct usbh_urb *urb,
                           struct usbh_hubport *hport,
                           struct usb_setup_packet *setup,
                           uint8_t *buffer,
                           uint16_t length,
                           uint32_t timeout_ms,
                           void *complete,
                           void *arg)
{
    (void)complete;
    (void)arg;
    urb->hport = hport;
    urb->setup = setup;
    urb->buffer = buffer;
    urb->length = length;
    urb->timeout_ms = timeout_ms;
    urb->actual_length = 0U;
    urb->hcpriv = NULL;
}

int usbh_submit_urb(struct usbh_urb *urb)
{
    const uint32_t index = s_submit_count++;
    CHECK(s_lock_held);
    if (s_check_setup_in_submit) {
        CHECK(urb->setup->bmRequestType == s_expected_request_type);
        CHECK(urb->setup->bRequest == s_expected_request);
        CHECK(urb->setup->wValue == s_expected_value);
        CHECK(urb->setup->wIndex == s_expected_index);
        CHECK(urb->setup->wLength == s_expected_length);
        if (s_expected_buffer != NULL && s_expected_length != 0U) {
            s_payload_was_zeroed = true;
            for (uint16_t i = 0U; i < s_expected_length; ++i) {
                if (s_expected_buffer[i] != 0U) {
                    s_payload_was_zeroed = false;
                }
            }
            CHECK(s_payload_was_zeroed);
        }
    }
    const int result = index < 8U ? s_submit_results[index] : 0;
    urb->actual_length = index < 8U ? s_submit_actual_lengths[index] : 0U;
    urb->hcpriv = index < 8U && s_submit_busy[index] ? (void *)(uintptr_t)1U : NULL;
    return result;
}

typedef struct {
    int statuses[8];
    int raw_results[8];
    bool raw_valid[8];
    bool channel_idle[8];
    size_t actual_payloads[8];
    uint32_t count;
    bool stop_after_attempt;
    int64_t time_per_attempt_us;
} fake_retry_context_t;

static bool retry_stopping(void *context)
{
    (void)context;
    return s_stopping;
}

static int fake_retry_attempt(void *context,
                              uint32_t attempt,
                              int64_t deadline_us,
                              int *raw_result,
                              bool *raw_result_valid,
                              size_t *actual_payload,
                              bool *channel_idle)
{
    fake_retry_context_t *fake = context;
    (void)deadline_us;
    const uint32_t index = attempt - 1U;
    ++fake->count;
    s_now_us += fake->time_per_attempt_us;
    *raw_result = fake->raw_results[index];
    *raw_result_valid = fake->raw_valid[index];
    *actual_payload = fake->actual_payloads[index];
    *channel_idle = fake->channel_idle[index];
    if (fake->stop_after_attempt) {
        s_stopping = true;
    }
    return fake->statuses[index];
}

typedef struct {
    struct usbh_hubport *hport;
    uint8_t buffer[8];
    uint16_t requested;
    uint32_t calls;
} real_transfer_context_t;

static bool real_transfer_stopping(void *context)
{
    real_transfer_context_t *request = context;
    return s_stopping || request == NULL || request->hport == NULL ||
        !request->hport->connected;
}

static int real_transfer_attempt(void *context,
                                 uint32_t attempt,
                                 int64_t deadline_us,
                                 int *raw_result,
                                 bool *raw_result_valid,
                                 size_t *actual_payload,
                                 bool *channel_idle)
{
    real_transfer_context_t *request = context;
    (void)attempt;
    ++request->calls;
    cherryusb_hid_control_trace_t trace = {0};
    const int status = cherryusb_hid_control_transfer_until(
        request->hport, 0x81U, 0x06U, 0x2200U, 2U, request->requested,
        request->buffer, deadline_us, &trace);
    *raw_result = trace.raw_result;
    *raw_result_valid = trace.raw_result_valid;
    *actual_payload = trace.payload_length;
    *channel_idle = trace.channel_idle;
    return status < 0 ? status : 0;
}

static void reset_fakes(void)
{
    s_now_us = 0;
    s_delay_count = 0U;
    memset(s_delays_ms, 0, sizeof(s_delays_ms));
    s_stopping = false;
    s_take_succeeds = true;
    s_lock_held = false;
    s_take_count = 0U;
    s_give_count = 0U;
    s_submit_count = 0U;
    memset(s_submit_results, 0, sizeof(s_submit_results));
    memset(s_submit_actual_lengths, 0, sizeof(s_submit_actual_lengths));
    memset(s_submit_busy, 0, sizeof(s_submit_busy));
    s_expected_buffer = NULL;
    s_expected_length = 0U;
    s_expected_request_type = 0U;
    s_expected_request = 0U;
    s_expected_value = 0U;
    s_expected_index = 0U;
    s_check_setup_in_submit = false;
    s_payload_was_zeroed = false;
}

static void test_retry_nak_then_success_uses_real_transfer(void)
{
    reset_fakes();
    struct usbh_bus bus = { .busid = 0U };
    struct usb_setup_packet setup;
    memset(&setup, 0xA5, sizeof(setup));
    struct usbh_hubport hport = {
        .bus = &bus, .setup = &setup, .mutex = (void *)(uintptr_t)1U,
        .connected = 1U,
    };
    real_transfer_context_t request = {
        .hport = &hport, .requested = 4U,
        .buffer = { 0xCCU, 0xCCU, 0xCCU, 0xCCU },
    };
    s_submit_results[0] = -USB_ERR_NAK;
    s_submit_actual_lengths[0] = 0U;
    s_submit_results[1] = 0;
    s_submit_actual_lengths[1] = 12U;
    s_check_setup_in_submit = true;
    s_expected_buffer = request.buffer;
    s_expected_length = request.requested;
    s_expected_request_type = 0x81U;
    s_expected_request = 0x06U;
    s_expected_value = 0x2200U;
    s_expected_index = 2U;

    cherryusb_report_retry_result_t result = {0};
    const cherryusb_report_retry_reason_t reason =
        cherryusb_report_descriptor_read_retry(
            &request, request.requested, real_transfer_attempt,
            real_transfer_stopping, &result);

    CHECK(reason == CHERRYUSB_REPORT_RETRY_SUCCESS);
    CHECK(result.attempts == 2U);
    CHECK(result.actual_payload == request.requested);
    CHECK(result.raw_result_valid && result.raw_result == 0);
    CHECK(result.elapsed_us >= 2000U);
    CHECK(s_delay_count == 1U && s_delays_ms[0] == 2U);
    CHECK(request.calls == 2U && s_submit_count == 2U);
    CHECK(s_take_count == 2U && s_give_count == 2U && !s_lock_held);
    CHECK(s_payload_was_zeroed);
}

static void test_retry_reasons(void)
{
    fake_retry_context_t fake;
    cherryusb_report_retry_result_t result;
    cherryusb_report_retry_reason_t reason;

    reset_fakes();
    memset(&fake, 0, sizeof(fake));
    for (uint32_t i = 0U; i < 4U; ++i) {
        fake.statuses[i] = -USB_ERR_NAK;
        fake.raw_results[i] = -USB_ERR_NAK;
        fake.raw_valid[i] = true;
        fake.channel_idle[i] = true;
    }
    reason = cherryusb_report_descriptor_read_retry(
        &fake, 8U, fake_retry_attempt, retry_stopping, &result);
    CHECK(reason == CHERRYUSB_REPORT_RETRY_EXHAUSTED);
    CHECK(result.attempts == 4U && fake.count == 4U);
    CHECK(s_delay_count == 3U);
    CHECK(s_delays_ms[0] == 2U && s_delays_ms[1] == 5U && s_delays_ms[2] == 10U);

    reset_fakes();
    memset(&fake, 0, sizeof(fake));
    fake.statuses[0] = -7;
    fake.raw_results[0] = -7;
    fake.raw_valid[0] = true;
    fake.channel_idle[0] = true;
    reason = cherryusb_report_descriptor_read_retry(
        &fake, 8U, fake_retry_attempt, retry_stopping, &result);
    CHECK(reason == CHERRYUSB_REPORT_RETRY_NON_NAK);
    CHECK(result.attempts == 1U && fake.count == 1U && s_delay_count == 0U);

    reset_fakes();
    memset(&fake, 0, sizeof(fake));
    fake.statuses[0] = 0;
    fake.actual_payloads[0] = 7U;
    fake.channel_idle[0] = true;
    reason = cherryusb_report_descriptor_read_retry(
        &fake, 8U, fake_retry_attempt, retry_stopping, &result);
    CHECK(reason == CHERRYUSB_REPORT_RETRY_SHORT_PAYLOAD);
    CHECK(result.attempts == 1U && s_delay_count == 0U);

    reset_fakes();
    memset(&fake, 0, sizeof(fake));
    fake.statuses[0] = -USB_ERR_NAK;
    fake.raw_results[0] = -USB_ERR_NAK;
    fake.raw_valid[0] = true;
    fake.channel_idle[0] = false;
    reason = cherryusb_report_descriptor_read_retry(
        &fake, 8U, fake_retry_attempt, retry_stopping, &result);
    CHECK(reason == CHERRYUSB_REPORT_RETRY_CHANNEL_BUSY);
    CHECK(result.attempts == 1U && s_delay_count == 0U);

    reset_fakes();
    memset(&fake, 0, sizeof(fake));
    fake.statuses[0] = -USB_ERR_NAK;
    fake.raw_results[0] = -USB_ERR_NAK;
    fake.raw_valid[0] = true;
    fake.channel_idle[0] = true;
    fake.stop_after_attempt = true;
    reason = cherryusb_report_descriptor_read_retry(
        &fake, 8U, fake_retry_attempt, retry_stopping, &result);
    CHECK(reason == CHERRYUSB_REPORT_RETRY_STOPPED);
    CHECK(result.attempts == 1U && fake.count == 1U && s_delay_count == 0U);

    reset_fakes();
    memset(&fake, 0, sizeof(fake));
    fake.statuses[0] = -USB_ERR_NAK;
    fake.raw_results[0] = -USB_ERR_NAK;
    fake.raw_valid[0] = true;
    fake.channel_idle[0] = true;
    fake.time_per_attempt_us = 499000LL;
    reason = cherryusb_report_descriptor_read_retry(
        &fake, 8U, fake_retry_attempt, retry_stopping, &result);
    CHECK(reason == CHERRYUSB_REPORT_RETRY_DEADLINE);
    CHECK(result.attempts == 1U && fake.count == 1U && s_delay_count == 0U);
    CHECK(result.elapsed_us == 499000U);
}

static void test_control_helper_lock_and_stopping_guards(void)
{
    reset_fakes();
    struct usbh_bus bus = { .busid = 0U };
    struct usb_setup_packet setup;
    memset(&setup, 0xA5, sizeof(setup));
    struct usbh_hubport hport = {
        .bus = &bus, .setup = &setup, .mutex = (void *)(uintptr_t)1U,
        .connected = 1U,
    };
    uint8_t buffer[4] = { 0xCCU, 0xCCU, 0xCCU, 0xCCU };
    cherryusb_hid_control_trace_t trace = {0};
    s_take_succeeds = false;
    int ret = cherryusb_hid_control_transfer_until(
        &hport, 0x81U, 0x06U, 0x2200U, 2U, sizeof(buffer), buffer,
        100000LL, &trace);
    CHECK(ret == -USB_ERR_TIMEOUT);
    CHECK(setup.bmRequestType == 0xA5U && setup.bRequest == 0xA5U);
    CHECK(s_submit_count == 0U && s_give_count == 0U);

    reset_fakes();
    memset(&setup, 0xA5, sizeof(setup));
    hport.setup = &setup;
    hport.ep0_urb.hcpriv = NULL;
    s_stopping = true;
    ret = cherryusb_hid_control_transfer_until(
        &hport, 0x81U, 0x06U, 0x2200U, 2U, sizeof(buffer), buffer,
        100000LL, &trace);
    CHECK(ret == -USB_ERR_SHUTDOWN);
    CHECK(s_take_count == 0U && s_submit_count == 0U);

    reset_fakes();
    memset(&setup, 0xA5, sizeof(setup));
    hport.setup = &setup;
    hport.ep0_urb.hcpriv = NULL;
    s_take_succeeds = true;
    s_submit_results[0] = -USB_ERR_NAK;
    s_check_setup_in_submit = true;
    s_expected_buffer = buffer;
    s_expected_length = sizeof(buffer);
    s_expected_request_type = 0x81U;
    s_expected_request = 0x06U;
    s_expected_value = 0x2200U;
    s_expected_index = 2U;
    ret = cherryusb_hid_control_transfer_until(
        &hport, 0x81U, 0x06U, 0x2200U, 2U, sizeof(buffer), buffer,
        100000LL, &trace);
    CHECK(ret == -USB_ERR_NAK);
    CHECK(trace.raw_result_valid && trace.raw_result == -USB_ERR_NAK);
    CHECK(trace.channel_idle && s_submit_count == 1U);
    CHECK(s_give_count == 1U && !s_lock_held);

    reset_fakes();
    memset(&setup, 0xA5, sizeof(setup));
    hport.setup = &setup;
    hport.ep0_urb.hcpriv = NULL;
    memset(buffer, 0xCC, sizeof(buffer));
    s_submit_busy[0] = true;
    ret = cherryusb_hid_control_transfer_until(
        &hport, 0x81U, 0x06U, 0x2200U, 2U, sizeof(buffer), buffer,
        100000LL, &trace);
    CHECK(ret == -USB_ERR_BUSY);
    CHECK(!trace.channel_idle && s_give_count == 1U && !s_lock_held);

    reset_fakes();
    memset(&setup, 0xA5, sizeof(setup));
    hport.setup = &setup;
    hport.ep0_urb.hcpriv = (void *)(uintptr_t)1U;
    memset(buffer, 0xCC, sizeof(buffer));
    ret = cherryusb_hid_control_transfer_until(
        &hport, 0x81U, 0x06U, 0x2200U, 2U, sizeof(buffer), buffer,
        100000LL, &trace);
    CHECK(ret == -USB_ERR_BUSY);
    CHECK(!trace.raw_result_valid && !trace.channel_idle);
    CHECK(s_submit_count == 0U && s_give_count == 1U && !s_lock_held);
    CHECK(setup.bmRequestType == 0xA5U && setup.bRequest == 0xA5U);
    for (size_t i = 0U; i < sizeof(buffer); ++i) {
        CHECK(buffer[i] == 0xCCU);
    }
}

int main(void)
{
    test_retry_nak_then_success_uses_real_transfer();
    test_retry_reasons();
    test_control_helper_lock_and_stopping_guards();
    if (s_failures != 0) {
        fprintf(stderr, "Cherry report descriptor retry fixture: %d failure(s)\n", s_failures);
        return 1;
    }
    puts("Cherry report descriptor production-source retry fixture: PASS");
    puts("  NAK->success, 4-attempt bound, 2/5/10 ms delays, non-NAK, short, channel-busy, deadline, stop: PASS");
    puts("  EP0 mutex-before-SETUP, cleared payload, idle NAK evidence, stop/lock/active-URB guards: PASS");
    return 0;
}
