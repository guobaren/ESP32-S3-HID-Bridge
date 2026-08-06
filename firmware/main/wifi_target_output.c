#include "wifi_target_output.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "secure_channel.h"
#include "wifi_manager.h"

#if CONFIG_HID_BRIDGE_WIFI_ENABLE && CONFIG_HID_BRIDGE_WIFI_TARGET_ENABLE

#define OUTPUT_QUEUE_LENGTH 32
#define HEARTBEAT_INTERVAL_MS 500

static const char *TAG = "wifi_target";
static QueueHandle_t s_queue;

static esp_err_t send_frame(secure_channel_t *channel, const bridge_frame_t *frame)
{
    uint8_t packet[9 + BRIDGE_MAX_PAYLOAD];
    size_t packet_length = 0;
    ESP_RETURN_ON_ERROR(
        bridge_frame_serialize(frame, packet, sizeof(packet), &packet_length),
        TAG,
        "序列化目标帧失败");
    return secure_channel_send(channel, packet, packet_length);
}

static int connect_target(void)
{
    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (socket_fd < 0) {
        return -1;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_HID_BRIDGE_WIFI_TARGET_PORT),
    };
    if (inet_pton(AF_INET, CONFIG_HID_BRIDGE_WIFI_TARGET_HOST, &address.sin_addr) != 1 ||
        connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(socket_fd);
        return -1;
    }
    struct timeval socket_timeout = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout, sizeof(socket_timeout));
    return socket_fd;
}

static void __attribute__((unused)) wifi_target_task(void *context)
{
    (void)context;
    uint16_t local_sequence = 0;
    while (true) {
        if (!wifi_manager_wait_connected(pdMS_TO_TICKS(1000))) {
            continue;
        }
        int socket_fd = connect_target();
        if (socket_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        secure_channel_t channel = {.socket_fd = -1};
        esp_err_t result = secure_channel_client_handshake(
            &channel,
            socket_fd,
            CONFIG_HID_BRIDGE_WIFI_TARGET_PSK);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "目标 Agent 认证失败：%s", esp_err_to_name(result));
            close(socket_fd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        xQueueReset(s_queue);
        bridge_frame_t session_start = {
            .version = BRIDGE_PROTOCOL_VERSION,
            .type = BRIDGE_MESSAGE_SESSION_START,
            .sequence = local_sequence++,
            .payload_length = 0,
        };
        result = send_frame(&channel, &session_start);
        if (result == ESP_OK) {
            ESP_LOGI(
                TAG,
                "已安全连接目标 Agent %s:%d",
                CONFIG_HID_BRIDGE_WIFI_TARGET_HOST,
                CONFIG_HID_BRIDGE_WIFI_TARGET_PORT);
        }

        while (result == ESP_OK) {
            bridge_frame_t frame;
            if (xQueueReceive(s_queue, &frame, pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS)) == pdTRUE) {
                result = send_frame(&channel, &frame);
            } else {
                bridge_frame_t ping = {
                    .version = BRIDGE_PROTOCOL_VERSION,
                    .type = BRIDGE_MESSAGE_PING,
                    .sequence = local_sequence++,
                    .payload_length = 0,
                };
                result = send_frame(&channel, &ping);
            }
        }

        secure_channel_close(&channel);
        shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
        xQueueReset(s_queue);
        ESP_LOGW(TAG, "目标 Agent 已断开，等待重连");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#endif  // CONFIG_HID_BRIDGE_WIFI_ENABLE && CONFIG_HID_BRIDGE_WIFI_TARGET_ENABLE

esp_err_t wifi_target_output_init(void)
{
#if !CONFIG_HID_BRIDGE_WIFI_ENABLE || !CONFIG_HID_BRIDGE_WIFI_TARGET_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!wifi_manager_is_configured()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (strlen(CONFIG_HID_BRIDGE_WIFI_TARGET_PSK) < 16) {
        ESP_LOGE(TAG, "目标 Agent 预共享密钥不足 16 字节");
        return ESP_ERR_INVALID_ARG;
    }
    s_queue = xQueueCreate(OUTPUT_QUEUE_LENGTH, sizeof(bridge_frame_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t created = xTaskCreate(wifi_target_task, "wifi_target", 6144, NULL, 6, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
#endif
}

esp_err_t wifi_target_output_submit(const bridge_frame_t *frame)
{
#if !CONFIG_HID_BRIDGE_WIFI_ENABLE || !CONFIG_HID_BRIDGE_WIFI_TARGET_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (frame == NULL || s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_queue, frame, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
#endif
}
