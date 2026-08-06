#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bridge_protocol.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "device_discovery.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_session.h"
#include "nvs_flash.h"
#include "output_router.h"
#include "runtime_features.h"
#include "status_led.h"
#include "wifi_input.h"
#include "wifi_manager.h"

#define BRIDGE_UART UART_NUM_0
#define BRIDGE_UART_BAUD_RATE 921600
#define UART_RX_BUFFER_SIZE 4096
#define UART_READ_CHUNK_SIZE 256
static const char *TAG = "hid_bridge";

static void send_device_hello(const bridge_frame_t *probe)
{
    if (probe->payload_length != DEVICE_PROBE_NONCE_LENGTH) {
        return;
    }

    uint8_t serialized[9 + BRIDGE_MAX_PAYLOAD];
    size_t serialized_length = 0;
    esp_err_t result = device_discovery_serialize_hello(
        probe,
        serialized,
        sizeof(serialized),
        &serialized_length);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "设备发现响应编码失败：%s", esp_err_to_name(result));
        return;
    }

    int written = uart_write_bytes(BRIDGE_UART, serialized, serialized_length);
    if (written != (int)serialized_length) {
        ESP_LOGW(TAG, "设备发现响应发送不完整：%d/%u", written, (unsigned)serialized_length);
    }
}

static void on_bridge_frame(const bridge_frame_t *frame, void *context)
{
    (void)context;
    if (frame->type == BRIDGE_MESSAGE_DEVICE_PROBE) {
        send_device_hello(frame);
        return;
    }
    input_session_handle(BRIDGE_INPUT_UART, frame);
}

static void uart_receiver_task(void *argument)
{
    (void)argument;
    bridge_parser_t parser;
    uint8_t buffer[UART_READ_CHUNK_SIZE];
    bridge_parser_init(&parser, on_bridge_frame, NULL);

    while (true) {
        int received = uart_read_bytes(
            BRIDGE_UART,
            buffer,
            1,
            portMAX_DELAY);
        if (received > 0) {
            size_t buffered = 0;
            if (uart_get_buffered_data_len(BRIDGE_UART, &buffered) == ESP_OK && buffered > 0) {
                size_t remaining = sizeof(buffer) - (size_t)received;
                size_t drain_length = buffered < remaining ? buffered : remaining;
                int drained = uart_read_bytes(
                    BRIDGE_UART,
                    buffer + received,
                    drain_length,
                    0);
                if (drained > 0) {
                    received += drained;
                }
            }
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

#if HID_BRIDGE_WIFI_RUNTIME_ENABLED && CONFIG_HID_BRIDGE_WIFI_ENABLE && \
    CONFIG_HID_BRIDGE_PROVISIONING_ENABLE
static void provisioning_button_task(void *argument)
{
    (void)argument;
    const gpio_num_t button_gpio = (gpio_num_t)CONFIG_HID_BRIDGE_PROVISIONING_BUTTON_GPIO;
    const TickType_t hold_time =
        pdMS_TO_TICKS(CONFIG_HID_BRIDGE_PROVISIONING_BUTTON_HOLD_SECONDS * 1000);
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << button_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    TickType_t pressed_since = 0;
    bool triggered = false;
    while (true) {
        bool pressed = gpio_get_level(button_gpio) == 0;
        if (!pressed) {
            pressed_since = 0;
            triggered = false;
        } else if (pressed_since == 0) {
            pressed_since = xTaskGetTickCount();
        } else if (!triggered && xTaskGetTickCount() - pressed_since >= hold_time) {
            triggered = true;
            input_session_release_all();
            esp_err_t result = wifi_manager_start_provisioning();
            if (result == ESP_OK) {
                ESP_LOGW(TAG, "检测到长按 BOOT，已进入网页配网模式");
            } else {
                ESP_LOGE(TAG, "进入网页配网模式失败：%s", esp_err_to_name(result));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }

#if HID_BRIDGE_WIFI_RUNTIME_ENABLED
    esp_err_t wifi_result = wifi_manager_init();
    if (wifi_result != ESP_OK && wifi_result != ESP_ERR_NOT_SUPPORTED) {
        ESP_ERROR_CHECK(wifi_result);
    }
#endif
    ESP_ERROR_CHECK(status_led_init());
    ESP_ERROR_CHECK(output_router_init());
    ESP_ERROR_CHECK(input_session_init());
    ESP_ERROR_CHECK(configure_uart());
#if HID_BRIDGE_WIFI_RUNTIME_ENABLED
    wifi_result = wifi_input_start();
    if (wifi_result != ESP_OK && wifi_result != ESP_ERR_NOT_SUPPORTED &&
        wifi_result != ESP_ERR_INVALID_ARG) {
        ESP_ERROR_CHECK(wifi_result);
    }
#endif

    BaseType_t created = xTaskCreate(
        uart_receiver_task,
        "uart_receiver",
        4096,
        NULL,
        9,
        NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

#if HID_BRIDGE_WIFI_RUNTIME_ENABLED && CONFIG_HID_BRIDGE_WIFI_ENABLE && \
    CONFIG_HID_BRIDGE_PROVISIONING_ENABLE
    created = xTaskCreate(
        provisioning_button_task,
        "provision_button",
        3072,
        NULL,
        5,
        NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
#endif

    ESP_LOGI(TAG, "运行模式：USB-to-UART/原生 USB CDC 输入，USB/BLE HID 输出；Wi-Fi 输入/输出代码已保留但暂不启用");
    ESP_LOGI(TAG, "ESP32-S3 HID Bridge 已启动");
}
