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
#include "hid_device_profile.h"
#include "pc_hid_output.h"
#include "usb_cdc_control.h"
#include "uart0_control.h"
#include "uart1_link.h"

#define DEVICE_PROBE_WINDOW_MS 5000
#define PROFILE_TRANSFER_TIMEOUT_MS 1000

static const char *TAG = "dual_proxy";
static uint8_t s_node_id[6];
static uint8_t s_role;
static hid_profile_receiver_t s_profile_receiver;
static int64_t s_profile_started_us;

static void on_profile_published(const hid_device_profile_t *profile, void *context);

static int16_t read_i16_le(const uint8_t *value)
{
    return (int16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8));
}

static void on_software_release(void)
{
    if (s_role == DUAL_ROLE_MOUSE_HOST) {
        if (dual_uart1_send_software_release() != ESP_OK) {
            ESP_LOGW(TAG, "软件release无法送入板间UART");
        }
    } else if (s_role == DUAL_ROLE_PC_DEVICE) {
        dual_pc_hid_software_release();
    }
}

static void on_software_frame(const dual_frame_t *frame)
{
    if (frame == NULL || frame->type != DUAL_MESSAGE_MOUSE_REPORT ||
        (frame->payload_length != 7 && frame->payload_length != 8)) {
        return;
    }
    if (s_role == DUAL_ROLE_MOUSE_HOST) {
        if (dual_uart1_send_software_mouse(frame->payload, frame->payload_length) != ESP_OK) {
            ESP_LOGW(TAG, "软件MouseReport无法送入板间UART，发送release");
            (void)dual_uart1_send_software_release();
        }
    } else if (s_role == DUAL_ROLE_PC_DEVICE) {
        dual_pc_hid_software_report(
            frame->payload[0],
            read_i16_le(&frame->payload[1]),
            read_i16_le(&frame->payload[3]),
            (int8_t)frame->payload[5],
            (int8_t)frame->payload[6],
            frame->payload_length == 8U ? frame->payload[7] : 0U);
    }
}

static void pc_device_gone(const char *reason)
{
    ESP_LOGW(TAG, "物理鼠标不可用，断开接收端USB：%s", reason);
    dual_pc_hid_vendor_link_fault();
    dual_pc_hid_release_all();
    const esp_err_t result = dual_pc_hid_schedule_disconnect();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "接收端USB断开排队失败：%s", esp_err_to_name(result));
    }
}

static void on_link_fault(void)
{
    if (s_role == DUAL_ROLE_PC_DEVICE) {
        pc_device_gone("板间UART超时或故障");
    } else if (s_role == DUAL_ROLE_MOUSE_HOST) {
        dual_hid_host_clear_control_queue();
    }
}

static void on_link_frame(const dual_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }
    if (s_role == DUAL_ROLE_PC_DEVICE) {
        const int64_t now_us = esp_timer_get_time();
        if (s_profile_started_us != 0 &&
            now_us - s_profile_started_us > PROFILE_TRANSFER_TIMEOUT_MS * 1000LL) {
            hid_profile_receiver_init(&s_profile_receiver, on_profile_published, NULL);
            s_profile_started_us = 0;
            pc_device_gone("Profile传输超时");
            if (frame->type != DUAL_MESSAGE_PROFILE_BEGIN) {
                return;
            }
        }
        if (frame->type == DUAL_MESSAGE_PROFILE_BEGIN ||
            frame->type == DUAL_MESSAGE_PROFILE_CHUNK ||
            frame->type == DUAL_MESSAGE_PROFILE_COMMIT) {
            if (!hid_profile_receiver_accept_frame(&s_profile_receiver, frame)) {
                s_profile_started_us = 0;
                pc_device_gone("Profile校验失败");
            } else if (frame->type == DUAL_MESSAGE_PROFILE_COMMIT) {
                s_profile_started_us = 0;
            } else {
                /* 超时按相邻分片的静默时长计算，而不是限制整个低优先级传输时长。 */
                s_profile_started_us = now_us;
            }
            return;
        }
        if (frame->type == DUAL_MESSAGE_DEVICE_GONE) {
            hid_profile_receiver_init(&s_profile_receiver, on_profile_published, NULL);
            s_profile_started_us = 0;
            pc_device_gone("鼠标侧报告物理USB拔出");
            return;
        }
        if (frame->type == DUAL_MESSAGE_PHYSICAL_RELEASE) {
            dual_pc_hid_physical_release();
            return;
        }
        if (frame->type == DUAL_MESSAGE_SOFTWARE_RELEASE) {
            dual_pc_hid_software_release();
            return;
        }
        if (frame->type == DUAL_MESSAGE_SOFTWARE_MOUSE &&
            (frame->payload_length == 7U || frame->payload_length == 8U)) {
            dual_pc_hid_software_report(
                frame->payload[0],
                read_i16_le(&frame->payload[1]),
                read_i16_le(&frame->payload[3]),
                (int8_t)frame->payload[5],
                (int8_t)frame->payload[6],
                frame->payload_length == 8U ? frame->payload[7] : 0U);
            return;
        }
        if (frame->type == DUAL_MESSAGE_RAW_HID_INPUT ||
            frame->type == DUAL_MESSAGE_HID_GET_REPORT_RESPONSE) {
            dual_pc_hid_handle_vendor_frame(frame);
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
    } else if (s_role == DUAL_ROLE_MOUSE_HOST &&
               (frame->type == DUAL_MESSAGE_HID_SET_REPORT ||
                frame->type == DUAL_MESSAGE_HID_GET_REPORT_REQUEST)) {
        dual_hid_host_handle_control_frame(frame);
    }
}

static void on_profile_published(const hid_device_profile_t *profile, void *context)
{
    (void)context;
    uint16_t vid = 0;
    uint16_t pid = 0;
    if (profile != NULL && profile->device_descriptor.length >= 12U) {
        const uint8_t *descriptor = profile->device_descriptor.data;
        vid = (uint16_t)descriptor[8] | ((uint16_t)descriptor[9] << 8);
        pid = (uint16_t)descriptor[10] | ((uint16_t)descriptor[11] << 8);
    }
    ESP_LOGI(TAG,
             "收到物理HID Profile观察快照：VID:PID=%04X:%04X manufacturer_len=%u "
             "product_len=%u serial_present=%s interfaces=%u length_flags=%02X",
             vid, pid, profile != NULL ? profile->manufacturer.length : 0,
             profile != NULL ? profile->product.length : 0,
             profile != NULL && profile->serial.length != 0 ? "yes" : "no",
             profile != NULL ? profile->report_descriptor_count : 0,
             profile != NULL ? profile->flags : 0);
    const esp_err_t result = dual_pc_hid_schedule_reconfigure(profile);
    s_profile_started_us = 0;
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "动态USB克隆排队失败：%s", esp_err_to_name(result));
    }
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

static void on_mouse_release(bool device_gone)
{
    if (s_role == DUAL_ROLE_MOUSE_HOST) {
        dual_uart1_set_usb_state(DUAL_USB_STATE_DISCONNECTED);
        (void)dual_uart1_send_release(2);
        if (device_gone) {
            (void)dual_uart1_send_device_gone(1);
        }
    }
}

static esp_err_t start_pc_role(void)
{
    s_role = DUAL_ROLE_PC_DEVICE;
    hid_profile_receiver_init(&s_profile_receiver, on_profile_published, NULL);
    esp_err_t result = dual_uart1_start(s_role, s_node_id, on_link_frame, on_link_fault);
    if (result != ESP_OK) {
        return result;
    }
    dual_uart1_set_usb_state(DUAL_USB_STATE_MOUNTED);
    result = dual_pc_hid_start_sender();
    if (result != ESP_OK) {
        return result;
    }
    dual_pc_hid_enable_reconfigure();
    result = dual_pc_hid_schedule_disconnect();
    if (result != ESP_OK) {
        return result;
    }
    dual_status_led_set_role(DUAL_STATUS_LED_ROLE_PC_DEVICE);
    dual_status_led_set_pc_mounted(tud_mounted());
    ESP_LOGI(TAG, "角色锁定：PC_DEVICE；原生USB=严格鼠标克隆，UART1=实体+软件输入链路");
    return ESP_OK;
}

static esp_err_t start_mouse_role(void)
{
    s_role = DUAL_ROLE_MOUSE_HOST;
    /*
     * 鼠标侧同样必须接收来自电脑侧的 HID++ SET/GET_REPORT。
     * 这里只注册断开回调会导致描述符虽然克隆成功，但 G HUB 的控制请求
     * 在 UART 解析后被静默丢弃。
     */
    esp_err_t result = dual_uart1_start(s_role, s_node_id, on_link_frame, on_link_fault);
    if (result != ESP_OK) {
        return result;
    }
    dual_uart1_set_usb_state(DUAL_USB_STATE_WAITING);
    result = dual_hid_host_start(on_mouse_report, on_mouse_release);
    if (result != ESP_OK) {
        return result;
    }
    result = dual_uart0_control_start(
        DUAL_ROLE_MOUSE_HOST, on_software_frame, on_software_release);
    if (result != ESP_OK) {
        return result;
    }
    dual_status_led_set_role(DUAL_STATUS_LED_ROLE_MOUSE_HOST);
    ESP_LOGI(TAG, "角色锁定：MOUSE_HOST；原生USB=真实鼠标Host，UART0=电脑A软件控制，UART1=透明代理链路");
    return ESP_OK;
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
