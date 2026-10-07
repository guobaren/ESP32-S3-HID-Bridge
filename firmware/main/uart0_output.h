#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "driver/uart.h"
#include "esp_err.h"

/*
 * Shared UART0 output gate for complete binary frames and complete log lines.
 * 初始化应早于 ESP_LOG vprintf 钩子。UART 驱动可稍后标记就绪；此前用硬件轮询等待 TX 空闲。
 */
esp_err_t dual_uart0_output_init(void);
void dual_uart0_output_set_driver_ready(bool ready);
void dual_uart0_output_set_makcu_ascii_mode(bool enabled);
void dual_uart0_output_set_makcu_mode(bool enabled);

/* 仅允许任务上下文调用；当前持锁任务递归调用会返回错误。 */
esp_err_t dual_uart0_output_lock(void);
void dual_uart0_output_unlock(void);
esp_err_t dual_uart0_output_wait_tx_idle(void);
esp_err_t dual_uart0_output_write(const void *data, size_t length);
/* 仅在 Makcu ASCII 独占模式下写响应；普通日志和 A5 帧会被屏蔽。 */
esp_err_t dual_uart0_output_write_makcu_ascii(const void *data, size_t length);
/* V4 ASCII、MAK_API replies 与 unsolicited event 共用同一独占输出通道。 */
esp_err_t dual_uart0_output_write_makcu(const void *data, size_t length);
