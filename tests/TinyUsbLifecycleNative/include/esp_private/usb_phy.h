#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void *usb_phy_handle_t;

typedef enum {
    USB_PHY_CTRL_OTG,
} usb_phy_controller_t;

typedef enum {
    USB_PHY_TARGET_INT,
    USB_PHY_TARGET_UTMI,
} usb_phy_target_t;

typedef enum {
    USB_OTG_MODE_DEVICE,
} usb_otg_mode_t;

typedef enum {
    USB_PHY_SPEED_FULL,
    USB_PHY_SPEED_HIGH,
} usb_phy_speed_t;

typedef struct {
    int vbus_monitor_io;
} usb_phy_otg_io_conf_t;

typedef struct {
    usb_phy_controller_t controller;
    usb_phy_target_t target;
    usb_otg_mode_t otg_mode;
    usb_phy_speed_t otg_speed;
    const usb_phy_otg_io_conf_t *otg_io_conf;
} usb_phy_config_t;

#define USB_PHY_SELF_POWERED_DEVICE(io) ((usb_phy_otg_io_conf_t){ .vbus_monitor_io = (io) })

esp_err_t usb_new_phy(const usb_phy_config_t *config, usb_phy_handle_t *handle);
esp_err_t usb_del_phy(usb_phy_handle_t handle);
