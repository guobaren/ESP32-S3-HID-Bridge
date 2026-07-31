#include "output_router.h"
#include "ble_output.h"
#include "hid_output.h"
#include "wifi_target_output.h"

esp_err_t output_router_init(void)
{
    esp_err_t usb_result = hid_output_init();
    if (usb_result != ESP_OK) {
        return usb_result;
    }
    esp_err_t ble_result = ble_output_init();
    if (ble_result != ESP_OK && ble_result != ESP_ERR_NOT_SUPPORTED) {
        return ble_result;
    }
    esp_err_t wifi_result = wifi_target_output_init();
    return wifi_result == ESP_ERR_NOT_SUPPORTED ? ESP_OK : wifi_result;
}

esp_err_t output_router_submit(const bridge_frame_t *frame)
{
    esp_err_t usb_result = hid_output_submit(frame);
    esp_err_t ble_result = ble_output_submit(frame);
    esp_err_t wifi_result = wifi_target_output_submit(frame);
    if (usb_result != ESP_OK && usb_result != ESP_ERR_NOT_SUPPORTED) {
        return usb_result;
    }
    if (ble_result != ESP_OK && ble_result != ESP_ERR_NOT_SUPPORTED) {
        return ble_result;
    }
    return wifi_result == ESP_ERR_NOT_SUPPORTED ? ESP_OK : wifi_result;
}

esp_err_t output_router_release_all(void)
{
    bridge_frame_t frame = {
        .version = BRIDGE_PROTOCOL_VERSION,
        .type = BRIDGE_MESSAGE_RELEASE_ALL,
        .payload_length = 0,
    };
    return output_router_submit(&frame);
}
