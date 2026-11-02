#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

struct usbh_bus;

esp_err_t dual_cherryusb_host_low_level_init(struct usbh_bus *bus);
esp_err_t dual_cherryusb_host_low_level_enable_irq(struct usbh_bus *bus);
esp_err_t dual_cherryusb_host_low_level_disable_irq(struct usbh_bus *bus);
esp_err_t dual_cherryusb_host_low_level_deinit(struct usbh_bus *bus);
bool dual_cherryusb_host_low_level_ready(struct usbh_bus *bus);
bool dual_cherryusb_bus_is_active(uint8_t busid);
/* 在 Host/Hub 停止阶段 fail-closed；仅查询 Hub lifecycle 标志。 */
bool dual_cherryusb_hub_is_stopping(struct usbh_bus *bus);
