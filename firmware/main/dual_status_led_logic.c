#include "dual_status_led_logic.h"

#include <string.h>

static bool deadline_is_after(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(deadline_ms - now_ms) > 0;
}

static void cancel_software_flash(dual_status_led_state_t *state)
{
    state->flash_until_ms = 0U;
    state->next_flash_allowed_ms = 0U;
}

static bool status_is_ready(const dual_status_led_state_t *state)
{
    if (state == NULL || state->flow_error || !state->peer_connected ||
        !state->peer_usb_ready) {
        return false;
    }
    if (state->role == DUAL_STATUS_LED_ROLE_MOUSE_HOST) {
        return state->host_mouse_ready && state->host_profile_ready;
    }
    if (state->role == DUAL_STATUS_LED_ROLE_PC_DEVICE) {
        return state->pc_mounted && state->local_usb_ready;
    }
    return false;
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
    const bool role_changed = state->role != role;
    state->role = role;
    cancel_software_flash(state);
    if (role_changed) {
        /* 只在实际身份切换时清理旧角色的就绪事实；同身份重复同步不得
         * 吞掉刚收到的当前事务 ACK。 */
        state->pc_mounted = false;
        state->host_mouse_ready = false;
        state->host_profile_ready = false;
        state->local_usb_ready = false;
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
        cancel_software_flash(state);
    }
}

void dual_status_led_logic_set_host_mouse_ready(
    dual_status_led_state_t *state,
    bool ready)
{
    if (state != NULL) {
        state->host_mouse_ready = ready;
        if (!ready) {
            cancel_software_flash(state);
        }
    }
}

void dual_status_led_logic_set_host_profile_ready(
    dual_status_led_state_t *state,
    bool ready)
{
    if (state != NULL) {
        state->host_profile_ready = ready;
        if (!ready) {
            cancel_software_flash(state);
        }
    }
}

void dual_status_led_logic_set_local_usb_ready(
    dual_status_led_state_t *state,
    bool ready)
{
    if (state != NULL) {
        state->local_usb_ready = ready;
        if (!ready) {
            cancel_software_flash(state);
        }
    }
}

void dual_status_led_logic_set_peer_connected(
    dual_status_led_state_t *state,
    bool connected)
{
    if (state != NULL) {
        state->peer_connected = connected;
        if (!connected) {
            cancel_software_flash(state);
        }
    }
}

void dual_status_led_logic_set_peer_usb_ready(
    dual_status_led_state_t *state,
    bool ready)
{
    if (state != NULL) {
        state->peer_usb_ready = ready;
        if (!ready) {
            cancel_software_flash(state);
        }
    }
}

void dual_status_led_logic_set_flow_error(
    dual_status_led_state_t *state,
    bool failed)
{
    if (state != NULL) {
        state->flow_error = failed;
        if (failed) {
            cancel_software_flash(state);
        }
    }
}

void dual_status_led_logic_notify_software_success(
    dual_status_led_state_t *state,
    uint32_t now_ms)
{
    if (state == NULL || state->role != DUAL_STATUS_LED_ROLE_PC_DEVICE ||
        !status_is_ready(state)) {
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
    const dual_status_led_color_t identity_color =
        state->role == DUAL_STATUS_LED_ROLE_MOUSE_HOST ?
            DUAL_STATUS_LED_COLOR_GREEN :
        state->role == DUAL_STATUS_LED_ROLE_PC_DEVICE ?
            DUAL_STATUS_LED_COLOR_BLUE : DUAL_STATUS_LED_COLOR_OFF;
    if (identity_color == DUAL_STATUS_LED_COLOR_OFF) {
        return DUAL_STATUS_LED_COLOR_OFF;
    }
    if (!status_is_ready(state)) {
        return ((now_ms / DUAL_STATUS_LED_ERROR_BLINK_MS) & 1U) == 0U
            ? identity_color : DUAL_STATUS_LED_COLOR_RED;
    }
    if (state->role == DUAL_STATUS_LED_ROLE_MOUSE_HOST) {
        return identity_color;
    }
    if (state->role == DUAL_STATUS_LED_ROLE_PC_DEVICE) {
        return deadline_is_after(now_ms, state->flash_until_ms)
            ? DUAL_STATUS_LED_COLOR_FLASH_OFF
            : identity_color;
    }
    return DUAL_STATUS_LED_COLOR_OFF;
}
