#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "input_session.h"
#include "lwip/inet.h"
#include "wifi_provisioning.h"

static const char *TAG = "wifi_manager";
static const EventBits_t CONNECTED_BIT = BIT0;
static EventGroupHandle_t s_event_group;
static bool s_initialized;
static bool s_has_credentials;
static bool s_station_connect_enabled;
static bool s_provisioning;
static bool s_stop_provisioning_after_connect;
static TickType_t s_provisioning_connected_since;
static TickType_t s_disconnected_since;
static char s_ip_address[16];

static bool credentials_are_valid(const char *ssid, const char *password)
{
    size_t ssid_length = ssid == NULL ? 0 : strlen(ssid);
    size_t password_length = password == NULL ? 0 : strlen(password);
    return ssid_length > 0 && ssid_length <= 32 &&
           (password_length == 0 || (password_length >= 8 && password_length <= 63));
}

static void wifi_event_handler(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && s_station_connect_enabled) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_event_group, CONNECTED_BIT);
        s_ip_address[0] = '\0';
        s_provisioning_connected_since = 0;
        if (s_disconnected_since == 0) {
            s_disconnected_since = xTaskGetTickCount();
        }
        if (s_station_connect_enabled) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(
            s_ip_address,
            sizeof(s_ip_address),
            IPSTR,
            IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi 已连接，地址：%s", s_ip_address);
        s_disconnected_since = 0;
        xEventGroupSetBits(s_event_group, CONNECTED_BIT);
    }
}

static void wifi_manager_task(void *argument)
{
    (void)argument;
    const TickType_t fallback_delay = pdMS_TO_TICKS(CONFIG_HID_BRIDGE_PROVISIONING_FALLBACK_SECONDS * 1000);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_provisioning && s_stop_provisioning_after_connect && wifi_manager_is_connected()) {
            if (s_provisioning_connected_since == 0) {
                s_provisioning_connected_since = xTaskGetTickCount();
                ESP_LOGI(TAG, "新网络连接成功，10 秒后关闭临时热点");
            } else if (xTaskGetTickCount() - s_provisioning_connected_since >= pdMS_TO_TICKS(10000)) {
                wifi_provisioning_stop();
                s_provisioning = false;
                s_stop_provisioning_after_connect = false;
                s_provisioning_connected_since = 0;
                esp_wifi_set_mode(WIFI_MODE_STA);
                ESP_LOGI(TAG, "配网完成，临时热点已关闭");
            }
            continue;
        }
        if (!s_provisioning && !wifi_manager_is_connected() &&
            s_disconnected_since != 0 &&
            xTaskGetTickCount() - s_disconnected_since >= fallback_delay) {
            input_session_release_all();
            esp_err_t result = wifi_manager_start_provisioning();
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "自动启动配网失败：%s", esp_err_to_name(result));
                s_disconnected_since = xTaskGetTickCount();
            }
        }
    }
}

esp_err_t wifi_manager_apply_credentials(const char *ssid, const char *password)
{
#if !CONFIG_HID_BRIDGE_WIFI_ENABLE
    (void)ssid;
    (void)password;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_initialized || !credentials_are_valid(ssid, password)) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config = {0};
    size_t ssid_length = strlen(ssid);
    memcpy(config.sta.ssid, ssid, ssid_length);
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = strlen(password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    xEventGroupClearBits(s_event_group, CONNECTED_BIT);
    s_ip_address[0] = '\0';
    s_has_credentials = false;
    s_station_connect_enabled = false;
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "保存 Wi-Fi 凭据失败");
    s_has_credentials = true;
    s_station_connect_enabled = true;
    s_stop_provisioning_after_connect = true;
    s_provisioning_connected_since = 0;
    s_disconnected_since = xTaskGetTickCount();
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "连接新 Wi-Fi 失败");
    ESP_LOGI(TAG, "已保存新的 Wi-Fi 配置，正在连接 SSID：%.*s", (int)ssid_length, ssid);
    return ESP_OK;
#endif
}

esp_err_t wifi_manager_start_provisioning(void)
{
#if !CONFIG_HID_BRIDGE_WIFI_ENABLE || !CONFIG_HID_BRIDGE_PROVISIONING_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_provisioning) {
        return ESP_OK;
    }
    size_t access_point_password_length = strlen(CONFIG_HID_BRIDGE_PROVISIONING_AP_PASSWORD);
    if (access_point_password_length > 0 &&
        (access_point_password_length < 8 || access_point_password_length > 63)) {
        ESP_LOGE(TAG, "配网热点密码必须留空或为 8 至 63 字节");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "读取设备地址失败");
    wifi_config_t access_point = {0};
    snprintf(
        (char *)access_point.ap.ssid,
        sizeof(access_point.ap.ssid),
        "HID-Bridge-Setup-%02X%02X",
        mac[4],
        mac[5]);
    access_point.ap.ssid_len = strlen((char *)access_point.ap.ssid);
    strlcpy(
        (char *)access_point.ap.password,
        CONFIG_HID_BRIDGE_PROVISIONING_AP_PASSWORD,
        sizeof(access_point.ap.password));
    access_point.ap.max_connection = 4;
    access_point.ap.authmode = strlen(CONFIG_HID_BRIDGE_PROVISIONING_AP_PASSWORD) == 0
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA2_PSK;

    s_station_connect_enabled = false;
    xEventGroupClearBits(s_event_group, CONNECTED_BIT);
    s_ip_address[0] = '\0';
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "切换 APSTA 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &access_point), TAG, "配置临时热点失败");
    ESP_RETURN_ON_ERROR(wifi_provisioning_start(), TAG, "启动配网页面失败");
    s_provisioning = true;
    s_stop_provisioning_after_connect = s_has_credentials;
    s_provisioning_connected_since = 0;
    ESP_LOGW(
        TAG,
        "已进入配网模式：连接热点 %s，访问 http://192.168.4.1",
        access_point.ap.ssid);
    return ESP_OK;
#endif
}

esp_err_t wifi_manager_init(void)
{
#if !CONFIG_HID_BRIDGE_WIFI_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "初始化网络栈失败");
    esp_err_t loop_result = esp_event_loop_create_default();
    if (loop_result != ESP_OK && loop_result != ESP_ERR_INVALID_STATE) {
        return loop_result;
    }
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "初始化 Wi-Fi 失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "启用 Wi-Fi NVS 存储失败");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
        TAG,
        "注册 Wi-Fi 事件失败");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
        TAG,
        "注册 IP 事件失败");

    s_initialized = true;
    wifi_config_t stored = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &stored), TAG, "读取 Wi-Fi NVS 配置失败");
    if (stored.sta.ssid[0] == '\0' && strlen(CONFIG_HID_BRIDGE_WIFI_SSID) > 0) {
        if (!credentials_are_valid(CONFIG_HID_BRIDGE_WIFI_SSID, CONFIG_HID_BRIDGE_WIFI_PASSWORD)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(
            stored.sta.ssid,
            CONFIG_HID_BRIDGE_WIFI_SSID,
            strlen(CONFIG_HID_BRIDGE_WIFI_SSID));
        strlcpy(
            (char *)stored.sta.password,
            CONFIG_HID_BRIDGE_WIFI_PASSWORD,
            sizeof(stored.sta.password));
        stored.sta.threshold.authmode = strlen(CONFIG_HID_BRIDGE_WIFI_PASSWORD) == 0
            ? WIFI_AUTH_OPEN
            : WIFI_AUTH_WPA2_PSK;
        stored.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        ESP_RETURN_ON_ERROR(
            esp_wifi_set_config(WIFI_IF_STA, &stored),
            TAG,
            "保存编译期 Wi-Fi 配置失败");
    }
    s_has_credentials = stored.sta.ssid[0] != '\0';
    s_station_connect_enabled = s_has_credentials;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "设置 Station 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "启动 Wi-Fi 失败");
    s_disconnected_since = xTaskGetTickCount();

    BaseType_t created = xTaskCreate(wifi_manager_task, "wifi_manager", 4096, NULL, 5, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_has_credentials) {
        return wifi_manager_start_provisioning();
    }
    return ESP_OK;
#endif
}

bool wifi_manager_wait_connected(TickType_t timeout)
{
    if (!s_initialized || s_event_group == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_event_group, CONNECTED_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & CONNECTED_BIT) != 0;
}

bool wifi_manager_is_configured(void)
{
    return s_initialized;
}

bool wifi_manager_is_connected(void)
{
    return s_event_group != NULL && (xEventGroupGetBits(s_event_group) & CONNECTED_BIT) != 0;
}

bool wifi_manager_is_provisioning(void)
{
    return s_provisioning;
}

esp_err_t wifi_manager_get_ip(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_manager_is_connected() || s_ip_address[0] == '\0') {
        buffer[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(buffer, s_ip_address, buffer_size);
    return ESP_OK;
}
