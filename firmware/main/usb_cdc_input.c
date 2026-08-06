#include "usb_cdc_input.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bridge_protocol.h"
#include "device_discovery.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_session.h"
#include "tinyusb_cdc_acm.h"

#define CDC_RX_CHUNK_SIZE 512
#define CDC_RX_QUEUE_LENGTH 8

static const char *TAG = "usb_cdc_input";
static QueueHandle_t s_rx_queue;
static volatile uint32_t s_dropped_chunks;

typedef struct {
    size_t length;
    uint8_t data[CDC_RX_CHUNK_SIZE];
} cdc_rx_chunk_t;

static void send_device_hello(const bridge_frame_t *probe)
{
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

    size_t queued = tinyusb_cdcacm_write_queue(
        TINYUSB_CDC_ACM_0,
        serialized,
        serialized_length);
    if (queued != serialized_length) {
        ESP_LOGW(TAG, "设备发现响应入队不完整：%u/%u", (unsigned)queued, (unsigned)serialized_length);
        return;
    }

    result = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "设备发现响应发送失败：%s", esp_err_to_name(result));
    }
}

static void on_bridge_frame(const bridge_frame_t *frame, void *context)
{
    (void)context;
    if (frame->type == BRIDGE_MESSAGE_DEVICE_PROBE) {
        send_device_hello(frame);
        return;
    }
    input_session_handle(BRIDGE_INPUT_USB_CDC, frame);
}

static void cdc_receiver_task(void *argument)
{
    (void)argument;
    bridge_parser_t parser;
    bridge_parser_init(&parser, on_bridge_frame, NULL);

    cdc_rx_chunk_t chunk;
    uint32_t reported_drops = 0;
    while (true) {
        if (xQueueReceive(s_rx_queue, &chunk, portMAX_DELAY) == pdTRUE) {
            if (s_dropped_chunks != reported_drops) {
                reported_drops = s_dropped_chunks;
                ESP_LOGW(TAG, "CDC 接收队列溢出，累计丢弃=%" PRIu32, reported_drops);
                bridge_parser_init(&parser, on_bridge_frame, NULL);
                input_session_disconnected(BRIDGE_INPUT_USB_CDC);
            }
            bridge_parser_feed(&parser, chunk.data, chunk.length);
        }
    }
}

static void cdc_rx_callback(int interface, cdcacm_event_t *event)
{
    (void)event;
    if (interface != TINYUSB_CDC_ACM_0 || s_rx_queue == NULL) {
        return;
    }

    cdc_rx_chunk_t chunk = {0};
    esp_err_t result = tinyusb_cdcacm_read(
        TINYUSB_CDC_ACM_0,
        chunk.data,
        sizeof(chunk.data),
        &chunk.length);
    if (result != ESP_OK || chunk.length == 0) {
        return;
    }
    if (xQueueSend(s_rx_queue, &chunk, 0) != pdTRUE) {
        s_dropped_chunks++;
    }
}

esp_err_t usb_cdc_input_init(void)
{
    if (s_rx_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_rx_queue = xQueueCreate(CDC_RX_QUEUE_LENGTH, sizeof(cdc_rx_chunk_t));
    if (s_rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    tinyusb_config_cdcacm_t config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    esp_err_t result = tinyusb_cdcacm_init(&config);
    if (result != ESP_OK) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        return result;
    }

    BaseType_t created = xTaskCreate(
        cdc_receiver_task,
        "usb_cdc_input",
        4096,
        NULL,
        9,
        NULL);
    if (created != pdPASS) {
        tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "原生 USB CDC 输入已启动");
    return ESP_OK;
}

void usb_cdc_input_on_detached(void)
{
    input_session_disconnected(BRIDGE_INPUT_USB_CDC);
}
