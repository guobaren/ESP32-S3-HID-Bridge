#include "dual_status_led_logic.h"

#include <string.h>

static bool deadline_is_after(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(deadline_ms - now_ms) > 0;
}

void dual_status_led_logic_init(dual_status_led_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void dual_status_led_logic_set_role(
    dual_status_led_state_t *state,
    dual_status_led_role_t role)
{
    if (state == NULL) {
        return;
    }
    state->role = role;
    state->flash_until_ms = 0;
    state->next_flash_allowed_ms = 0;
    if (role != DUAL_STATUS_LED_ROLE_PC_DEVICE) {
        state->pc_mounted = false;
    }
    if (role != DUAL_STATUS_LED_ROLE_MOUSE_HOST) {
        state->host_mouse_ready = false;
    }
}

void dual_status_led_logic_set_pc_mounted(
    dual_status_led_state_t *state,
    bool mounted)
{
    if (state == NULL) {
        return;
    }
    state->pc_mounted = mounted;
    if (!mounted) {
        state->flash_until_ms = 0;
        state->next_flash_allowed_ms = 0;
    }
}

void dual_status_led_logic_set_host_mouse_ready(
    dual_status_led_state_t *state,
    bool ready)
{
    if (state != NULL) {
        state->host_mouse_ready = ready;
    }
}

void dual_status_led_logic_set_peer_connected(
    dual_status_led_state_t *state,
    bool connected)
{
    if (state != NULL) {
        state->peer_connected = connected;
    }
}

void dual_status_led_logic_set_flow_error(
    dual_status_led_state_t *state,
    bool failed)
{
    if (state != NULL) {
        state->flow_error = failed;
    }
}

void dual_status_led_logic_notify_software_success(
    dual_status_led_state_t *state,
    uint32_t now_ms)
{
    if (state == NULL || state->role != DUAL_STATUS_LED_ROLE_PC_DEVICE ||
        !state->pc_mounted) {
        return;
    }
    if (!deadline_is_after(now_ms, state->flash_until_ms) &&
        !deadline_is_after(now_ms, state->next_flash_allowed_ms)) {
        state->flash_until_ms = now_ms + DUAL_STATUS_LED_FLASH_MS;
        state->next_flash_allowed_ms = now_ms +
            DUAL_STATUS_LED_FLASH_MS + DUAL_STATUS_LED_FLASH_COOLDOWN_MS;
    }
}

dual_status_led_color_t dual_status_led_logic_color(
    const dual_status_led_state_t *state,
    uint32_t now_ms)
{
    if (state == NULL) {
        return DUAL_STATUS_LED_COLOR_OFF;
    }
    if (state->role == DUAL_STATUS_LED_ROLE_NONE) {
        return DUAL_STATUS_LED_COLOR_OFF;
    }
    if (!state->peer_connected) {
        return DUAL_STATUS_LED_COLOR_RED;
    }
    if (state->flow_error) {
        return ((now_ms / DUAL_STATUS_LED_ERROR_BLINK_MS) & 1U) == 0U
            ? DUAL_STATUS_LED_COLOR_RED
            : DUAL_STATUS_LED_COLOR_OFF;
    }
    if (state->role == DUAL_STATUS_LED_ROLE_MOUSE_HOST) {
        return DUAL_STATUS_LED_COLOR_GREEN;
    }
    if (state->role == DUAL_STATUS_LED_ROLE_PC_DEVICE) {
        return state->pc_mounted && deadline_is_after(now_ms, state->flash_until_ms)
            ? DUAL_STATUS_LED_COLOR_FLASH_OFF
            : DUAL_STATUS_LED_COLOR_BLUE;
    }
    return DUAL_STATUS_LED_COLOR_OFF;
}
