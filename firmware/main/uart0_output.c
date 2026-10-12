#include "uart0_output.h"

#include <limits.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "dual_proxy_runtime_config.h"
#include "bridge_protocol.h"

#define UART0_OUTPUT_PORT UART_NUM_0

static StaticSemaphore_t s_output_mutex_storage;
static SemaphoreHandle_t s_output_mutex;
static TaskHandle_t s_output_owner;
static volatile bool s_driver_ready;
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API || DUAL_PROXY_ENABLE_MAKCU_V4_API
static volatile bool s_makcu_ascii_mode;
#endif
static vprintf_like_t s_previous_vprintf;

static int serialized_vprintf(const char *format, va_list args)
{
    if (s_previous_vprintf == NULL) {
        return 0;
    }
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API || DUAL_PROXY_ENABLE_MAKCU_V4_API
    if (__atomic_load_n(&s_makcu_ascii_mode, __ATOMIC_ACQUIRE)) {
        return 0;
    }
#endif
    if (dual_uart0_output_lock() != ESP_OK) {
        /* 失败时丢弃整行，绝不在帧持锁期间绕过串行化输出。 */
        return 0;
    }
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API || DUAL_PROXY_ENABLE_MAKCU_V4_API
    if (__atomic_load_n(&s_makcu_ascii_mode, __ATOMIC_ACQUIRE)) {
        dual_uart0_output_unlock();
        return 0;
    }
#endif
    const int result = s_previous_vprintf(format, args);
    (void)dual_uart0_output_wait_tx_idle();
    dual_uart0_output_unlock();
    return result;
}

esp_err_t dual_uart0_output_init(void)
{
    /* app_main 在安装日志钩子或创建 UART 控制任务前调用。 */
    if (s_output_mutex == NULL) {
        s_output_mutex = xSemaphoreCreateMutexStatic(&s_output_mutex_storage);
    }
    if (s_output_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (s_previous_vprintf == NULL) {
        s_previous_vprintf = esp_log_set_vprintf(serialized_vprintf);
    }
    return s_previous_vprintf != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void dual_uart0_output_set_driver_ready(bool ready)
{
    __atomic_store_n(&s_driver_ready, ready, __ATOMIC_RELEASE);
}

void dual_uart0_output_set_makcu_ascii_mode(bool enabled)
{
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API || DUAL_PROXY_ENABLE_MAKCU_V4_API
    __atomic_store_n(&s_makcu_ascii_mode, enabled, __ATOMIC_RELEASE);
#else
    (void)enabled;
#endif
}

void dual_uart0_output_set_makcu_mode(bool enabled)
{
    dual_uart0_output_set_makcu_ascii_mode(enabled);
}

esp_err_t dual_uart0_output_lock(void)
{
    if (xPortInIsrContext() || s_output_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (__atomic_load_n(&s_output_owner, __ATOMIC_ACQUIRE) == current) {
        /* 当前任务持有输出锁时不允许递归输出，避免驱动/日志回调死锁。 */
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_output_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    __atomic_store_n(&s_output_owner, current, __ATOMIC_RELEASE);
    return ESP_OK;
}

esp_err_t dual_uart0_output_try_lock(void)
{
    if (xPortInIsrContext() || s_output_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (__atomic_load_n(&s_output_owner, __ATOMIC_ACQUIRE) == current) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_output_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    __atomic_store_n(&s_output_owner, current, __ATOMIC_RELEASE);
    return ESP_OK;
}

void dual_uart0_output_unlock(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (s_output_mutex == NULL ||
        __atomic_load_n(&s_output_owner, __ATOMIC_ACQUIRE) != current) {
        return;
    }
    __atomic_store_n(&s_output_owner, NULL, __ATOMIC_RELEASE);
    (void)xSemaphoreGive(s_output_mutex);
}

esp_err_t dual_uart0_output_wait_tx_idle(void)
{
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (xPortInIsrContext() ||
        __atomic_load_n(&s_output_owner, __ATOMIC_ACQUIRE) != current) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!__atomic_load_n(&s_driver_ready, __ATOMIC_ACQUIRE)) {
        /* UART VFS 在驱动安装前可直接写 FIFO，轮询等待硬件真正空闲。 */
        return uart_wait_tx_idle_polling(UART0_OUTPUT_PORT);
    }
    return uart_wait_tx_done(UART0_OUTPUT_PORT, portMAX_DELAY);
}

esp_err_t dual_uart0_output_write_locked_nonblocking(
    const void *data, size_t length, bool allow_makcu_mode)
{
    if ((data == NULL && length != 0U) || length > INT_MAX ||
        length > 9U + DUAL_PROXY_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length == 0U) {
        return ESP_OK;
    }
    if (xPortInIsrContext() ||
        __atomic_load_n(&s_output_owner, __ATOMIC_ACQUIRE) !=
            xTaskGetCurrentTaskHandle() ||
        !__atomic_load_n(&s_driver_ready, __ATOMIC_ACQUIRE)) {
        return ESP_ERR_INVALID_STATE;
    }
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API || DUAL_PROXY_ENABLE_MAKCU_V4_API
    if (!allow_makcu_mode &&
        __atomic_load_n(&s_makcu_ascii_mode, __ATOMIC_ACQUIRE)) {
        return ESP_ERR_INVALID_STATE;
    }
#else
    (void)allow_makcu_mode;
#endif
    size_t free_bytes = 0U;
    const esp_err_t free_result = uart_get_tx_buffer_free_size(
        UART0_OUTPUT_PORT, &free_bytes);
    if (free_result != ESP_OK) {
        return free_result;
    }
    if (free_bytes < length) {
        return ESP_ERR_NO_MEM;
    }
    /*
     * 所有 UART0 写入都先持有本模块的 output mutex，容量查询后不会有并发写者抢占空间。
     * IDF 6 的 free-size API 已保守扣除 NOSPLIT item/descriptor 开销；帧长最多 73 字节，
     * 因而下面的 uart_write_bytes 不会等待 TX ring 空间。控制循环不等待 TX idle。
     */
    const int written = uart_write_bytes(UART0_OUTPUT_PORT, data, length);
    return written == (int)length ? ESP_OK : ESP_FAIL;
}

esp_err_t dual_uart0_output_wait_tx_done_locked(uint32_t timeout_ms)
{
    if (xPortInIsrContext() ||
        __atomic_load_n(&s_output_owner, __ATOMIC_ACQUIRE) !=
            xTaskGetCurrentTaskHandle()) {
        return ESP_ERR_INVALID_STATE;
    }
    return uart_wait_tx_done(UART0_OUTPUT_PORT, pdMS_TO_TICKS(timeout_ms));
}

static esp_err_t write_bytes(const void *data, size_t length, bool makcu_ascii)
{
    if ((data == NULL && length != 0U) || length > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length == 0U) {
        return ESP_OK;
    }
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API || DUAL_PROXY_ENABLE_MAKCU_V4_API
    if (__atomic_load_n(&s_makcu_ascii_mode, __ATOMIC_ACQUIRE) != makcu_ascii) {
        return ESP_ERR_INVALID_STATE;
    }
#else
    if (makcu_ascii) {
        return ESP_ERR_INVALID_STATE;
    }
#endif

    esp_err_t result = dual_uart0_output_lock();
    if (result != ESP_OK) {
        return result;
    }
#if DUAL_PROXY_ENABLE_MAKCU_ASCII_API || DUAL_PROXY_ENABLE_MAKCU_V4_API
    if (__atomic_load_n(&s_makcu_ascii_mode, __ATOMIC_ACQUIRE) != makcu_ascii) {
        dual_uart0_output_unlock();
        return ESP_ERR_INVALID_STATE;
    }
#endif

    const int written = uart_write_bytes(UART0_OUTPUT_PORT, data, length);
    const esp_err_t idle_result = dual_uart0_output_wait_tx_idle();
    dual_uart0_output_unlock();

    if (written != (int)length) {
        return ESP_FAIL;
    }
    return idle_result;
}

esp_err_t dual_uart0_output_write(const void *data, size_t length)
{
    return write_bytes(data, length, false);
}

esp_err_t dual_uart0_output_write_makcu_ascii(const void *data, size_t length)
{
    return write_bytes(data, length, true);
}

esp_err_t dual_uart0_output_write_makcu(const void *data, size_t length)
{
    return write_bytes(data, length, true);
}
