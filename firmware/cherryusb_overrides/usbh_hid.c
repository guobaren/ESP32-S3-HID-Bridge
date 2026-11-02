/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * CherryUSB 1.5.2~2 的项目本地纯日志覆盖副本。
 * 上游源文件 SHA-256：D4B669AECA2A6493495F3852207460AB99241FE4A2199C46353270D62958ADCC
 * 仅在 CONFIG_DUAL_PROXY_USE_CHERRYUSB 启用时编译此副本。
 */
#include "usbh_core.h"
#include "usbh_hid.h"
#include "cherryusb_hid_control.h"
#include "cherryusb_host_low_level.h"
#include "cherryusb_report_retry.h"
#include "usb_host_diagnostics.h"

#include <inttypes.h>
#include <stdio.h>
#include "esp_timer.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_hid"
#include "usb_log.h"

#define DEV_FORMAT "/dev/input%d"

/* general descriptor field offsets */
#define DESC_bLength         0 /** Length offset */
#define DESC_bDescriptorType 1 /** Descriptor type offset */

/* interface descriptor field offsets */
#define INTF_DESC_bInterfaceNumber  2 /** Interface number offset */
#define INTF_DESC_bAlternateSetting 3 /** Alternate setting offset */

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_hid_buf[CONFIG_USBHOST_MAX_HID_CLASS][USB_ALIGN_UP(64, CONFIG_USB_ALIGN_SIZE)];

static struct usbh_hid g_hid_class[CONFIG_USBHOST_MAX_HID_CLASS];
static uint32_t g_devinuse = 0;

static struct usbh_hid *usbh_hid_class_alloc(void)
{
    uint8_t devno;

    for (devno = 0; devno < CONFIG_USBHOST_MAX_HID_CLASS; devno++) {
        if ((g_devinuse & (1U << devno)) == 0) {
            g_devinuse |= (1U << devno);
            memset(&g_hid_class[devno], 0, sizeof(struct usbh_hid));
            g_hid_class[devno].minor = devno;
            return &g_hid_class[devno];
        }
    }
    return NULL;
}

static void usbh_hid_class_free(struct usbh_hid *hid_class)
{
    uint8_t devno = hid_class->minor;

    if (devno < 32) {
        g_devinuse &= ~(1U << devno);
    }
    memset(hid_class, 0, sizeof(struct usbh_hid));
}

static uint8_t usbh_hid_diag_interface_number(struct usbh_hubport *hport,
                                              uint8_t intf_index)
{
    if (hport == NULL || intf_index >= hport->config.config_desc.bNumInterfaces ||
        hport->config.intf[intf_index].altsetting_num == 0U) {
        return UINT8_MAX;
    }
    return hport->config.intf[intf_index].altsetting[0].intf_desc.bInterfaceNumber;
}

static const char *usbh_hid_diag_report_length(int result, char *buffer,
                                                size_t buffer_size)
{
    const int setup_size = (int)sizeof(struct usb_setup_packet);
    if (result < setup_size) {
        return "unknown";
    }
    (void)snprintf(buffer, buffer_size, "%d", result - setup_size);
    return buffer;
}

typedef struct {
    struct usbh_hid *hid_class;
    uint8_t *buffer;
    uint16_t requested_length;
} hid_report_request_context_t;

static bool hid_report_request_stopping(void *context)
{
    const hid_report_request_context_t *request = context;
    return request == NULL || request->hid_class == NULL ||
        request->hid_class->hport == NULL ||
        !request->hid_class->hport->connected ||
        dual_cherryusb_hub_is_stopping(request->hid_class->hport->bus);
}

static int hid_report_request_attempt(
    void *context,
    uint32_t attempt,
    int64_t deadline_us,
    int *raw_result,
    bool *raw_result_valid,
    size_t *actual_payload,
    bool *channel_idle)
{
    hid_report_request_context_t *request = context;
    struct usbh_hid *hid_class = request->hid_class;
    const int64_t started_us = esp_timer_get_time();
    cherryusb_hid_control_trace_t trace = {0};
    const int ret = cherryusb_hid_control_transfer_until(
        hid_class->hport,
        USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_INTERFACE,
        USB_REQUEST_GET_DESCRIPTOR,
        HID_DESCRIPTOR_TYPE_HID_REPORT << 8,
        hid_class->intf, request->requested_length, request->buffer,
        deadline_us, &trace);
    *raw_result = trace.raw_result;
    *raw_result_valid = trace.raw_result_valid;
    *actual_payload = trace.payload_length;
    *channel_idle = trace.channel_idle;
    USB_LOG_INFO("HID request: stage=get_report_descriptor attempt=%" PRIu32
                 " wIndex=%u requested=%u raw_urb_valid=%u raw_urb=%d actual_payload=%u channel_idle=%u elapsed_us=%" PRId64 "\r\n",
                 attempt, (unsigned)hid_class->intf,
                 (unsigned)request->requested_length,
                 trace.raw_result_valid ? 1U : 0U, trace.raw_result,
                 (unsigned)trace.payload_length, trace.channel_idle ? 1U : 0U,
                 esp_timer_get_time() - started_us);
    return ret < 0 ? ret : 0;
}

int usbh_hid_get_report_descriptor(struct usbh_hid *hid_class, uint8_t *buffer, uint32_t buflen)
{
    if (!hid_class || !hid_class->hport || buffer == NULL || buflen == 0U ||
        buflen > UINT16_MAX) {
        return -USB_ERR_INVAL;
    }
    hid_report_request_context_t request = {
        .hid_class = hid_class,
        .buffer = buffer,
        .requested_length = (uint16_t)buflen,
    };
    cherryusb_report_retry_result_t retry = {0};
    const cherryusb_report_retry_reason_t reason =
        cherryusb_report_descriptor_read_retry(
            &request, buflen, hid_report_request_attempt,
            hid_report_request_stopping, &retry);
    USB_LOG_INFO("HID descriptor retry summary: wIndex=%u requested=%u attempts=%" PRIu32
                 " actual_payload=%u total_us=%" PRIu32 " budget_ms=%u reason=%s recovered=%u\r\n",
                 (unsigned)hid_class->intf, (unsigned)buflen, retry.attempts,
                 (unsigned)retry.actual_payload, retry.elapsed_us,
                 CHERRYUSB_REPORT_RETRY_BUDGET_MS,
                 cherryusb_report_retry_reason_name(reason),
                 (reason == CHERRYUSB_REPORT_RETRY_SUCCESS && retry.attempts > 1U) ? 1U : 0U);
    if (reason == CHERRYUSB_REPORT_RETRY_SUCCESS) {
        return (int)(sizeof(struct usb_setup_packet) + retry.actual_payload);
    }
    if (reason == CHERRYUSB_REPORT_RETRY_SHORT_PAYLOAD) {
        return -USB_ERR_RANGE;
    }
    if (reason == CHERRYUSB_REPORT_RETRY_STOPPED) {
        return -USB_ERR_SHUTDOWN;
    }
    if (reason == CHERRYUSB_REPORT_RETRY_DEADLINE) {
        return -USB_ERR_TIMEOUT;
    }
    if (retry.raw_result_valid && retry.raw_result < 0) {
        return retry.raw_result;
    }
    if (retry.status < 0) {
        return retry.status;
    }
    return -USB_ERR_IO;
}

int usbh_hid_set_idle(struct usbh_hid *hid_class, uint8_t report_id, uint8_t duration)
{
    if (!hid_class || !hid_class->hport) {
        return -USB_ERR_INVAL;
    }
    const int64_t deadline_us = esp_timer_get_time() +
        ((int64_t)CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT * 1000LL);
    return cherryusb_hid_control_transfer_until(
        hid_class->hport,
        USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE,
        HID_REQUEST_SET_IDLE, (uint16_t)((duration << 8U) | report_id),
        hid_class->intf, 0U, NULL, deadline_us, NULL);
}

int usbh_hid_get_idle(struct usbh_hid *hid_class, uint8_t *buffer)
{
    struct usb_setup_packet *setup;
    int ret;

    if (!hid_class || !hid_class->hport) {
        return -USB_ERR_INVAL;
    }
    setup = hid_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = HID_REQUEST_GET_IDLE;
    setup->wValue = 0;
    setup->wIndex = hid_class->intf;
    setup->wLength = 1;

    ret = usbh_control_transfer(hid_class->hport, setup, g_hid_buf[hid_class->minor]);
    if (ret < 8) {
        return ret;
    }
    memcpy(buffer, g_hid_buf[hid_class->minor], MIN(ret - 8, 1));
    return ret;
}

int usbh_hid_set_protocol(struct usbh_hid *hid_class, uint8_t protocol)
{
    struct usb_setup_packet *setup;

    if (!hid_class || !hid_class->hport) {
        return -USB_ERR_INVAL;
    }
    setup = hid_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = HID_REQUEST_SET_PROTOCOL;
    setup->wValue = protocol;
    setup->wIndex = 0;
    setup->wLength = 0;

    return usbh_control_transfer(hid_class->hport, setup, NULL);
}

int usbh_hid_get_protocol(struct usbh_hid *hid_class, uint8_t *protocol)
{
    struct usb_setup_packet *setup;
    int ret;

    if (!hid_class || !hid_class->hport) {
        return -USB_ERR_INVAL;
    }
    setup = hid_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = HID_REQUEST_GET_PROTOCOL;
    setup->wValue = 0;
    setup->wIndex = hid_class->intf;
    setup->wLength = 1;

    ret = usbh_control_transfer(hid_class->hport, setup, g_hid_buf[hid_class->minor]);
    if (ret < 8) {
        return ret;
    }
    memcpy(protocol, g_hid_buf[hid_class->minor], MIN(ret - 8, 1));
    return ret;
}

int usbh_hid_set_report(struct usbh_hid *hid_class, uint8_t report_type, uint8_t report_id, uint8_t *buffer, uint32_t buflen)
{
    struct usb_setup_packet *setup;

    if (!hid_class || !hid_class->hport) {
        return -USB_ERR_INVAL;
    }
    setup = hid_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = HID_REQUEST_SET_REPORT;
    setup->wValue = (uint16_t)(((uint32_t)report_type << 8U) | (uint32_t)report_id);
    setup->wIndex = 0;
    setup->wLength = buflen;

    return usbh_control_transfer(hid_class->hport, setup, buffer);
}

int usbh_hid_get_report(struct usbh_hid *hid_class, uint8_t report_type, uint8_t report_id, uint8_t *buffer, uint32_t buflen)
{
    struct usb_setup_packet *setup;
    int ret;

    if (!hid_class || !hid_class->hport) {
        return -USB_ERR_INVAL;
    }
    setup = hid_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = HID_REQUEST_GET_REPORT;
    setup->wValue = (uint16_t)(((uint32_t)report_type << 8U) | (uint32_t)report_id);
    setup->wIndex = 0;
    setup->wLength = buflen;

    ret = usbh_control_transfer(hid_class->hport, setup, g_hid_buf[hid_class->minor]);
    if (ret < 8) {
        return ret;
    }
    memcpy(buffer, g_hid_buf[hid_class->minor], MIN(ret - 8, buflen));
    return ret;
}

int usbh_hid_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usb_endpoint_descriptor *ep_desc;
    int ret;
    uint8_t cur_iface = 0xff;
    uint8_t *p;
    bool found = false;
    const int64_t connect_started_us = esp_timer_get_time();
    const uint8_t bus_id = hport != NULL && hport->bus != NULL
        ? hport->bus->busid : UINT8_MAX;
    const uint8_t interface_number = usbh_hid_diag_interface_number(hport, intf);

    USB_LOG_INFO("HID load begin: stage=class_alloc bus=%u addr=%u intf_index=%u intf_num=%u vid=%04x pid=%04x\r\n",
                 (unsigned)bus_id, hport != NULL ? (unsigned)hport->dev_addr : UINT8_MAX,
                 (unsigned)intf, (unsigned)interface_number,
                 hport != NULL ? (unsigned)hport->device_desc.idVendor : 0U,
                 hport != NULL ? (unsigned)hport->device_desc.idProduct : 0U);

    struct usbh_hid *hid_class = usbh_hid_class_alloc();
    if (hid_class == NULL) {
        dual_usb_host_diag_failure(
            DUAL_USB_HOST_DIAG_STAGE_HID_CLASS_ALLOC_USB, -USB_ERR_NOMEM);
        USB_LOG_ERR("HID load failed: stage=class_alloc bus=%u addr=%u intf_index=%u intf_num=%u raw_return=%d report_size=unknown elapsed_us=%" PRId64 "\r\n",
                    (unsigned)bus_id, hport != NULL ? (unsigned)hport->dev_addr : UINT8_MAX,
                    (unsigned)intf, (unsigned)interface_number, -USB_ERR_NOMEM,
                    esp_timer_get_time() - connect_started_us);
        return -USB_ERR_NOMEM;
    }

    hid_class->hport = hport;
    hid_class->intf = intf;

    hport->config.intf[intf].priv = hid_class;

    p = hport->raw_config_desc;
    while (p[DESC_bLength]) {
        switch (p[DESC_bDescriptorType]) {
            case USB_DESCRIPTOR_TYPE_INTERFACE:
                cur_iface = p[INTF_DESC_bInterfaceNumber];
                if (cur_iface == intf) {
                    hid_class->protocol = p[7];
                    struct usb_hid_descriptor *desc = (struct usb_hid_descriptor *)(p + 9);

                    if (desc->bDescriptorType != HID_DESCRIPTOR_TYPE_HID) {
                        dual_usb_host_diag_failure(
                            DUAL_USB_HOST_DIAG_STAGE_HID_DESCRIPTOR_VALIDATE_USB,
                            -USB_ERR_INVAL);
                        USB_LOG_ERR("HID load failed: stage=hid_descriptor_validate bus=%u addr=%u intf_index=%u intf_num=%u raw_return=%d report_size=unknown elapsed_us=%" PRId64 "\r\n",
                                    (unsigned)bus_id, (unsigned)hport->dev_addr,
                                    (unsigned)intf, (unsigned)interface_number,
                                    -USB_ERR_INVAL, esp_timer_get_time() - connect_started_us);
                        return -USB_ERR_INVAL;
                    }

                    if (desc->subdesc[0].bDescriptorType != HID_DESCRIPTOR_TYPE_HID_REPORT) {
                        dual_usb_host_diag_failure(
                            DUAL_USB_HOST_DIAG_STAGE_HID_REPORT_DESCRIPTOR_HEADER_USB,
                            -USB_ERR_INVAL);
                        USB_LOG_ERR("HID load failed: stage=report_descriptor_header_validate bus=%u addr=%u intf_index=%u intf_num=%u raw_return=%d report_size=unknown elapsed_us=%" PRId64 "\r\n",
                                    (unsigned)bus_id, (unsigned)hport->dev_addr,
                                    (unsigned)intf, (unsigned)interface_number,
                                    -USB_ERR_INVAL, esp_timer_get_time() - connect_started_us);
                        return -USB_ERR_INVAL;
                    }

                    hid_class->report_size = desc->subdesc[0].wDescriptorLength;
                    found = true;
                    goto found;
                }
                break;
            default:
                break;
        }
        /* skip to next descriptor */
        p += p[DESC_bLength];
    }

    if (found == false) {
        dual_usb_host_diag_failure(
            DUAL_USB_HOST_DIAG_STAGE_HID_INTERFACE_LOOKUP_USB, -USB_ERR_INVAL);
        USB_LOG_ERR("HID load failed: stage=interface_descriptor_lookup bus=%u addr=%u intf_index=%u intf_num=%u raw_return=%d report_size=unknown elapsed_us=%" PRId64 "\r\n",
                    (unsigned)bus_id, (unsigned)hport->dev_addr,
                    (unsigned)intf, (unsigned)interface_number,
                    -USB_ERR_INVAL, esp_timer_get_time() - connect_started_us);
        return -USB_ERR_INVAL;
    }
found:
    USB_LOG_INFO("HID descriptor parsed: bus=%u addr=%u intf_index=%u intf_num=%u class_index=%u subclass=%u protocol=%u report_size=%u\r\n",
                 (unsigned)bus_id, (unsigned)hport->dev_addr, (unsigned)intf,
                 (unsigned)interface_number, (unsigned)hid_class->minor,
                 (unsigned)hport->config.intf[intf].altsetting[0].intf_desc.bInterfaceSubClass,
                 (unsigned)hid_class->protocol, (unsigned)hid_class->report_size);

    // /* 0x0 = boot protocol, 0x1 = report protocol */
    // ret = usbh_hid_set_protocol(hid_class, 0x1);
    // if (ret < 0) {
    //     return ret;
    // }

    int64_t request_started_us = esp_timer_get_time();
    ret = usbh_hid_set_idle(hid_class, 0, 0);
    const int64_t set_idle_elapsed_us = esp_timer_get_time() - request_started_us;
    if (ret < 0) {
        USB_LOG_WRN("HID request: stage=set_idle bus=%u addr=%u intf_index=%u intf_num=%u class_index=%u wIndex=%u report_id=0 duration=0 raw_return=%d elapsed_us=%" PRId64 " optional=continue\r\n",
                    (unsigned)bus_id, (unsigned)hport->dev_addr, (unsigned)intf,
                    (unsigned)interface_number, (unsigned)hid_class->minor,
                    (unsigned)hid_class->intf, ret, set_idle_elapsed_us);
    } else {
        USB_LOG_INFO("HID request: stage=set_idle bus=%u addr=%u intf_index=%u intf_num=%u class_index=%u wIndex=%u report_id=0 duration=0 raw_return=%d elapsed_us=%" PRId64 "\r\n",
                     (unsigned)bus_id, (unsigned)hport->dev_addr, (unsigned)intf,
                     (unsigned)interface_number, (unsigned)hid_class->minor,
                     (unsigned)hid_class->intf, ret, set_idle_elapsed_us);
    }

    /* We read report desc but do nothing (because of too much memory usage for parsing report desc, parsed by users) */
    const uint32_t requested_report_length =
        MIN(sizeof(g_hid_buf[hid_class->minor]), hid_class->report_size);
    request_started_us = esp_timer_get_time();
    ret = usbh_hid_get_report_descriptor(
        hid_class, g_hid_buf[hid_class->minor], requested_report_length);
    const int64_t report_descriptor_elapsed_us =
        esp_timer_get_time() - request_started_us;
    char actual_report_length[16];
    if (ret < 0) {
        dual_usb_host_diag_failure(
            DUAL_USB_HOST_DIAG_STAGE_HID_GET_REPORT_DESCRIPTOR_USB, ret);
        USB_LOG_ERR("HID request failed: stage=get_report_descriptor bus=%u addr=%u intf_index=%u intf_num=%u class_index=%u wIndex=%u requested=%" PRIu32 " raw_return=%d actual_payload=unknown elapsed_us=%" PRId64 "\r\n",
                    (unsigned)bus_id, (unsigned)hport->dev_addr, (unsigned)intf,
                    (unsigned)interface_number, (unsigned)hid_class->minor,
                    (unsigned)hid_class->intf, requested_report_length, ret,
                    report_descriptor_elapsed_us);
        return ret;
    }
    USB_LOG_INFO("HID request: stage=get_report_descriptor bus=%u addr=%u intf_index=%u intf_num=%u class_index=%u wIndex=%u requested=%" PRIu32 " raw_return=%d actual_payload=%s elapsed_us=%" PRId64 "\r\n",
                 (unsigned)bus_id, (unsigned)hport->dev_addr, (unsigned)intf,
                 (unsigned)interface_number, (unsigned)hid_class->minor,
                 (unsigned)hid_class->intf, requested_report_length, ret,
                 usbh_hid_diag_report_length(ret, actual_report_length,
                                             sizeof(actual_report_length)),
                 report_descriptor_elapsed_us);

    for (uint8_t i = 0; i < hport->config.intf[intf].altsetting[0].intf_desc.bNumEndpoints; i++) {
        ep_desc = &hport->config.intf[intf].altsetting[0].ep[i].ep_desc;
        if (ep_desc->bEndpointAddress & 0x80) {
            USBH_EP_INIT(hid_class->intin, ep_desc);
        } else {
            USBH_EP_INIT(hid_class->intout, ep_desc);
        }
    }

    snprintf(hport->config.intf[intf].devname, CONFIG_USBHOST_DEV_NAMELEN, DEV_FORMAT, hid_class->minor);

    USB_LOG_INFO("Register HID Class:%s\r\n", hport->config.intf[intf].devname);

    USB_LOG_INFO("HID run call: stage=before bus=%u addr=%u intf_index=%u intf_num=%u class_index=%u report_size=%u\r\n",
                 (unsigned)bus_id, (unsigned)hport->dev_addr, (unsigned)intf,
                 (unsigned)interface_number, (unsigned)hid_class->minor,
                 (unsigned)hid_class->report_size);
    const int64_t run_started_us = esp_timer_get_time();
    usbh_hid_run(hid_class);
    USB_LOG_INFO("HID run call: stage=returned bus=%u addr=%u intf_index=%u intf_num=%u class_index=%u elapsed_us=%" PRId64 "\r\n",
                 (unsigned)bus_id, (unsigned)hport->dev_addr, (unsigned)intf,
                 (unsigned)interface_number, (unsigned)hid_class->minor,
                 esp_timer_get_time() - run_started_us);
    return ret;
}

int usbh_hid_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    int ret = 0;

    struct usbh_hid *hid_class = (struct usbh_hid *)hport->config.intf[intf].priv;

    if (hid_class) {
        if (hid_class->intin) {
            usbh_kill_urb(&hid_class->intin_urb);
        }

        if (hid_class->intout) {
            usbh_kill_urb(&hid_class->intout_urb);
        }

        if (hport->config.intf[intf].devname[0] != '\0') {
            usb_osal_thread_schedule_other();
            USB_LOG_INFO("Unregister HID Class:%s\r\n", hport->config.intf[intf].devname);
            usbh_hid_stop(hid_class);
        }

        usbh_hid_class_free(hid_class);
    }

    return ret;
}

__WEAK void usbh_hid_run(struct usbh_hid *hid_class)
{
    (void)hid_class;
}

__WEAK void usbh_hid_stop(struct usbh_hid *hid_class)
{
    (void)hid_class;
}

const struct usbh_class_driver hid_class_driver = {
    .driver_name = "hid",
    .connect = usbh_hid_connect,
    .disconnect = usbh_hid_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info hid_custom_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS,
    .bInterfaceClass = USB_DEVICE_CLASS_HID,
    .bInterfaceSubClass = 0x00,
    .bInterfaceProtocol = 0x00,
    .id_table = NULL,
    .class_driver = &hid_class_driver
};
