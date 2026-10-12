#pragma once

#include "bridge_protocol.h"
#include "dual_proxy_runtime_config.h"
#include "esp_err.h"

typedef void (*dual_software_report_callback_t)(const dual_frame_t *frame);
typedef void (*dual_software_release_callback_t)(void);
typedef uint8_t (*dual_software_physical_buttons_callback_t)(void);

#if DUAL_PROXY_ENABLE_BUTTON_STATE
/* Called only for decoded physical mouse reports and immediate mouse disconnects. */
uint32_t dual_uart0_control_button_state_capture_epoch(void);
uint32_t dual_uart0_control_button_state_capture_epoch_isr(void);
void dual_uart0_control_button_state_update(uint8_t buttons, uint32_t observation_epoch);
void dual_uart0_control_button_state_invalidate_for_epoch(uint32_t observation_epoch);
void dual_uart0_control_button_state_invalidate(void);
#endif

esp_err_t dual_uart0_control_start(
    uint8_t role,
    dual_software_report_callback_t report_callback,
    dual_software_release_callback_t release_callback,
    dual_software_physical_buttons_callback_t physical_buttons_callback);

/* 由 UART 控制任务清空待发送软件位移并释放按钮。 */
void dual_uart0_control_cancel_software_input(void);

#if DUAL_PROXY_ENABLE_MAKCU_V4_API
/* Called by the M raw-report worker before forwarding each physical report. */
void dual_uart0_control_v4_physical_buttons(uint8_t buttons);
void dual_uart0_control_v4_cancel_input(void);
void dual_uart0_control_v4_get_physical_masks(
    uint8_t *button_mask,
    uint8_t *move_mask,
    uint8_t *wheel_mask);
void dual_uart0_control_v4_track_physical_move(int32_t x, int32_t y);
#endif
