#pragma once

#include <stdint.h>

struct usbh_bus {
    uint8_t busid;
};

struct usb_setup_packet {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct usbh_urb {
    void *hcpriv;
    uint32_t actual_length;
    struct usbh_hubport *hport;
    struct usb_setup_packet *setup;
    uint8_t *buffer;
    uint16_t length;
    uint32_t timeout_ms;
};

struct usbh_hubport {
    struct usbh_bus *bus;
    struct usb_setup_packet *setup;
    void *mutex;
    uint8_t connected;
    struct usbh_urb ep0_urb;
};

void usbh_control_urb_fill(struct usbh_urb *urb,
                           struct usbh_hubport *hport,
                           struct usb_setup_packet *setup,
                           uint8_t *buffer,
                           uint16_t length,
                           uint32_t timeout_ms,
                           void *complete,
                           void *arg);
int usbh_submit_urb(struct usbh_urb *urb);
