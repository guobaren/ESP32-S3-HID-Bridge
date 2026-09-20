#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#define LINK_UART UART_NUM_1
#define LINK_TX_GPIO 17
#define LINK_RX_GPIO 18
#define LINK_BAUD 921600
#define LINK_RX_BUFFER_SIZE 2048
#define DEVICE_PROBE_WINDOW_MS 5000
#define LINK_PERIOD_MS 250
#define LINK_TIMEOUT_MS 1500
#define LINK_MAGIC_0 0xA7
#define LINK_MAGIC_1 0x51
#define LINK_VERSION 1
#define HID_REPORT_LOG_INTERVAL_MS 100

static const char *TAG = "conn_validate";

typedef enum {
    ROLE_PROBING_DEVICE = 0,
    ROLE_PC_DEVICE = 1,
    ROLE_MOUSE_HOST = 2,
    ROLE_USB_FAILED = 3,
} validation_role_t;

typedef enum {
    USB_STATE_WAITING = 0,
    USB_STATE_MOUNTED = 1,
    USB_STATE_HID_CONNECTED = 2,
    USB_STATE_DISCONNECTED = 3,
    USB_STATE_ERROR = 4,
} validation_usb_state_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[2];
    uint8_t version;
    uint8_t role;
    uint8_t usb_state;
    uint8_t reserved;
    uint8_t node_id[6];
    uint32_t sequence;
    uint32_t hid_reports;
    uint16_t crc;
} link_frame_t;

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} hid_driver_event_t;

static volatile validation_role_t s_role = ROLE_PROBING_DEVICE;
static volatile validation_usb_state_t s_usb_state = USB_STATE_WAITING;
static volatile uint32_t s_hid_report_count;
static volatile uint32_t s_hid_error_count;
static uint8_t s_node_id[6];
static QueueHandle_t s_hid_event_queue;
static uint32_t s_link_tx_count;
static uint32_t s_link_rx_count;
static uint32_t s_link_crc_errors;
static int64_t s_last_peer_rx_us;

static uint16_t crc16_ccitt(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static const char *role_name(validation_role_t role)
{
    switch (role) {
    case ROLE_PROBING_DEVICE: return "PROBING_DEVICE";
    case ROLE_PC_DEVICE: return "PC_DEVICE";
    case ROLE_MOUSE_HOST: return "MOUSE_HOST";
    default: return "USB_FAILED";
    }
}

static esp_err_t link_uart_init(void)
{
    const uart_config_t config = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(LINK_UART, &config), TAG, "UART1参数配置失败");
    ESP_RETURN_ON_ERROR(
        uart_set_pin(LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
        TAG,
        "UART1引脚配置失败");
    return uart_driver_install(LINK_UART, LINK_RX_BUFFER_SIZE, 0, 0, NULL, 0);
}

static bool link_frame_valid(const link_frame_t *frame)
{
    if (frame->magic[0] != LINK_MAGIC_0 || frame->magic[1] != LINK_MAGIC_1 ||
        frame->version != LINK_VERSION) {
        return false;
    }
    return crc16_ccitt((const uint8_t *)frame, offsetof(link_frame_t, crc)) == frame->crc;
}

static void link_task(void *argument)
{
    (void)argument;
    uint8_t receive_window[sizeof(link_frame_t)] = {0};
    size_t receive_length = 0;
    uint32_t sequence = 0;
    int64_t last_send_us = 0;
    int64_t last_summary_us = 0;
    bool peer_online = false;

    while (true) {
        uint8_t chunk[64];
        int received = uart_read_bytes(LINK_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(10));
        for (int i = 0; i < received; ++i) {
            if (receive_length < sizeof(receive_window)) {
                receive_window[receive_length++] = chunk[i];
            }
            if (receive_length == sizeof(receive_window)) {
                const link_frame_t *frame = (const link_frame_t *)receive_window;
                if (link_frame_valid(frame)) {
                    ++s_link_rx_count;
                    s_last_peer_rx_us = esp_timer_get_time();
                    if (!peer_online) {
                        peer_online = true;
                        ESP_LOGI(
                            TAG,
                            "UART1对端上线: id=%02X%02X%02X%02X%02X%02X role=%s usb=%u seq=%" PRIu32,
                            frame->node_id[0], frame->node_id[1], frame->node_id[2],
                            frame->node_id[3], frame->node_id[4], frame->node_id[5],
                            role_name((validation_role_t)frame->role), frame->usb_state, frame->sequence);
                    }
                    receive_length = 0;
                } else {
                    if (receive_window[0] == LINK_MAGIC_0 && receive_window[1] == LINK_MAGIC_1) {
                        ++s_link_crc_errors;
                    }
                    memmove(receive_window, receive_window + 1, sizeof(receive_window) - 1);
                    receive_length = sizeof(receive_window) - 1;
                }
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_send_us >= LINK_PERIOD_MS * 1000LL) {
            link_frame_t frame = {
                .magic = {LINK_MAGIC_0, LINK_MAGIC_1},
                .version = LINK_VERSION,
                .role = (uint8_t)s_role,
                .usb_state = (uint8_t)s_usb_state,
                .sequence = sequence++,
                .hid_reports = s_hid_report_count,
            };
            memcpy(frame.node_id, s_node_id, sizeof(frame.node_id));
            frame.crc = crc16_ccitt((const uint8_t *)&frame, offsetof(link_frame_t, crc));
            if (uart_write_bytes(LINK_UART, &frame, sizeof(frame)) == sizeof(frame)) {
                ++s_link_tx_count;
            }
            last_send_us = now_us;
        }

        if (peer_online && now_us - s_last_peer_rx_us > LINK_TIMEOUT_MS * 1000LL) {
            peer_online = false;
            ESP_LOGW(TAG, "UART1对端超时");
        }
        if (now_us - last_summary_us >= 2000000LL) {
            ESP_LOGI(
                TAG,
                "状态 role=%s usb=%u UART1 tx=%" PRIu32 " rx=%" PRIu32
                " crc=%" PRIu32 " peer=%s HID reports=%" PRIu32 " errors=%" PRIu32,
                role_name(s_role), s_usb_state, s_link_tx_count, s_link_rx_count,
                s_link_crc_errors, peer_online ? "online" : "offline", s_hid_report_count,
                s_hid_error_count);
            last_summary_us = now_us;
        }
    }
}

static void log_hid_report(hid_host_device_handle_t handle)
{
    uint8_t data[64];
    size_t length = 0;
    esp_err_t result = hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &length);
    if (result != ESP_OK) {
        ++s_hid_error_count;
        ESP_LOGW(TAG, "读取HID报告失败: %s", esp_err_to_name(result));
        return;
    }

    ++s_hid_report_count;
    static int64_t last_log_us;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_log_us < HID_REPORT_LOG_INTERVAL_MS * 1000LL) {
        return;
    }
    char hex[3 * 64 + 1];
    size_t offset = 0;
    for (size_t i = 0; i < length && offset + 3 < sizeof(hex); ++i) {
        offset += (size_t)snprintf(hex + offset, sizeof(hex) - offset, "%02X%s", data[i], i + 1 < length ? " " : "");
    }
    ESP_LOGI(TAG, "HID报告 #%" PRIu32 " len=%u data=%s", s_hid_report_count, (unsigned)length, hex);
    last_log_us = now_us;
}

static void hid_interface_callback(
    hid_host_device_handle_t handle,
    hid_host_interface_event_t event,
    void *argument)
{
    (void)argument;
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        ++s_hid_error_count;
        return;
    }
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        log_hid_report(handle);
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        s_usb_state = USB_STATE_DISCONNECTED;
        ESP_LOGW(TAG, "HID接口断开: proto=%u", params.proto);
        if (hid_host_device_close(handle) != ESP_OK) {
            ++s_hid_error_count;
        }
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ++s_hid_error_count;
        ESP_LOGW(TAG, "HID传输错误: proto=%u", params.proto);
        break;
    default:
        ESP_LOGW(TAG, "未处理HID接口事件: %u", event);
        break;
    }
}

static void hid_driver_callback(
    hid_host_device_handle_t handle,
    hid_host_driver_event_t event,
    void *argument)
{
    (void)argument;
    if (s_hid_event_queue == NULL) {
        return;
    }
    const hid_driver_event_t queued = {.handle = handle, .event = event};
    if (xQueueSend(s_hid_event_queue, &queued, 0) != pdTRUE) {
        ++s_hid_error_count;
    }
}

static void hid_event_task(void *argument)
{
    (void)argument;
    hid_driver_event_t event;
    while (true) {
        if (xQueueReceive(s_hid_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.event != HID_HOST_DRIVER_EVENT_CONNECTED) {
            continue;
        }
        hid_host_dev_params_t params;
        if (hid_host_device_get_params(event.handle, &params) != ESP_OK) {
            ++s_hid_error_count;
            continue;
        }
        hid_host_dev_info_t info = {0};
        esp_err_t info_result = hid_host_get_device_info(event.handle, &info);
        if (info_result == ESP_OK) {
            ESP_LOGI(
                TAG,
                "HID设备连接: addr=%u interface=%u subclass=%u proto=%u VID:PID=%04X:%04X",
                params.addr, params.iface_num, params.sub_class, params.proto, info.VID, info.PID);
        } else {
            ESP_LOGW(
                TAG,
                "HID设备连接但读取VID/PID失败: addr=%u interface=%u error=%s",
                params.addr, params.iface_num, esp_err_to_name(info_result));
        }
        const hid_host_device_config_t config = {
            .callback = hid_interface_callback,
            .callback_arg = NULL,
        };
        esp_err_t result = hid_host_device_open(event.handle, &config);
        if (result == ESP_OK && params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            result = hid_class_request_set_protocol(event.handle, HID_REPORT_PROTOCOL_BOOT);
        }
        if (result == ESP_OK) {
            result = hid_host_device_start(event.handle);
        }
        if (result == ESP_OK) {
            s_usb_state = USB_STATE_HID_CONNECTED;
            ESP_LOGI(TAG, "鼠标/HID通信已建立，等待真实输入报告");
        } else {
            ++s_hid_error_count;
            s_usb_state = USB_STATE_ERROR;
            ESP_LOGE(TAG, "启动HID接口失败: %s", esp_err_to_name(result));
        }
    }
}

static void usb_host_library_task(void *argument)
{
    TaskHandle_t owner = (TaskHandle_t)argument;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t result = usb_host_install(&config);
    xTaskNotify(owner, (uint32_t)result, eSetValueWithOverwrite);
    if (result != ESP_OK) {
        vTaskDelete(NULL);
    }
    while (true) {
        uint32_t flags = 0;
        result = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (result != ESP_OK) {
            ++s_hid_error_count;
            ESP_LOGE(TAG, "USB Host事件处理失败: %s", esp_err_to_name(result));
        }
    }
}

static esp_err_t start_host_mode(void)
{
    s_role = ROLE_MOUSE_HOST;
    s_usb_state = USB_STATE_WAITING;
    if (xTaskCreatePinnedToCore(
            usb_host_library_task, "usb_host_events", 4096, xTaskGetCurrentTaskHandle(), 2, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t notification = ESP_FAIL;
    if (xTaskNotifyWait(0, UINT32_MAX, &notification, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if ((esp_err_t)notification != ESP_OK) {
        return (esp_err_t)notification;
    }

    s_hid_event_queue = xQueueCreate(8, sizeof(hid_driver_event_t));
    if (s_hid_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const hid_host_driver_config_t config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_driver_callback,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(hid_host_install(&config), TAG, "HID Host驱动安装失败");
    if (xTaskCreate(hid_event_task, "hid_events", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB角色锁定: MOUSE_HOST；等待鼠标枚举");
    return ESP_OK;
}

static esp_err_t probe_device_then_switch(void)
{
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&config), TAG, "TinyUSB Device安装失败");
    ESP_LOGI(TAG, "USB Device探测窗口 %d ms：连接电脑会锁定PC_DEVICE，否则切到MOUSE_HOST", DEVICE_PROBE_WINDOW_MS);

    const int64_t deadline_us = esp_timer_get_time() + DEVICE_PROBE_WINDOW_MS * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        if (tud_mounted()) {
            s_role = ROLE_PC_DEVICE;
            s_usb_state = USB_STATE_MOUNTED;
            ESP_LOGI(TAG, "USB角色锁定: PC_DEVICE");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "电脑未枚举，卸载USB Device并切换Host");
    ESP_RETURN_ON_ERROR(tinyusb_driver_uninstall(), TAG, "TinyUSB Device卸载失败");
    vTaskDelay(pdMS_TO_TICKS(200));
    return start_host_mode();
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(s_node_id));
    ESP_LOGI(
        TAG,
        "连接验证固件启动 node=%02X%02X%02X%02X%02X%02X UART1 TX=GPIO%d RX=GPIO%d %d baud",
        s_node_id[0], s_node_id[1], s_node_id[2], s_node_id[3], s_node_id[4], s_node_id[5],
        LINK_TX_GPIO, LINK_RX_GPIO, LINK_BAUD);
    ESP_ERROR_CHECK(link_uart_init());
    ESP_ERROR_CHECK(xTaskCreate(link_task, "uart1_link", 4096, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    esp_err_t result = probe_device_then_switch();
    if (result != ESP_OK) {
        s_role = ROLE_USB_FAILED;
        s_usb_state = USB_STATE_ERROR;
        ESP_LOGE(TAG, "USB角色验证初始化失败，停止后续阶段: %s", esp_err_to_name(result));
    }
}
