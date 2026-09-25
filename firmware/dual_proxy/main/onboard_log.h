#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 板载滚动日志：分区上的文本日志，跨复位续写，容量有上限并轮转。
 *
 * 目标是在“调试串口没插、目标电脑看不到控制台”的线序测试里也能拿到两板的
 * 完整历史，因此 dual_onboard_log_start() 要在 app_main 最先调用——早于
 * UART1/USB 初始化和身份探测。
 *
 * 容量：4 个文件 × 128 KB（合计 512 KB），写满后覆盖最旧的。
 * 读取：逻辑字节流按“最旧 → 最新”排列，offset 从 0 开始。
 */

#define DUAL_LOG_FILE_COUNT 4U
#define DUAL_LOG_FILE_BYTES (512U * 1024U)
#define DUAL_LOG_TOTAL_BYTES (DUAL_LOG_FILE_COUNT * DUAL_LOG_FILE_BYTES)
#define DUAL_LOG_CHUNK_MAX 56U

/* 挂载分区、安装日志钩子并启动写盘任务；失败不阻塞启动，只停用板载日志。 */
esp_err_t dual_onboard_log_start(void);

/* 板载日志当前可读的总字节数（尚未落盘的缓冲不计入）。 */
uint32_t dual_onboard_log_total_bytes(void);

/* 请求写盘任务尽快 fflush，读日志前调用可拿到最新尾部（异步，不阻塞）。 */
void dual_onboard_log_request_flush(void);

/* 运行时暂停/恢复板载写盘；暂停期间钩子只计数不落盘（用于 A/B 对照排查时序影响）。 */
void dual_onboard_log_set_paused(bool paused);

/* 输入路径活跃时调用：写盘任务会让路，避免 flash 停顿饿到 UART1 接收。 */
void dual_onboard_log_note_input_activity(void);
bool dual_onboard_log_paused(void);

/* 从逻辑 offset 读取最多 max 字节，返回实际读到的字节数。 */
size_t dual_onboard_log_read(uint32_t offset, uint8_t *output, size_t max);

/* 清空全部日志文件与状态（下次启动 boot 计数继续，不清零）。 */
esp_err_t dual_onboard_log_clear(void);

/* 因缓冲满而丢弃的行数，以及是否已成功挂载。 */
uint32_t dual_onboard_log_dropped_lines(void);
bool dual_onboard_log_ready(void);
