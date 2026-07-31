#include "ble_output.h"

#include <stdbool.h>
#include <string.h>
#include "esp_check.h"
#include "esp_hid_gap.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble_output";
static esp_hidd_dev_t *s_device;
static volatile bool s_connected;

static const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05,
    0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,

    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02,
    0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03,
    0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x03, 0x81, 0x06, 0x05, 0x0C, 0x0A, 0x38,
    0x02, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {.data = REPORT_MAP, .len = sizeof(REPORT_MAP)},
};

static esp_hid_device_config_t s_config = {
    .vendor_id = 0x303A,
    .product_id = 0x4002,
    .version = 0x0100,
    .device_name = CONFIG_HID_BRIDGE_BLE_DEVICE_NAME,
    .manufacturer_name = "HidBridge",
    .serial_number = "HIDBRIDGE",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

void ble_hid_task_start_up(void) {}
void ble_hid_task_shut_down(void) {}

static void ble_host_task(void *context)
{
    (void)context;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

static void hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *data)
{
    (void)handler_args;
    (void)base;
    esp_hidd_event_data_t *event = (esp_hidd_event_data_t *)data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "BLE HID 已启动并开始广播");
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = event->connect.status == ESP_OK;
        ESP_LOGI(TAG, "BLE HID %s", s_connected ? "已连接" : "连接失败");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        ESP_LOGI(TAG, "BLE HID 已断开，重新广播");
        esp_hid_ble_gap_adv_start();
        break;
    default:
        break;
    }
}

esp_err_t ble_output_init(void)
{
#if !CONFIG_HID_BRIDGE_BLE_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#elif !CONFIG_BT_NIMBLE_ENABLED
    ESP_LOGE(TAG, "BLE 输出已启用，但 NimBLE 未启用");
    return ESP_ERR_INVALID_STATE;
#else
    ESP_RETURN_ON_ERROR(esp_hid_gap_init(HIDD_BLE_MODE), TAG, "初始化 BLE GAP 失败");
    ESP_RETURN_ON_ERROR(
        esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GENERIC, s_config.device_name),
        TAG,
        "初始化 BLE 广播失败");
    ESP_RETURN_ON_ERROR(
        esp_hidd_dev_init(&s_config, ESP_HID_TRANSPORT_BLE, hidd_event_callback, &s_device),
        TAG,
        "初始化 BLE HID 失败");
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
#endif
}

esp_err_t ble_output_submit(const bridge_frame_t *frame)
{
#if !CONFIG_HID_BRIDGE_BLE_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (frame == NULL || s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_connected) {
        return ESP_OK;
    }
    if (frame->type == BRIDGE_MESSAGE_KEYBOARD_REPORT && frame->payload_length == 8) {
        return esp_hidd_dev_input_set(s_device, 0, 1, (uint8_t *)frame->payload, 8);
    }
    if (frame->type == BRIDGE_MESSAGE_MOUSE_REPORT && frame->payload_length == 5) {
        return esp_hidd_dev_input_set(s_device, 0, 2, (uint8_t *)frame->payload, 5);
    }
    if (frame->type == BRIDGE_MESSAGE_RELEASE_ALL) {
        uint8_t keyboard[8] = {0};
        uint8_t mouse[5] = {0};
        esp_err_t keyboard_result = esp_hidd_dev_input_set(s_device, 0, 1, keyboard, sizeof(keyboard));
        esp_err_t mouse_result = esp_hidd_dev_input_set(s_device, 0, 2, mouse, sizeof(mouse));
        return keyboard_result != ESP_OK ? keyboard_result : mouse_result;
    }
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
