#include "diag_stream.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bridge_protocol.h"
#include "uart0_output.h"

#define DIAG_QUEUE_LENGTH 128U
#define DIAG_TX_TASK_STACK 3072U
#define DIAG_TX_TASK_PRIORITY 2U
#define DIAG_EVENT_HEADER_LENGTH 12U
#define DIAG_EVENT_FRAGMENT_DATA_MAX \
    (DUAL_PROXY_MAX_PAYLOAD - DIAG_EVENT_HEADER_LENGTH)

static const char *TAG = "dual_diag_stream";

typedef struct {
    uint32_t event_id;
    uint32_t timestamp_us_low32;
    uint8_t source;
    uint8_t kind;
    uint8_t length;
    uint32_t subscription_generation;
    uint8_t data[DUAL_DIAG_EVENT_DATA_MAX];
} diag_event_t;

static QueueHandle_t s_event_queue;
static TaskHandle_t s_tx_task;
static volatile bool s_enabled;
static volatile uint32_t s_subscription_generation;
static volatile uint32_t s_next_event_id;
static volatile uint16_t s_next_frame_sequence;
static volatile uint32_t s_captured;
static volatile uint32_t s_dropped;
static volatile uint32_t s_queue_received;
static volatile uint32_t s_queue_rejected;
static volatile uint32_t s_queue_dropped;
static volatile uint32_t s_queue_peak;
static volatile int64_t s_last_queue_stats_us;

static void queue_metric_observe_depth(void)
{
    if (s_event_queue == NULL) {
        return;
    }
    uint32_t peak = __atomic_load_n(&s_queue_peak, __ATOMIC_RELAXED);
    const uint32_t depth = (uint32_t)uxQueueMessagesWaiting(s_event_queue);
    while (depth > peak && !__atomic_compare_exchange_n(
               &s_queue_peak, &peak, depth, true,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static bool write_frame(uint8_t type, uint16_t sequence,
                        const uint8_t *payload, uint8_t payload_length)
{
    dual_frame_t frame = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = type,
        .sequence = sequence,
        .payload_length = payload_length,
    };
    if (payload_length != 0U) {
        memcpy(frame.payload, payload, payload_length);
    }
    uint8_t wire[9U + DUAL_PROXY_MAX_PAYLOAD];
    size_t wire_length = 0U;
    if (dual_frame_serialize(&frame, wire, sizeof(wire), &wire_length) != ESP_OK) {
        return false;
    }
    return dual_uart0_output_write(wire, wire_length) == ESP_OK;
}

static void send_event(const diag_event_t *event)
{
    uint8_t offset = 0U;
    do {
        uint8_t chunk_length = (uint8_t)(event->length - offset);
        if (chunk_length > DIAG_EVENT_FRAGMENT_DATA_MAX) {
            chunk_length = DIAG_EVENT_FRAGMENT_DATA_MAX;
        }
        uint8_t payload[DUAL_PROXY_MAX_PAYLOAD] = {0};
        memcpy(&payload[0], &event->event_id, sizeof(event->event_id));
        memcpy(&payload[4], &event->timestamp_us_low32,
               sizeof(event->timestamp_us_low32));
        payload[8] = event->source;
        payload[9] = event->kind;
        payload[10] = event->length;
        payload[11] = offset;
        if (chunk_length != 0U) {
            memcpy(&payload[DIAG_EVENT_HEADER_LENGTH], &event->data[offset],
                   chunk_length);
        }
        const uint16_t sequence = __atomic_fetch_add(
            &s_next_frame_sequence, 1U, __ATOMIC_RELAXED);
        if (!write_frame(DUAL_MESSAGE_DIAG_STREAM_EVENT, sequence, payload,
                         (uint8_t)(DIAG_EVENT_HEADER_LENGTH + chunk_length))) {
            __atomic_fetch_add(&s_dropped, 1U, __ATOMIC_RELAXED);
            return;
        }
        offset = (uint8_t)(offset + chunk_length);
    } while (offset < event->length);
    __atomic_fetch_add(&s_captured, 1U, __ATOMIC_RELAXED);
}

static void diag_tx_task(void *argument)
{
    (void)argument;
    diag_event_t event;
    while (true) {
        if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            if (!__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE) ||
                event.subscription_generation != __atomic_load_n(
                    &s_subscription_generation, __ATOMIC_ACQUIRE)) {
                __atomic_fetch_add(&s_dropped, 1U, __ATOMIC_RELAXED);
                continue;
            }
            send_event(&event);
        }
    }
}

esp_err_t dual_diag_stream_start(uart_port_t uart_port)
{
    if (uart_port != UART_NUM_0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_event_queue != NULL) {
        return ESP_OK;
    }
    s_captured = 0U;
    s_dropped = 0U;
    s_queue_received = 0U;
    s_queue_rejected = 0U;
    s_queue_dropped = 0U;
    s_queue_peak = 0U;
    s_last_queue_stats_us = 0;
    s_event_queue = xQueueCreate(DIAG_QUEUE_LENGTH, sizeof(diag_event_t));
    if (s_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(diag_tx_task, "diag_stream_tx", DIAG_TX_TASK_STACK,
                    NULL, DIAG_TX_TASK_PRIORITY, &s_tx_task) != pdPASS) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dual_diag_stream_set_enabled(bool enabled)
{
    const bool current = __atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE);
    if (current != enabled) {
        __atomic_add_fetch(&s_subscription_generation, 1U, __ATOMIC_ACQ_REL);
        __atomic_store_n(&s_enabled, enabled, __ATOMIC_RELEASE);
        if (!enabled && s_event_queue != NULL) {
            const uint32_t discarded =
                (uint32_t)uxQueueMessagesWaiting(s_event_queue);
            if (discarded != 0U) {
                __atomic_fetch_add(&s_queue_dropped, discarded, __ATOMIC_RELAXED);
                __atomic_fetch_add(&s_dropped, discarded, __ATOMIC_RELAXED);
                xQueueReset(s_event_queue);
            }
        }
    }
}

void dual_diag_stream_record(uint8_t source, uint8_t kind,
                             const uint8_t *data, uint8_t length)
{
    const uint32_t generation = __atomic_load_n(
        &s_subscription_generation, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_fetch_add(&s_queue_received, 1U, __ATOMIC_RELAXED);
    if (s_event_queue == NULL || length > DUAL_DIAG_EVENT_DATA_MAX ||
        (data == NULL && length != 0U)) {
        __atomic_fetch_add(&s_queue_rejected, 1U, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_dropped, 1U, __ATOMIC_RELAXED);
        return;
    }
    diag_event_t event = {
        .event_id = __atomic_add_fetch(&s_next_event_id, 1U, __ATOMIC_RELAXED),
        .timestamp_us_low32 = (uint32_t)esp_timer_get_time(),
        .source = source,
        .kind = kind,
        .length = length,
        .subscription_generation = generation,
    };
    if (length != 0U) {
        memcpy(event.data, data, length);
    }
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        __atomic_fetch_add(&s_queue_dropped, 1U, __ATOMIC_RELAXED);
        queue_metric_observe_depth();
        __atomic_fetch_add(&s_dropped, 1U, __ATOMIC_RELAXED);
    } else {
        queue_metric_observe_depth();
    }
}

void dual_diag_stream_send_status(uint16_t sequence)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t previous = s_last_queue_stats_us;
    if (previous == 0 || now_us - previous >= 1000000LL) {
        s_last_queue_stats_us = now_us;
        ESP_LOGI(TAG, "QUEUE name=diag_event received=%" PRIu32
                 " rejected=%" PRIu32 " dropped=%" PRIu32 " peak=%" PRIu32,
                 __atomic_load_n(&s_queue_received, __ATOMIC_RELAXED),
                 __atomic_load_n(&s_queue_rejected, __ATOMIC_RELAXED),
                 __atomic_load_n(&s_queue_dropped, __ATOMIC_RELAXED),
                 __atomic_load_n(&s_queue_peak, __ATOMIC_RELAXED));
    }
    uint8_t payload[8];
    const uint32_t captured = __atomic_load_n(&s_captured, __ATOMIC_RELAXED);
    const uint32_t dropped = __atomic_load_n(&s_dropped, __ATOMIC_RELAXED);
    memcpy(&payload[0], &captured, sizeof(captured));
    memcpy(&payload[4], &dropped, sizeof(dropped));
    (void)write_frame(DUAL_MESSAGE_DIAG_STREAM_STATUS, sequence, payload,
                      sizeof(payload));
}
