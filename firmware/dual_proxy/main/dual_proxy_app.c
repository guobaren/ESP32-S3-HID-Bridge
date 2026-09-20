#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb.h"

#include "bridge_protocol.h"
#include "dual_status_led.h"
#include "hid_host_mouse.h"
#include "pc_hid_output.h"
#include "usb_cdc_control.h"
#include "uart1_link.h"

#define DEVICE_PROBE_WINDOW_MS 5000

static const char *TAG = "dual_proxy";
static uint8_t s_node_id[6];
static uint8_t s_role;

static int16_t read_i16_le(const uint8_t *value)
{
    return (int16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8));
}

static void on_software_release(void)
{
    dual_pc_hid_software_release();
}

static void on_software_frame(const dual_frame_t *frame)
{
    if (frame == NULL || frame->type != DUAL_MESSAGE_MOUSE_REPORT ||
        (frame->payload_length != 7 && frame->payload_length != 8)) {
        return;
    }
    dual_pc_hid_software_report(
        frame->payload[0],
        read_i16_le(&frame->payload[1]),
        read_i16_le(&frame->payload[3]),
        (int8_t)frame->payload[5],
        (int8_t)frame->payload[6]);
}

static void on_link_fault(void)
{
    if (s_role == DUAL_ROLE_PC_DEVICE) {
        dual_pc_hid_physical_release();
    }
}

static void on_link_frame(const dual_frame_t *frame)
{
    if (s_role != DUAL_ROLE_PC_DEVICE || frame == NULL) {
        return;
    }
    if (frame->type == DUAL_MESSAGE_PHYSICAL_RELEASE) {
        dual_pc_hid_physical_release();
        return;
    }
    if (frame->type != DUAL_MESSAGE_PHYSICAL_MOUSE || frame->payload_length != 9) {
        return;
    }
    /* UART1 reports are already normalized by the mouse-side boot parser. */
    dual_pc_hid_physical_report(
        frame->payload[2],
        read_i16_le(&frame->payload[3]),
        read_i16_le(&frame->payload[5]),
        (int8_t)frame->payload[7],
        (int8_t)frame->payload[8]);
}

static void on_mouse_report(
    uint8_t interface_number,
    uint8_t report_id,
    uint8_t buttons,
    int16_t x,
    int16_t y,
    int8_t wheel,
    int8_t pan)
{
    if (s_role != DUAL_ROLE_MOUSE_HOST) {
        return;
    }
    dual_uart1_set_usb_state(DUAL_USB_STATE_HID_CONNECTED);
    if (dual_uart1_send_mouse(interface_number, report_id, buttons, x, y, wheel, pan) != ESP_OK) {
        (void)dual_uart1_send_release(1);
    }
}

static void on_mouse_release(void)
{
    if (s_role == DUAL_ROLE_MOUSE_HOST) {
        dual_uart1_set_usb_state(DUAL_USB_STATE_DISCONNECTED);
        (void)dual_uart1_send_release(2);
    }
}

static esp_err_t start_pc_role(void)
{
    s_role = DUAL_ROLE_PC_DEVICE;
    esp_err_t result = dual_uart1_start(s_role, s_node_id, on_link_frame, on_link_fault);
    if (result != ESP_OK) {
        return result;
    }
    dual_uart1_set_usb_state(DUAL_USB_STATE_MOUNTED);
    result = dual_pc_hid_start_sender();
    if (result != ESP_OK) {
        return result;
    }
    result = dual_usb_cdc_control_start(on_software_frame, on_software_release);
    if (result != ESP_OK) {
        return result;
    }
    dual_status_led_set_role(DUAL_STATUS_LED_ROLE_PC_DEVICE);
    dual_status_led_set_pc_mounted(tud_mounted());
    ESP_LOGI(TAG, "角色锁定：PC_DEVICE；原生USB=HID+CDC控制，UART0=日志，UART1=实体鼠标链路");
    return ESP_OK;
}

static esp_err_t start_mouse_role(void)
{
    s_role = DUAL_ROLE_MOUSE_HOST;
    esp_err_t result = dual_uart1_start(s_role, s_node_id, NULL, on_mouse_release);
    if (result != ESP_OK) {
        return result;
    }
    dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
    result = dual_hid_host_start(on_mouse_report, on_mouse_release);
    if (result == ESP_OK) {
        dual_status_led_set_role(DUAL_STATUS_LED_ROLE_MOUSE_HOST);
    }
    return result;
}

static esp_err_t probe_device_then_switch(void)
{
    esp_err_t result = dual_pc_hid_install_device();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "USB Device安装失败，直接尝试USB Host：%s", esp_err_to_name(result));
        return start_mouse_role();
    }
    ESP_LOGI(TAG, "USB Device探测窗口 %d ms：枚举则PC_DEVICE，否则切换MOUSE_HOST", DEVICE_PROBE_WINDOW_MS);
    const int64_t deadline_us = esp_timer_get_time() + DEVICE_PROBE_WINDOW_MS * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        if (tud_mounted()) {
            return start_pc_role();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "电脑未枚举，卸载USB Device并切换Host");
    result = tinyusb_driver_uninstall();
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    return start_mouse_role();
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(s_node_id));
    const esp_err_t led_result = dual_status_led_init();
    if (led_result != ESP_OK) {
        ESP_LOGW(TAG, "板载状态灯初始化失败：%s；继续运行输入链路", esp_err_to_name(led_result));
    }
    ESP_LOGI(TAG, "dual_proxy启动 node=%02X%02X%02X%02X%02X%02X UART1 TX=GPIO17 RX=GPIO18 baud=921600",
             s_node_id[0], s_node_id[1], s_node_id[2],
             s_node_id[3], s_node_id[4], s_node_id[5]);
    const esp_err_t result = probe_device_then_switch();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "USB角色初始化失败：%s；停止输入", esp_err_to_name(result));
        dual_status_led_set_role(DUAL_STATUS_LED_ROLE_NONE);
        dual_pc_hid_release_all();
    }
}
