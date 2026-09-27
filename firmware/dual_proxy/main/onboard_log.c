#include "onboard_log.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "uart0_output.h"

#define LOG_PARTITION_LABEL "storage"
#define LOG_MOUNT_POINT "/spiffs"
#define LOG_STATE_PATH LOG_MOUNT_POINT "/state.bin"
#define LOG_STATE_MAGIC 0x474F4C31U /* "1LOG" */
#define LOG_WRITE_STEP_BYTES 512U
#define LOG_FLUSH_INTERVAL_US 500000LL
#define LOG_TASK_STACK 4096U
#define LOG_TASK_PRIORITY 2U
#define LOG_TASK_CORE 0
#define LOG_LINE_MAX 512U

#define LOG_DROP_REPORT_INTERVAL_US 60000000LL
#define LOG_QUEUE_LENGTH 16U
#define LOG_ENTRY_MAX (LOG_LINE_MAX + 32U)

typedef struct {
    uint16_t length;
    char text[LOG_ENTRY_MAX];
} log_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t next_index;  /* 当前追加写入的文件下标 */
    uint32_t sequence;    /* 单调递增序号，用于判断哪个文件最旧 */
    uint32_t boot_count;
    uint32_t file_sequence[DUAL_LOG_FILE_COUNT]; /* 0 表示该文件为空 */
} log_state_t;

static const char *TAG = "dual_log";

static vprintf_like_t s_previous_vprintf;
/*
 * 用“整块入队”而不是字节流：StreamBuffer 空间不足时会部分写入，
 * 截断的半个前缀会插进上一行中间（实测到 `dual_pc_hid:51.861] I (…)` 这种断行）。
 * 队列以条目为单位，要么整条进、要么整条丢，天然不会串行。
 */
static QueueHandle_t s_queue;
static TaskHandle_t s_writer_task;
static FILE *s_file;
static log_state_t s_state;
static uint32_t s_current_bytes;
static volatile uint32_t s_total_bytes;
static volatile uint32_t s_dropped_lines;
static volatile bool s_ready;
static volatile bool s_flush_requested;
/* 运行时暂停：A/B 对照用（暂停后钩子不再入队、写盘任务不再落盘）。 */
static volatile bool s_paused;
static volatile uint32_t s_paused_drops;
static volatile int64_t s_last_stats_capture_us;
static bool s_mounted;
/*
 * 每核一个静态条目缓冲：日志钩子会被任意任务调用（含栈只有 2048 字节的
 * hid_host_stats）。原来在栈上放 512 字节行缓冲 + 544 字节条目，直接把小栈
 * 任务压爆——实测 `***ERROR*** A stack overflow in task hid_host_stats has
 * been detected`，而该 panic 会连锁成“对端 generation 反复变化 → 电脑侧反复
 * 重建克隆 → 驱动不断重连”。改为静态缓冲 + 短暂挂起调度器保证独占，
 * 钩子的栈占用降到几十字节。
 */
static log_entry_t s_entry_pool[2];
/*
 * 写盘节流：flash 写入/擦除/GC 期间缓存被关、两个核都停，实测会把 UART1 接收
 * 任务饿到缓冲溢出（A/B 对照：写盘开启时移动 +6396 报文 → 溢出 81 次；写盘
 * 暂停时 +11381 报文 → 溢出 0 次）。这里用令牌桶限制落盘速率，并在输入活跃期
 * 主动让路，避免日志影响 1 kHz 输入路径。
 */
#define LOG_WRITE_BUDGET_BYTES_PER_SEC 4096U
#define LOG_INPUT_QUIET_US 50000LL
static volatile int64_t s_last_input_activity_us;
static uint32_t s_write_tokens;
static int64_t s_token_refill_us;

static void log_file_path(uint32_t index, char *buffer, size_t capacity)
{
    snprintf(buffer, capacity, LOG_MOUNT_POINT "/log%" PRIu32 ".txt", index);
}

static uint32_t s_file_bytes_cache[DUAL_LOG_FILE_COUNT];

static uint32_t log_file_bytes(uint32_t index)
{
    return index < DUAL_LOG_FILE_COUNT ? s_file_bytes_cache[index] : 0U;
}

static void log_refresh_file_bytes(uint32_t index)
{
    if (index >= DUAL_LOG_FILE_COUNT) {
        return;
    }
    char path[32];
    struct stat info;
    log_file_path(index, path, sizeof(path));
    if (stat(path, &info) != 0 || info.st_size < 0) {
        s_file_bytes_cache[index] = 0U;
        return;
    }
    s_file_bytes_cache[index] = (uint32_t)info.st_size;
}

static void log_refresh_all_file_bytes(void)
{
    for (uint32_t index = 0U; index < DUAL_LOG_FILE_COUNT; ++index) {
        log_refresh_file_bytes(index);
    }
}

static uint32_t log_recompute_total(void)
{
    uint32_t total = 0U;
    for (uint32_t index = 0U; index < DUAL_LOG_FILE_COUNT; ++index) {
        const uint32_t bytes = log_file_bytes(index);
        total += bytes > DUAL_LOG_FILE_BYTES ? DUAL_LOG_FILE_BYTES : bytes;
    }
    return total > DUAL_LOG_TOTAL_BYTES ? DUAL_LOG_TOTAL_BYTES : total;
}

static void log_load_state(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.magic = LOG_STATE_MAGIC;
    FILE *file = fopen(LOG_STATE_PATH, "rb");
    if (file == NULL) {
        return;
    }
    log_state_t stored;
    const size_t read = fread(&stored, 1, sizeof(stored), file);
    fclose(file);
    if (read != sizeof(stored) || stored.magic != LOG_STATE_MAGIC ||
        stored.next_index >= DUAL_LOG_FILE_COUNT) {
        return;
    }
    s_state = stored;
    uint32_t maximum = 0U;
    for (uint32_t index = 0U; index < DUAL_LOG_FILE_COUNT; ++index) {
        if (s_state.file_sequence[index] > maximum) {
            maximum = s_state.file_sequence[index];
        }
    }
    if (s_state.sequence < maximum) {
        s_state.sequence = maximum;
    }
}

static void log_save_state(void)
{
    FILE *file = fopen(LOG_STATE_PATH, "wb");
    if (file == NULL) {
        return;
    }
    (void)fwrite(&s_state, 1, sizeof(s_state), file);
    fclose(file);
}

static void log_close_file(void)
{
    if (s_file != NULL) {
        fflush(s_file);
        fclose(s_file);
        s_file = NULL;
    }
}

static bool log_open_append(uint32_t index)
{
    char path[32];
    log_file_path(index, path, sizeof(path));
    s_file = fopen(path, "ab");
    if (s_file == NULL) {
        return false;
    }
    log_refresh_file_bytes(index);
    s_current_bytes = log_file_bytes(index);
    if (s_state.file_sequence[index] == 0U) {
        /* 首次创建或状态丢失：补一个序号，否则读取端会把该文件当成空。 */
        s_state.sequence += 1U;
        s_state.file_sequence[index] = s_state.sequence;
        log_save_state();
    }
    return true;
}

static void log_rotate(void)
{
    log_close_file();
    s_state.next_index = (s_state.next_index + 1U) % DUAL_LOG_FILE_COUNT;
    s_state.sequence += 1U;
    s_state.file_sequence[s_state.next_index] = s_state.sequence;
    log_save_state();
    char path[32];
    log_file_path(s_state.next_index, path, sizeof(path));
    /* 覆盖最旧的槽位：始终只有 4 个文件，总字节数不超过上限。 */
    s_file = fopen(path, "wb");
    s_current_bytes = 0U;
    log_refresh_file_bytes(s_state.next_index);
}

static bool log_ensure_file(void)
{
    if (s_file != NULL) {
        return true;
    }
    if (s_current_bytes >= DUAL_LOG_FILE_BYTES) {
        log_rotate();
        return s_file != NULL;
    }
    return log_open_append(s_state.next_index);
}

static void log_write(const char *data, size_t length)
{
    size_t offset = 0U;
    while (offset < length) {
        if (!log_ensure_file()) {
            return;
        }
        size_t room = DUAL_LOG_FILE_BYTES - s_current_bytes;
        if (room == 0U) {
            log_rotate();
            continue;
        }
        size_t chunk = length - offset;
        if (chunk > room) {
            chunk = room;
        }
        if (chunk > LOG_WRITE_STEP_BYTES) {
            chunk = LOG_WRITE_STEP_BYTES;
        }
        const size_t written = fwrite(data + offset, 1, chunk, s_file);
        if (written == 0U) {
            return;
        }
        offset += written;
        s_current_bytes += (uint32_t)written;
        s_file_bytes_cache[s_state.next_index] = s_current_bytes;
        const uint32_t total = s_total_bytes + (uint32_t)written;
        s_total_bytes = total > DUAL_LOG_TOTAL_BYTES ? DUAL_LOG_TOTAL_BYTES : total;
    }
}

/*
 * 周期统计行（1 Hz）是日志体积的主要来源：全量写入会让 512 KB 只能覆盖约
 * 11 分钟。这里把统计行节流到每 10 秒一条，事件行（角色、Profile、恢复事务）
 * 全部保留，于是历史长度提升到小时级，同时仍保留“板子还活着”的心跳证据。
 */
#define LOG_STATS_INTERVAL_US 10000000LL

static bool log_line_is_periodic_stats(const char *line)
{
    static const char *const markers[] = {
        "UART1统计", "HID统计", "Host HID统计", "UART0协议统计", "CDC协议统计",
    };
    for (size_t index = 0U; index < sizeof(markers) / sizeof(markers[0]); ++index) {
        if (strstr(line, markers[index]) != NULL) {
            return true;
        }
    }
    return false;
}

/*
 * 高频行：G HUB 轮询厂商报文时每条打两行（摘要 + hex），实测峰值 170 行/秒、
 * 占过日志全文 23%，也是把 flash 写爆、进而饿到 UART1 接收的元凶之一。
 * 这类行在板载副本里限流（控制台仍全量输出），保留少量样本 + 定期汇总。
 * 注意：移动报文（1 kHz）本身不打印任何行，只进计数器。
 */
#define LOG_HIGH_RATE_BURST 4U
#define LOG_HIGH_RATE_INTERVAL_US 1000000LL
#define LOG_HIGH_RATE_SUMMARY_INTERVAL_US 10000000LL
static uint32_t s_high_rate_burst;
static int64_t s_high_rate_last_us;
static uint32_t s_high_rate_suppressed;
static int64_t s_high_rate_summary_us;

static bool log_line_is_hex_continuation(const char *line)
{
    const char *body = strstr(line, ": ");
    if (body == NULL) {
        return false;
    }
    body += 2;
    size_t digits = 0U;
    size_t pairs = 0U;
    for (const char *cursor = body; *cursor != '\0'; ++cursor) {
        const char character = *cursor;
        if (character == ' ' || character == '\n' || character == '\r') {
            if (digits == 2U) {
                ++pairs;
            }
            digits = 0U;
            continue;
        }
        const bool is_hex = (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f') ||
            (character >= 'A' && character <= 'F');
        if (!is_hex || digits >= 2U) {
            return false;
        }
        ++digits;
    }
    if (digits == 2U) {
        ++pairs;
    }
    return pairs >= 2U;
}

static bool log_line_is_high_rate(const char *line)
{
    static const char *const markers[] = {
        "PC SET_REPORT", "物理 SET_REPORT",
    };
    for (size_t index = 0U; index < sizeof(markers) / sizeof(markers[0]); ++index) {
        if (strstr(line, markers[index]) != NULL) {
            return true;
        }
    }
    return log_line_is_hex_continuation(line);
}

static int log_capture_vprintf(const char *format, va_list arguments)
{
    va_list copy;
    va_copy(copy, arguments);
    int written = 0;
    if (dual_uart0_output_lock() == ESP_OK) {
        written = s_previous_vprintf != NULL ? s_previous_vprintf(format, arguments)
                                             : vprintf(format, arguments);
        /* 整条日志离开 UART0 后才释放共享输出锁。 */
        (void)dual_uart0_output_wait_tx_idle();
        dual_uart0_output_unlock();
    }
    /*
     * 写盘任务自身的日志不回灌（避免“写日志触发日志”），中断上下文不采集。
     * 发送不阻塞：缓冲满就丢行并计数，绝不拖慢调用方——1 kHz 路径也走这里。
     * 缓冲用每核静态池（见 s_entry_pool 注释），栈上不再放大缓冲。
     */
    if (s_ready && s_queue != NULL && !xPortInIsrContext() &&
        xTaskGetCurrentTaskHandle() != s_writer_task) {
        if (s_paused) {
            ++s_paused_drops;
            va_end(copy);
            return written;
        }
        log_entry_t *pool = &s_entry_pool[xPortGetCoreID() & 1U];
        char *entry_text = pool->text;
        const int64_t now_us = esp_timer_get_time();
        const int64_t seconds = now_us / 1000000LL;
        /* 挂起调度器：静态池必须独占，期间只做一次格式化 + 一次入队。 */
        vTaskSuspendAll();
        const int prefix = snprintf(entry_text, LOG_ENTRY_MAX,
                                    "[b%" PRIu32 " %02" PRId64 ":%02" PRId64 ".%03" PRId64 "] ",
                                    s_state.boot_count, seconds / 60, seconds % 60,
                                    (now_us / 1000) % 1000);
        if (prefix <= 0 || (size_t)prefix >= LOG_ENTRY_MAX) {
            (void)xTaskResumeAll();
            va_end(copy);
            return written;
        }
        const size_t room = LOG_ENTRY_MAX - (size_t)prefix;
        const int body = vsnprintf(entry_text + prefix, room, format, copy);
        if (body <= 0) {
            (void)xTaskResumeAll();
            va_end(copy);
            return written;
        }
        size_t to_send = (size_t)prefix + ((size_t)body < room ? (size_t)body : room - 1U);
        if (log_line_is_periodic_stats(entry_text) &&
            now_us - s_last_stats_capture_us < LOG_STATS_INTERVAL_US) {
            (void)xTaskResumeAll();
            va_end(copy);
            return written;
        }
        if (log_line_is_periodic_stats(entry_text)) {
            s_last_stats_capture_us = now_us;
        }
        /*
         * 高频厂商/hex 行限流：允许每次活动开头 4 行（便于看清格式），之后
         * 每秒最多 1 行；被省略的条数在 10 秒汇总里报告，信息不丢只是不逐条落盘。
         */
        if (log_line_is_high_rate(entry_text)) {
            if (now_us - s_high_rate_last_us > LOG_HIGH_RATE_INTERVAL_US) {
                s_high_rate_burst = 0U;
            }
            s_high_rate_last_us = now_us;
            if (s_high_rate_burst >= LOG_HIGH_RATE_BURST) {
                ++s_high_rate_suppressed;
                if (now_us - s_high_rate_summary_us >= LOG_HIGH_RATE_SUMMARY_INTERVAL_US) {
                    s_high_rate_summary_us = now_us;
                    const int summary = snprintf(
                        entry_text, LOG_ENTRY_MAX,
                        "[b%" PRIu32 " %02" PRId64 ":%02" PRId64 ".%03" PRId64
                        "] 已省略 %" PRIu32 " 条高频厂商/hex 日志（控制台仍全量）\n",
                        s_state.boot_count, seconds / 60, seconds % 60,
                        (now_us / 1000) % 1000, s_high_rate_suppressed);
                    if (summary > 0) {
                        pool->length = (uint16_t)((size_t)summary < LOG_ENTRY_MAX ?
                                                  (size_t)summary : LOG_ENTRY_MAX - 1U);
                        (void)xQueueSend(s_queue, pool, 0);
                    }
                }
                (void)xTaskResumeAll();
                va_end(copy);
                return written;
            }
            ++s_high_rate_burst;
        }
        if ((size_t)body >= room) {
            /* 超长行截断并标记，避免与下一行连在一起看不出断点。 */
            static const char marker[] = "…[截断]\n";
            if (to_send + sizeof(marker) - 1U <= LOG_ENTRY_MAX) {
                memcpy(&entry_text[to_send], marker, sizeof(marker) - 1U);
                to_send += sizeof(marker) - 1U;
            }
        }
        pool->length = (uint16_t)to_send;
        /* 直接以静态池为源入队：队列会拷贝整条，栈上不需要任何大缓冲。 */
        const BaseType_t queued = xQueueSend(s_queue, pool, 0);
        (void)xTaskResumeAll();
        if (queued != pdTRUE) {
            ++s_dropped_lines;
        }
    }
    va_end(copy);
    return written;
}

static void log_write_boot_banner(void)
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char banner[224];
    const int length = snprintf(
        banner, sizeof(banner),
        "\n===== 板载日志 boot=%" PRIu32 " mac=%02X%02X%02X%02X%02X%02X"
        " reset_reason=%d t=%" PRId64 "us =====\n",
        s_state.boot_count + 1U, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        (int)esp_reset_reason(), esp_timer_get_time());
    if (length > 0) {
        log_write(banner, (size_t)length);
    }
}

/*
 * 令牌桶：返回本次最多允许落盘的字节数。输入路径刚刚活跃时不给令牌，
 * 让写盘让路（滚动缓冲里的日志会稍后落盘，不会丢）。
 */
static size_t log_take_write_budget(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (s_last_input_activity_us != 0 &&
        now_us - s_last_input_activity_us < LOG_INPUT_QUIET_US) {
        return 0U;
    }
    if (s_token_refill_us == 0) {
        s_token_refill_us = now_us;
        s_write_tokens = LOG_WRITE_BUDGET_BYTES_PER_SEC;
    }
    const int64_t elapsed_us = now_us - s_token_refill_us;
    if (elapsed_us > 0) {
        const uint32_t refill = (uint32_t)((elapsed_us * LOG_WRITE_BUDGET_BYTES_PER_SEC) / 1000000LL);
        if (refill > 0U) {
            s_write_tokens += refill;
            if (s_write_tokens > LOG_WRITE_BUDGET_BYTES_PER_SEC) {
                s_write_tokens = LOG_WRITE_BUDGET_BYTES_PER_SEC;
            }
            s_token_refill_us = now_us;
        }
    }
    if (s_write_tokens == 0U) {
        return 0U;
    }
    size_t budget = s_write_tokens;
    if (budget > 512U) {
        budget = 512U; /* 单次最多 512 字节：把 flash 停顿切成小块 */
    }
    s_write_tokens -= (uint32_t)budget;
    return budget;
}

static void log_writer_task(void *argument)
{
    (void)argument;
    log_load_state();
    log_refresh_all_file_bytes();
    if (!log_ensure_file()) {
        s_ready = false;
        vTaskDelete(NULL);
        return;
    }
    log_write_boot_banner();
    s_state.boot_count += 1U;
    log_save_state();
    s_total_bytes = log_recompute_total();
    s_ready = true;
    ESP_LOGI(TAG, "板载日志就绪：boot=%" PRIu32 " 文件=%u×%uKB 已存=%" PRIu32 "B",
             s_state.boot_count, (unsigned)DUAL_LOG_FILE_COUNT,
             (unsigned)(DUAL_LOG_FILE_BYTES / 1024U), s_total_bytes);

    log_entry_t entry;
    int64_t last_flush_us = esp_timer_get_time();
    int64_t last_drop_report_us = last_flush_us;
    uint32_t reported_drops = 0U;
    while (true) {
        if (xQueueReceive(s_queue, &entry, pdMS_TO_TICKS(200)) == pdTRUE) {
            /*
             * 令牌用完或输入正活跃就让路，但**必须把整条写完**：早先的写法在
             * 令牌为 0 时直接退出循环，把条目后半截丢掉，日志里会出现
             * `待完成USB卸17.858] W (...)` 这种半行。
             */
            size_t written = 0U;
            while (written < entry.length) {
                size_t budget = log_take_write_budget();
                if (budget == 0U) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                size_t step = entry.length - written;
                if (step > budget) {
                    step = budget;
                }
                log_write(entry.text + written, step);
                written += step;
            }
        }
        const int64_t now_us = esp_timer_get_time();
        if (s_file != NULL && (s_flush_requested ||
                               now_us - last_flush_us >= LOG_FLUSH_INTERVAL_US)) {
            fflush(s_file);
            s_flush_requested = false;
            last_flush_us = now_us;
        }
        /*
         * 丢弃行只报到控制台（本任务自己的日志不回灌板载文件），
         * 这样“板载日志有缺口”这件事在串口上看得见，且不会自我递归。
         */
        if (now_us - last_drop_report_us >= LOG_DROP_REPORT_INTERVAL_US) {
            last_drop_report_us = now_us;
            const uint32_t dropped = s_dropped_lines;
            if (dropped != reported_drops) {
                reported_drops = dropped;
                ESP_LOGW(TAG, "板载日志缓冲满，累计丢弃 %" PRIu32 " 行（控制台不受影响）",
                         dropped);
            }
        }
    }
}

/*
 * A/B 实验开关：置 1 时板载日志"开机即暂停"——完全不写 flash（复位也不会恢复
 * 写盘，运行时暂停做不到这一点，因为暂停标志在 RAM 里、复位即失效）。
 * 历史上用于判定"控制传输失败"是否由写盘停顿引起；2026-09-27 起用于
 * UART1 硬件 FIFO_OVF 的 A/B（flash 写/擦除期间 cache 关、双核停是否饿到
 * UART ISR）。判定完请改回 0。
 * 注意：暂停分支在 UART0 输出之后返回，因此 UART0 控制台日志照常输出，
 * 但会同时跳过日志的格式化与入队——本开关对比的是"整条板载日志落盘路径"，
 * 不是单独隔离 fwrite。
 */
#define ONBOARD_LOG_PAUSED_AT_BOOT 1

esp_err_t dual_onboard_log_start(void)
{
    /* UART0 console logging starts before uart0_control installs its driver. */
    const esp_err_t output_result = dual_uart0_output_init();
    if (output_result != ESP_OK) {
        return output_result;
    }
    const esp_vfs_spiffs_conf_t config = {
        .base_path = LOG_MOUNT_POINT,
        .partition_label = LOG_PARTITION_LABEL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    const esp_err_t mount_result = esp_vfs_spiffs_register(&config);
    if (mount_result != ESP_OK) {
        return mount_result;
    }
    s_mounted = true;
#if ONBOARD_LOG_PAUSED_AT_BOOT
    s_paused = true;
    /* 明确宣告本次启动不写 flash：采集侧据此确认 A/B 条件确实生效。 */
    ESP_LOGW(TAG, "板载写盘开机即暂停（A/B 对照 ONBOARD_LOG_PAUSED_AT_BOOT=1）："
                  "本次启动不写 flash，日志只走 UART0");
#endif
    s_queue = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(log_entry_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(log_writer_task, "dual_log", LOG_TASK_STACK, NULL,
                                LOG_TASK_PRIORITY, &s_writer_task, LOG_TASK_CORE) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* 钩子在写盘任务起来之后再装，避免启动早期日志挤占缓冲。 */
    s_previous_vprintf = esp_log_set_vprintf(log_capture_vprintf);
    return ESP_OK;
}

uint32_t dual_onboard_log_total_bytes(void)
{
    return s_total_bytes;
}

void dual_onboard_log_set_paused(bool paused)
{
    s_paused = paused;
}

void dual_onboard_log_note_input_activity(void)
{
    s_last_input_activity_us = esp_timer_get_time();
}

bool dual_onboard_log_paused(void)
{
    return s_paused;
}

void dual_onboard_log_request_flush(void)
{
    /* 交给写盘任务去 fflush：FILE* 只由它持有，跨任务调用不安全。 */
    s_flush_requested = true;
}

uint32_t dual_onboard_log_dropped_lines(void)
{
    return s_dropped_lines;
}

bool dual_onboard_log_ready(void)
{
    return s_ready;
}

size_t dual_onboard_log_read(uint32_t offset, uint8_t *output, size_t max)
{
    if (!s_mounted || output == NULL || max == 0U || offset >= DUAL_LOG_TOTAL_BYTES) {
        return 0U;
    }
    /* 按序号升序读取 = 逻辑上的“最旧 → 最新”。 */
    uint32_t order[DUAL_LOG_FILE_COUNT];
    uint32_t count = 0U;
    for (uint32_t index = 0U; index < DUAL_LOG_FILE_COUNT; ++index) {
        if (s_state.file_sequence[index] != 0U) {
            order[count++] = index;
        }
    }
    for (uint32_t a = 0U; a < count; ++a) {
        for (uint32_t b = a + 1U; b < count; ++b) {
            if (s_state.file_sequence[order[b]] < s_state.file_sequence[order[a]]) {
                const uint32_t swap = order[a];
                order[a] = order[b];
                order[b] = swap;
            }
        }
    }

    uint32_t skip = offset;
    size_t produced = 0U;
    for (uint32_t step = 0U; step < count && produced < max; ++step) {
        uint32_t bytes = log_file_bytes(order[step]);
        if (bytes > DUAL_LOG_FILE_BYTES) {
            bytes = DUAL_LOG_FILE_BYTES;
        }
        if (skip >= bytes) {
            skip -= bytes;
            continue;
        }
        char path[32];
        log_file_path(order[step], path, sizeof(path));
        FILE *file = fopen(path, "rb");
        if (file == NULL) {
            continue;
        }
        if (skip > 0U) {
            fseek(file, (long)skip, SEEK_SET);
            skip = 0U;
        }
        /*
         * 必须在文件边界继续读下一个文件：只读一次 fread 的话，请求跨过
         * 128 KB 文件边界时会在边界处提前结束（实测 dump 只拿到 389/524 KB）。
         */
        while (produced < max) {
            const size_t want = max - produced;
            const size_t got = fread(output + produced, 1, want, file);
            if (got == 0U) {
                break;
            }
            produced += got;
        }
        fclose(file);
    }
    return produced;
}

esp_err_t dual_onboard_log_clear(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ready = false;
    log_close_file();
    for (uint32_t index = 0U; index < DUAL_LOG_FILE_COUNT; ++index) {
        char path[32];
        log_file_path(index, path, sizeof(path));
        (void)remove(path);
        s_state.file_sequence[index] = 0U;
    }
    s_state.next_index = 0U;
    s_state.sequence += 1U;
    log_save_state();
    s_current_bytes = 0U;
    s_total_bytes = 0U;
    log_refresh_all_file_bytes();
    s_ready = true;
    return ESP_OK;
}
