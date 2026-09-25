#pragma once

#include "bridge_protocol.h"
#include "esp_err.h"

typedef void (*dual_software_report_callback_t)(const dual_frame_t *frame);
typedef void (*dual_software_release_callback_t)(void);

esp_err_t dual_uart0_control_start(
    uint8_t role,
    dual_software_report_callback_t report_callback,
    dual_software_release_callback_t release_callback);

/*
 * 只服务板载日志命令的 UART0 任务（电脑侧板调试口用）：应答 LOG_READ/CLEAR，
 * 不接受软件输入会话，也不转发 MouseReport。
 */
esp_err_t dual_uart0_log_service_start(uint8_t role);
