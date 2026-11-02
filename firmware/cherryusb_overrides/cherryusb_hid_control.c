#include "cherryusb_hid_control.h"

#include <string.h>

#include "cherryusb_host_low_level.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usbh_core.h"
#include "usb_errno.h"

static TickType_t remaining_ticks(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0U;
    }
    uint64_t remaining_ms = ((uint64_t)remaining_us + 999U) / 1000U;
    if (remaining_ms > UINT32_MAX) {
        remaining_ms = UINT32_MAX;
    }
    TickType_t ticks = pdMS_TO_TICKS((uint32_t)remaining_ms);
    return ticks == 0U ? 1U : ticks;
}

int cherryusb_hid_control_transfer_until(
    struct usbh_hubport *hport,
    uint8_t bm_request_type,
    uint8_t b_request,
    uint16_t w_value,
    uint16_t w_index,
    uint16_t w_length,
    uint8_t *buffer,
    int64_t deadline_us,
    cherryusb_hid_control_trace_t *trace)
{
    if (trace != NULL) {
        *trace = (cherryusb_hid_control_trace_t){ .channel_idle = true };
    }
    if (hport == NULL || hport->bus == NULL || hport->setup == NULL ||
        hport->mutex == NULL || (w_length != 0U && buffer == NULL)) {
        return -USB_ERR_INVAL;
    }
    if (dual_cherryusb_hub_is_stopping(hport->bus) || !hport->connected) {
        return -USB_ERR_SHUTDOWN;
    }

    TickType_t ticks = remaining_ticks(deadline_us);
    if (ticks == 0U || xSemaphoreTake((SemaphoreHandle_t)hport->mutex, ticks) != pdTRUE) {
        return -USB_ERR_TIMEOUT;
    }

    int ret = -USB_ERR_SHUTDOWN;
    if (dual_cherryusb_hub_is_stopping(hport->bus) || !hport->connected) {
        goto cleanup;
    }
    ticks = remaining_ticks(deadline_us);
    if (ticks == 0U) {
        ret = -USB_ERR_TIMEOUT;
        goto cleanup;
    }
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        ret = -USB_ERR_TIMEOUT;
        goto cleanup;
    }
    uint64_t remaining_ms64 = ((uint64_t)remaining_us + 999U) / 1000U;
    uint32_t remaining_ms = remaining_ms64 > UINT32_MAX
        ? UINT32_MAX : (uint32_t)remaining_ms64;
    if (remaining_ms == 0U) {
        ret = -USB_ERR_TIMEOUT;
        goto cleanup;
    }
    if (hport->ep0_urb.hcpriv != NULL) {
        if (trace != NULL) {
            trace->channel_idle = false;
        }
        ret = -USB_ERR_BUSY;
        goto cleanup;
    }

    /* 共享 SETUP 只能在拿到 EP0 mutex 后写，避免与 heartbeat 争用。 */
    hport->setup->bmRequestType = bm_request_type;
    hport->setup->bRequest = b_request;
    hport->setup->wValue = w_value;
    hport->setup->wIndex = w_index;
    hport->setup->wLength = w_length;
    if (buffer != NULL && w_length != 0U) {
        memset(buffer, 0, w_length);
    }

    usbh_control_urb_fill(&hport->ep0_urb, hport, hport->setup,
                          w_length == 0U ? NULL : buffer,
                          w_length, remaining_ms, NULL, NULL);
    const int raw_result = usbh_submit_urb(&hport->ep0_urb);
    const bool channel_idle = hport->ep0_urb.hcpriv == NULL;
    if (trace != NULL) {
        trace->raw_result = raw_result;
        trace->raw_result_valid = true;
        trace->channel_idle = channel_idle;
    }
    if (!channel_idle) {
        ret = -USB_ERR_BUSY;
        goto cleanup;
    }
    if (dual_cherryusb_hub_is_stopping(hport->bus) || !hport->connected) {
        ret = -USB_ERR_SHUTDOWN;
        goto cleanup;
    }
    if (raw_result < 0) {
        ret = raw_result;
        goto cleanup;
    }

    const uint32_t actual_length = hport->ep0_urb.actual_length;
    if (actual_length < sizeof(struct usb_setup_packet)) {
        ret = -USB_ERR_IO;
        goto cleanup;
    }
    const size_t payload_length = actual_length - sizeof(struct usb_setup_packet);
    if (payload_length > w_length) {
        ret = -USB_ERR_RANGE;
        goto cleanup;
    }
    if (trace != NULL) {
        trace->payload_length = payload_length;
    }
    /* 保留 CherryUSB 原语义：成功返回 SETUP(8 B)+实际 payload 长度。 */
    ret = (int)actual_length;

cleanup:
    xSemaphoreGive((SemaphoreHandle_t)hport->mutex);
    return ret;
}
