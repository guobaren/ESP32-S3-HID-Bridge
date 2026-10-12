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

int usbh_hid_get_report_descriptor(struct usbh_hid *hid_class, uint8_t *buffer, uint32_t buflen)
{
    struct usb_setup_packet *setup;

    if (!hid_class || !hid_class->hport) {
        return -USB_ERR_INVAL;
    }
    setup = hid_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup->wValue = HID_DESCRIPTOR_TYPE_HID_REPORT << 8;
    setup->wIndex = hid_class->intf;
    setup->wLength = buflen;

    return usbh_control_transfer(hid_class->hport, setup, buffer);
}

int usbh_hid_set_idle(struct usbh_hid *hid_class, uint8_t report_id, uint8_t duration)
{
    struct usb_setup_packet *setup;

    if (!hid_class || !hid_class->hport) {
        return -USB_ERR_INVAL;
    }
    setup = hid_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = HID_REQUEST_SET_IDLE;
    setup->wValue = (duration << 8) | report_id;
    setup->wIndex = hid_class->intf;
    setup->wLength = 0;

    return usbh_control_transfer(hid_class->hport, setup, NULL);
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
                        USB_LOG_ERR("HID load failed: stage=hid_descriptor_validate bus=%u addr=%u intf_index=%u intf_num=%u raw_return=%d report_size=unknown elapsed_us=%" PRId64 "\r\n",
                                    (unsigned)bus_id, (unsigned)hport->dev_addr,
                                    (unsigned)intf, (unsigned)interface_number,
                                    -USB_ERR_INVAL, esp_timer_get_time() - connect_started_us);
                        return -USB_ERR_INVAL;
                    }

                    if (desc->subdesc[0].bDescriptorType != HID_DESCRIPTOR_TYPE_HID_REPORT) {
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
