#include "uart0_output.h"

#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define UART0_OUTPUT_PORT UART_NUM_0

static StaticSemaphore_t s_output_mutex_storage;
static SemaphoreHandle_t s_output_mutex;
static TaskHandle_t s_output_owner;
static volatile bool s_driver_ready;

esp_err_t dual_uart0_output_init(void)
{
    /* app_main 在安装日志钩子或创建 UART 控制任务前调用。 */
    if (s_output_mutex == NULL) {
        s_output_mutex = xSemaphoreCreateMutexStatic(&s_output_mutex_storage);
    }
    return s_output_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void dual_uart0_output_set_driver_ready(bool ready)
{
    __atomic_store_n(&s_driver_ready, ready, __ATOMIC_RELEASE);
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

esp_err_t dual_uart0_output_write(const void *data, size_t length)
{
    if ((data == NULL && length != 0U) || length > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length == 0U) {
        return ESP_OK;
    }

    esp_err_t result = dual_uart0_output_lock();
    if (result != ESP_OK) {
        return result;
    }

    const int written = uart_write_bytes(UART0_OUTPUT_PORT, data, length);
    const esp_err_t idle_result = dual_uart0_output_wait_tx_idle();
    dual_uart0_output_unlock();

    if (written != (int)length) {
        return ESP_FAIL;
    }
    return idle_result;
}
