#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef uint8_t tinyusb_port_t;
struct tinyusb_event;
typedef void (*tinyusb_event_cb_t)(const struct tinyusb_event *event, void *arg);

typedef struct {
    size_t size;
    uint8_t priority;
    int xCoreID;
} tinyusb_task_config_t;

typedef struct {
    const void *device;
    const void *string;
    size_t string_count;
    const void *full_speed_config;
} tinyusb_desc_config_t;

typedef struct {
    bool skip_setup;
    bool self_powered;
    int vbus_monitor_io;
} tinyusb_phy_config_t;

typedef struct tinyusb_event {
    int id;
    tinyusb_port_t rhport;
    struct {
        bool remote_wakeup;
    } suspended;
} tinyusb_event_t;

enum {
    TINYUSB_EVENT_ATTACHED = 1,
    TINYUSB_EVENT_DETACHED,
    TINYUSB_EVENT_SUSPENDED,
    TINYUSB_EVENT_RESUMED,
};

typedef struct {
    tinyusb_port_t port;
    tinyusb_phy_config_t phy;
    tinyusb_task_config_t task;
    tinyusb_desc_config_t descriptor;
    tinyusb_event_cb_t event_cb;
    void *event_arg;
    bool pm_lock_enable;
} tinyusb_config_t;

enum {
    TINYUSB_PORT_FULL_SPEED_0 = 0,
    TINYUSB_PORT_HIGH_SPEED_0 = 1,
    TINYUSB_PORT_MAX = 2,
};

esp_err_t tinyusb_driver_install(const tinyusb_config_t *config);
esp_err_t tinyusb_driver_uninstall(void);
esp_err_t tinyusb_remote_wakeup(void);
