#include "uart1_link.h"

#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "dual_proxy_runtime_config.h"

#define LINK_UART UART_NUM_1
#define LINK_TX_GPIO 17
#define LINK_RX_GPIO 18
#define LINK_BAUD 921600
#define LINK_RX_BUFFER_SIZE 4096
#define LINK_TX_BUFFER_SIZE 4096
#define LINK_EVENT_QUEUE_LENGTH 32
#define LINK_TX_QUEUE_LENGTH 64
#define LINK_SAFETY_QUEUE_LENGTH 8
#define LINK_RX_CHUNK_SIZE 128
#define LINK_TASK_PRIORITY 6
#define LINK_PERIOD_MS 250
#define LINK_TIMEOUT_MS 750
#define LINK_HELLO_LENGTH 10
#define LINK_PING_LENGTH 2
#define LINK_STATS_PERIOD_US 5000000LL

_Static_assert(CONFIG_FREERTOS_HZ == DUAL_PROXY_REQUIRED_FREERTOS_HZ,
               "dual_proxy要求CONFIG_FREERTOS_HZ=1000");
_Static_assert(pdMS_TO_TICKS(1) == 1, "1ms必须正好折算为1 tick");

static const char *TAG = "dual_uart1";

typedef struct {
    uint8_t type;
    uint8_t length;
    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD];
} tx_item_t;

static uint8_t s_role;
static uint8_t s_usb_state;
static uint8_t s_node_id[6];
static uint16_t s_generation;
static uint16_t s_next_sequence;
static uint16_t s_peer_last_sequence;
static bool s_peer_sequence_initialized;
static uint16_t s_peer_generation;
static bool s_peer_generation_initialized;
static volatile bool s_peer_online;
static int64_t s_last_peer_rx_us;
static uint32_t s_tx_count;
static uint32_t s_rx_count;
static uint32_t s_crc_or_frame_errors;
static uint32_t s_rejected_peers;
static volatile uint32_t s_tx_queue_current;
static volatile uint32_t s_tx_queue_peak;
static volatile uint32_t s_tx_queue_overflows;
static volatile uint32_t s_tx_queue_drops;
static volatile uint32_t s_tx_write_failures;
static QueueHandle_t s_uart_event_queue;
static QueueHandle_t s_tx_queue;
static QueueHandle_t s_safety_tx_queue;
static TaskHandle_t s_rx_task;
static TaskHandle_t s_tx_task;
static dual_link_frame_callback_t s_frame_callback;
static dual_link_fault_callback_t s_fault_callback;
static volatile bool s_fault_in_progress;

static void write_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static uint16_t read_u16_le(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static void update_tx_queue_metrics(void)
{
    UBaseType_t current = 0;
    if (s_tx_queue != NULL) {
        current += uxQueueMessagesWaiting(s_tx_queue);
    }
    if (s_safety_tx_queue != NULL) {
        current += uxQueueMessagesWaiting(s_safety_tx_queue);
    }
    s_tx_queue_current = (uint32_t)current;
    if (s_tx_queue_current > s_tx_queue_peak) {
        s_tx_queue_peak = s_tx_queue_current;
    }
}

static void reset_tx_queues(void)
{
    if (s_tx_queue != NULL) {
        xQueueReset(s_tx_queue);
    }
    if (s_safety_tx_queue != NULL) {
        xQueueReset(s_safety_tx_queue);
    }
    update_tx_queue_metrics();
}

static void notify_tx_task(void)
{
    if (s_tx_task != NULL) {
        xTaskNotifyGive(s_tx_task);
    }
}

static void queue_fault(void)
{
    /* Drop stale motion, then give the callback one chance to enqueue release. */
    if (s_fault_in_progress) {
        return;
    }
    s_fault_in_progress = true;
    reset_tx_queues();
    if (s_fault_callback != NULL) {
        s_fault_callback();
    }
    s_fault_in_progress = false;
}

static bool peer_sequence_is_new(uint16_t sequence)
{
    if (!s_peer_sequence_initialized) {
        s_peer_last_sequence = sequence;
        s_peer_sequence_initialized = true;
        return true;
    }
    const uint16_t distance = (uint16_t)(sequence - s_peer_last_sequence);
    if (distance == 0 || distance >= 0x8000U) {
        return false;
    }
    s_peer_last_sequence = sequence;
    return true;
}

static bool accept_peer_frame(const dual_frame_t *frame)
{
    if (frame->type == DUAL_MESSAGE_LINK_HELLO) {
        if (frame->payload_length != LINK_HELLO_LENGTH) {
            ++s_crc_or_frame_errors;
            return false;
        }
        const uint8_t peer_role = frame->payload[0];
        if (peer_role != DUAL_ROLE_PC_DEVICE && peer_role != DUAL_ROLE_MOUSE_HOST) {
            ++s_rejected_peers;
            return false;
        }
        if (peer_role == s_role || memcmp(&frame->payload[2], s_node_id, sizeof(s_node_id)) == 0) {
            ++s_rejected_peers;
            ESP_LOGW(TAG, "拒绝UART1对端：相同角色或相同node ID");
            return false;
        }
        const uint16_t peer_generation = read_u16_le(&frame->payload[8]);
        const bool new_generation = !s_peer_online || !s_peer_generation_initialized ||
            peer_generation != s_peer_generation;
        if (new_generation) {
            s_peer_sequence_initialized = false;
            s_peer_generation = peer_generation;
            s_peer_generation_initialized = true;
        }
        s_peer_online = true;
        s_last_peer_rx_us = esp_timer_get_time();
        return true;
    }
    if (!s_peer_online || !peer_sequence_is_new(frame->sequence)) {
        return false;
    }
    if (frame->type == DUAL_MESSAGE_LINK_PING &&
        (frame->payload_length != LINK_PING_LENGTH ||
         !s_peer_generation_initialized ||
         read_u16_le(frame->payload) != s_peer_generation)) {
        ++s_rejected_peers;
        return false;
    }
    s_last_peer_rx_us = esp_timer_get_time();
    return true;
}

static void on_link_frame(const dual_frame_t *frame, void *context)
{
    (void)context;
    if (!accept_peer_frame(frame)) {
        return;
    }
    ++s_rx_count;
    if (s_frame_callback != NULL) {
        s_frame_callback(frame);
    }
}

static esp_err_t enqueue_item(uint8_t type, const uint8_t *payload, uint8_t length)
{
    if (s_tx_queue == NULL || s_safety_tx_queue == NULL || length > DUAL_PROXY_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_STATE;
    }
    tx_item_t item = {.type = type, .length = length};
    if (length > 0 && payload != NULL) {
        memcpy(item.payload, payload, length);
    }

    const bool safety = type == DUAL_MESSAGE_PHYSICAL_RELEASE;
    QueueHandle_t target = safety ? s_safety_tx_queue : s_tx_queue;
    if (xQueueSend(target, &item, 0) != pdTRUE) {
        ++s_tx_queue_overflows;
        if (safety) {
            /* Preserve the newest release even if stale releases accumulated. */
            const UBaseType_t discarded = uxQueueMessagesWaiting(s_tx_queue) +
                uxQueueMessagesWaiting(s_safety_tx_queue);
            reset_tx_queues();
            s_tx_queue_drops += (uint32_t)discarded;
            if (xQueueSend(s_safety_tx_queue, &item, 0) != pdTRUE) {
                ++s_tx_queue_drops;
            }
        } else {
            ++s_tx_queue_drops;
            queue_fault();
        }
        update_tx_queue_metrics();
        notify_tx_task();
        return ESP_ERR_TIMEOUT;
    }
    update_tx_queue_metrics();
    notify_tx_task();
    return ESP_OK;
}

static void send_status_frame(uint8_t type, const uint8_t *payload, uint8_t length)
{
    dual_frame_t frame = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = type,
        .sequence = s_next_sequence++,
        .payload_length = length,
    };
    if (length > 0) {
        memcpy(frame.payload, payload, length);
    }
    uint8_t serialized[9 + DUAL_PROXY_MAX_PAYLOAD];
    size_t serialized_length = 0;
    if (dual_frame_serialize(&frame, serialized, sizeof(serialized), &serialized_length) != ESP_OK) {
        return;
    }
    if (uart_write_bytes(LINK_UART, serialized, serialized_length) == (int)serialized_length) {
        ++s_tx_count;
    } else {
        ++s_tx_write_failures;
    }
}

static void send_link_status(void)
{
    uint8_t hello[LINK_HELLO_LENGTH] = {0};
    hello[0] = s_role;
    hello[1] = s_usb_state;
    memcpy(&hello[2], s_node_id, sizeof(s_node_id));
    write_u16_le(&hello[8], s_generation);
    send_status_frame(DUAL_MESSAGE_LINK_HELLO, hello, sizeof(hello));

    uint8_t ping[LINK_PING_LENGTH] = {0};
    write_u16_le(ping, s_generation);
    send_status_frame(DUAL_MESSAGE_LINK_PING, ping, sizeof(ping));
}

static bool tx_queues_have_items(void)
{
    return (s_safety_tx_queue != NULL && uxQueueMessagesWaiting(s_safety_tx_queue) > 0) ||
        (s_tx_queue != NULL && uxQueueMessagesWaiting(s_tx_queue) > 0);
}

static void link_tx_task(void *argument)
{
    (void)argument;
    int64_t last_status_us = 0;
    int64_t last_summary_us = 0;
    while (true) {
        /*
         * Only an empty pair of queues may enter the heartbeat wait.  If a
         * producer races this check, its notification is retained by the
         * counting task notification and the take returns immediately.
         */
        if (dual_proxy_link_should_wait_for_notification(tx_queues_have_items())) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LINK_PERIOD_MS));
        }
        const int64_t now_us = esp_timer_get_time();
        unsigned processed = 0;
        tx_item_t item;

        while (processed < DUAL_PROXY_LINK_TX_BATCH_LIMIT &&
               xQueueReceive(s_safety_tx_queue, &item, 0) == pdTRUE) {
            send_status_frame(item.type, item.payload, item.length);
            ++processed;
        }

        if (last_status_us == 0 || now_us - last_status_us >= LINK_PERIOD_MS * 1000LL) {
            send_link_status();
            last_status_us = now_us;
        }

        while (processed < DUAL_PROXY_LINK_TX_BATCH_LIMIT &&
               xQueueReceive(s_tx_queue, &item, 0) == pdTRUE) {
            send_status_frame(item.type, item.payload, item.length);
            ++processed;
        }

        if (s_peer_online && now_us - s_last_peer_rx_us > LINK_TIMEOUT_MS * 1000LL) {
            s_peer_online = false;
            s_peer_sequence_initialized = false;
            s_peer_generation_initialized = false;
            ESP_LOGW(TAG, "UART1对端超时，清理实体输入");
            queue_fault();
        }

        update_tx_queue_metrics();
        if (last_summary_us == 0 || now_us - last_summary_us >= LINK_STATS_PERIOD_US) {
            ESP_LOGI(TAG, "UART1统计 tx=%" PRIu32 " rx=%" PRIu32
                     " reject=%" PRIu32 " q=%" PRIu32 " peak=%" PRIu32
                     " overflow=%" PRIu32 " drop=%" PRIu32 " write_fail=%" PRIu32
                     " peer=%s",
                     s_tx_count, s_rx_count, s_rejected_peers, s_tx_queue_current,
                     s_tx_queue_peak, s_tx_queue_overflows, s_tx_queue_drops,
                     s_tx_write_failures, s_peer_online ? "online" : "offline");
            last_summary_us = now_us;
        }

        /* A continuously populated queue gets a 1 ms budget break at 1 kHz. */
        if (tx_queues_have_items()) {
            vTaskDelay(1);
            /* Do not re-enter the 250 ms heartbeat wait while backlog remains. */
            continue;
        }
    }
}

static void link_rx_task(void *argument)
{
    (void)argument;
    dual_parser_t parser;
    dual_parser_init(&parser, on_link_frame, NULL);
    uart_event_t event;
    while (true) {
        if (xQueueReceive(s_uart_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.type == UART_DATA) {
            size_t remaining = event.size;
            while (remaining > 0) {
                uint8_t receive_buffer[LINK_RX_CHUNK_SIZE];
                const size_t requested = remaining < sizeof(receive_buffer) ?
                    remaining : sizeof(receive_buffer);
                const int received = uart_read_bytes(LINK_UART, receive_buffer, requested, 0);
                if (received <= 0) {
                    ++s_crc_or_frame_errors;
                    break;
                }
                dual_parser_feed(&parser, receive_buffer, (size_t)received);
                remaining -= (size_t)received;
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            ++s_crc_or_frame_errors;
            ESP_LOGW(TAG, "UART1接收缓冲溢出，清理输入和实体状态");
            uart_flush_input(LINK_UART);
            xQueueReset(s_uart_event_queue);
            queue_fault();
        }

        /* Avoid spinning if a burst left more hardware events queued. */
        if (uxQueueMessagesWaiting(s_uart_event_queue) > 0) {
            vTaskDelay(1);
        }
    }
}

esp_err_t dual_uart1_start(
    uint8_t role,
    const uint8_t node_id[6],
    dual_link_frame_callback_t frame_callback,
    dual_link_fault_callback_t fault_callback)
{
    if ((role != DUAL_ROLE_PC_DEVICE && role != DUAL_ROLE_MOUSE_HOST) || node_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_role = role;
    s_usb_state = DUAL_USB_STATE_WAITING;
    memcpy(s_node_id, node_id, sizeof(s_node_id));
    s_generation = (uint16_t)(esp_timer_get_time() ^ ((uint16_t)node_id[4] << 8) ^ node_id[5]);
    s_next_sequence = 0;
    s_peer_sequence_initialized = false;
    s_peer_generation_initialized = false;
    s_peer_online = false;
    s_last_peer_rx_us = 0;
    s_tx_count = 0;
    s_rx_count = 0;
    s_crc_or_frame_errors = 0;
    s_rejected_peers = 0;
    s_tx_queue_current = 0;
    s_tx_queue_peak = 0;
    s_tx_queue_overflows = 0;
    s_tx_queue_drops = 0;
    s_tx_write_failures = 0;
    s_frame_callback = frame_callback;
    s_fault_callback = fault_callback;
    s_fault_in_progress = false;

    const uart_config_t config = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t result = uart_param_config(LINK_UART, &config);
    if (result != ESP_OK) {
        return result;
    }
    result = uart_set_pin(LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (result != ESP_OK) {
        return result;
    }
    result = uart_driver_install(LINK_UART, LINK_RX_BUFFER_SIZE, LINK_TX_BUFFER_SIZE,
                                 LINK_EVENT_QUEUE_LENGTH, &s_uart_event_queue, 0);
    if (result != ESP_OK) {
        return result;
    }
    s_tx_queue = xQueueCreate(LINK_TX_QUEUE_LENGTH, sizeof(tx_item_t));
    s_safety_tx_queue = xQueueCreate(LINK_SAFETY_QUEUE_LENGTH, sizeof(tx_item_t));
    if (s_tx_queue == NULL || s_safety_tx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(link_rx_task, "dual_uart1_rx", 4096, NULL, LINK_TASK_PRIORITY, &s_rx_task) != pdPASS ||
        xTaskCreate(link_tx_task, "dual_uart1_tx", 4096, NULL, LINK_TASK_PRIORITY, &s_tx_task) != pdPASS) {
        if (s_rx_task != NULL) {
            vTaskDelete(s_rx_task);
            s_rx_task = NULL;
        }
        if (s_tx_task != NULL) {
            vTaskDelete(s_tx_task);
            s_tx_task = NULL;
        }
        vQueueDelete(s_tx_queue);
        vQueueDelete(s_safety_tx_queue);
        s_tx_queue = NULL;
        s_safety_tx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dual_uart1_set_usb_state(uint8_t usb_state)
{
    s_usb_state = usb_state;
    notify_tx_task();
}

bool dual_uart1_peer_online(void)
{
    return s_peer_online;
}

uint16_t dual_uart1_generation(void)
{
    return s_generation;
}

esp_err_t dual_uart1_send_mouse(
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan)
{
    uint8_t payload[9] = {
        interface_number,
        report_id,
        (uint8_t)(buttons & 0x1FU),
        (uint8_t)x,
        (uint8_t)((uint16_t)x >> 8),
        (uint8_t)y,
        (uint8_t)((uint16_t)y >> 8),
        (uint8_t)wheel,
        (uint8_t)pan,
    };
    return enqueue_item(DUAL_MESSAGE_PHYSICAL_MOUSE, payload, sizeof(payload));
}

esp_err_t dual_uart1_send_release(uint8_t reason)
{
    return enqueue_item(DUAL_MESSAGE_PHYSICAL_RELEASE, &reason, 1);
}
