#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DUAL_STATUS_LED_FLASH_MS 40U
#define DUAL_STATUS_LED_FLASH_COOLDOWN_MS 60U
#define DUAL_STATUS_LED_ERROR_BLINK_MS 250U

typedef enum {
    DUAL_STATUS_LED_ROLE_NONE = 0,
    DUAL_STATUS_LED_ROLE_MOUSE_HOST,
    DUAL_STATUS_LED_ROLE_PC_DEVICE,
} dual_status_led_role_t;

typedef enum {
    DUAL_STATUS_LED_COLOR_OFF = 0,
    DUAL_STATUS_LED_COLOR_RED,
    DUAL_STATUS_LED_COLOR_GREEN,
    DUAL_STATUS_LED_COLOR_BLUE,
    DUAL_STATUS_LED_COLOR_FLASH_OFF,
} dual_status_led_color_t;

typedef struct {
    dual_status_led_role_t role;
    bool pc_mounted;
    bool host_mouse_ready;
    bool peer_connected;
    bool peer_usb_ready;
    bool flow_error;
    uint32_t flash_until_ms;
    uint32_t next_flash_allowed_ms;
} dual_status_led_state_t;

void dual_status_led_logic_init(dual_status_led_state_t *state);
void dual_status_led_logic_set_role(
    dual_status_led_state_t *state,
    dual_status_led_role_t role);
void dual_status_led_logic_set_pc_mounted(
    dual_status_led_state_t *state,
    bool mounted);
void dual_status_led_logic_set_host_mouse_ready(
    dual_status_led_state_t *state,
    bool ready);
void dual_status_led_logic_set_peer_connected(
    dual_status_led_state_t *state,
    bool connected);
void dual_status_led_logic_set_peer_usb_ready(
    dual_status_led_state_t *state,
    bool ready);
void dual_status_led_logic_set_flow_error(
    dual_status_led_state_t *state,
    bool failed);
void dual_status_led_logic_notify_software_success(
    dual_status_led_state_t *state,
    uint32_t now_ms);
dual_status_led_color_t dual_status_led_logic_color(
    const dual_status_led_state_t *state,
    uint32_t now_ms);
