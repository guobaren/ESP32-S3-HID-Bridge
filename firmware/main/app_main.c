#include <stddef.h>
#include <stdint.h>

#include "bridge_protocol.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_output.h"

#define BRIDGE_UART UART_NUM_0
#define BRIDGE_UART_BAUD_RATE 921600
#define UART_RX_BUFFER_SIZE 4096
#define UART_READ_CHUNK_SIZE 256

static const char *TAG = "hid_bridge";

static void on_bridge_frame(const bridge_frame_t *frame, void *context)
{
    (void)context;

    if (frame->type == BRIDGE_MESSAGE_PING) {
        return;
    }

    esp_err_t error = hid_output_submit(frame);
    if (error != ESP_OK && error != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "丢弃 HID 帧，错误：%s", esp_err_to_name(error));
    }
}

static void uart_receiver_task(void *argument)
{
    (void)argument;
    bridge_parser_t parser;
    uint8_t buffer[UART_READ_CHUNK_SIZE];
    bridge_parser_init(&parser, on_bridge_frame, NULL);

    while (true) {
        const int received = uart_read_bytes(
            BRIDGE_UART,
            buffer,
            sizeof(buffer),
            pdMS_TO_TICKS(100));
        if (received > 0) {
            bridge_parser_feed(&parser, buffer, (size_t)received);
        }
    }
}

static esp_err_t configure_uart(void)
{
    const uart_config_t config = {
        .baud_rate = BRIDGE_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(BRIDGE_UART, &config), TAG, "UART 参数失败");
    ESP_RETURN_ON_ERROR(
        uart_set_pin(
            BRIDGE_UART,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE),
        TAG,
        "UART 引脚失败");
    ESP_RETURN_ON_ERROR(
        uart_driver_install(
            BRIDGE_UART,
            UART_RX_BUFFER_SIZE,
            0,
            0,
            NULL,
            0),
        TAG,
        "UART 驱动失败");
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(hid_output_init());
    ESP_ERROR_CHECK(configure_uart());

    BaseType_t created = xTaskCreate(
        uart_receiver_task,
        "uart_receiver",
        4096,
        NULL,
        9,
        NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "ESP32-S3 HID Bridge 已启动");
}
