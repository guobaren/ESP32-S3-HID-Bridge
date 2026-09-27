#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "driver/uart.h"
#include "esp_err.h"

/*
 * Shared UART0 output gate for complete binary frames and complete log lines.
 * 初始化应早于板载日志 vprintf 钩子。UART 驱动可稍后标记就绪；此前用硬件轮询等待 TX 空闲。
 */
esp_err_t dual_uart0_output_init(void);
void dual_uart0_output_set_driver_ready(bool ready);

/* 仅允许任务上下文调用；当前持锁任务递归调用会返回错误。 */
esp_err_t dual_uart0_output_lock(void);
void dual_uart0_output_unlock(void);
esp_err_t dual_uart0_output_wait_tx_idle(void);
esp_err_t dual_uart0_output_write(const void *data, size_t length);
