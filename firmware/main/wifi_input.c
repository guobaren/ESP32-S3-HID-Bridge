#include "wifi_input.h"

#include <string.h>
#include "bridge_protocol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_session.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "secure_channel.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_input";

static void parser_callback(const bridge_frame_t *frame, void *context)
{
    (void)context;
    input_session_handle(BRIDGE_INPUT_WIFI, frame);
}

static void wifi_input_task(void *context)
{
    (void)context;
    while (!wifi_manager_wait_connected(portMAX_DELAY)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_socket < 0) {
        ESP_LOGE(TAG, "无法创建监听套接字");
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_HID_BRIDGE_WIFI_INPUT_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_socket, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_socket, 1) != 0) {
        ESP_LOGE(TAG, "无法监听 Wi-Fi 输入端口 %d", CONFIG_HID_BRIDGE_WIFI_INPUT_PORT);
        close(listen_socket);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "安全 Wi-Fi 输入正在监听端口 %d", CONFIG_HID_BRIDGE_WIFI_INPUT_PORT);

    while (true) {
        int client_socket = accept(listen_socket, NULL, NULL);
        if (client_socket < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        struct timeval socket_timeout = {.tv_sec = 2, .tv_usec = 0};
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout));
        setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout, sizeof(socket_timeout));
        secure_channel_t channel = {.socket_fd = -1};
        esp_err_t result = secure_channel_server_handshake(
            &channel,
            client_socket,
            CONFIG_HID_BRIDGE_WIFI_INPUT_PSK);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "拒绝未通过认证的 Wi-Fi 输入连接：%s", esp_err_to_name(result));
            close(client_socket);
            continue;
        }

        ESP_LOGI(TAG, "Wi-Fi 输入客户端已认证");
        bridge_parser_t parser;
        bridge_parser_init(&parser, parser_callback, NULL);
        uint8_t packet[9 + BRIDGE_MAX_PAYLOAD];
        while (true) {
            size_t packet_length = 0;
            result = secure_channel_receive(&channel, packet, sizeof(packet), &packet_length);
            if (result != ESP_OK) {
                break;
            }
            bridge_parser_feed(&parser, packet, packet_length);
        }

        input_session_disconnected(BRIDGE_INPUT_WIFI);
        secure_channel_close(&channel);
        shutdown(client_socket, SHUT_RDWR);
        close(client_socket);
        ESP_LOGI(TAG, "Wi-Fi 输入客户端已断开");
    }
}

esp_err_t wifi_input_start(void)
{
#if !CONFIG_HID_BRIDGE_WIFI_ENABLE || !CONFIG_HID_BRIDGE_WIFI_INPUT_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!wifi_manager_is_configured()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (strlen(CONFIG_HID_BRIDGE_WIFI_INPUT_PSK) < 16) {
        ESP_LOGW(TAG, "Wi-Fi 输入预共享密钥不足 16 字节，服务不会启动");
        return ESP_ERR_INVALID_ARG;
    }
    BaseType_t created = xTaskCreate(wifi_input_task, "wifi_input", 6144, NULL, 7, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
#endif
}
