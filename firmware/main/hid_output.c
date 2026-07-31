#include "hid_output.h"

#include <stdbool.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "esp_tinyusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tusb.h"

#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE 2
#define HID_QUEUE_LENGTH 32
#define USB_INTERFACE_COUNT 1
#define USB_HID_ENDPOINT 0x81
#define USB_CONFIG_TOTAL_LENGTH (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const char *TAG = "hid_output";
static QueueHandle_t s_hid_queue;

typedef struct {
    bridge_message_type_t type;
    uint8_t length;
    uint8_t payload[8];
} hid_event_t;

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
};

static const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        USB_INTERFACE_COUNT,
        0,
        USB_CONFIG_TOTAL_LENGTH,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100),
    TUD_HID_DESCRIPTOR(
        0,
        0,
        HID_ITF_PROTOCOL_NONE,
        sizeof(s_hid_report_descriptor),
        USB_HID_ENDPOINT,
        16,
        1),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t *buffer,
    uint16_t requested_length)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)requested_length;
    return 0;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t const *buffer,
    uint16_t buffer_size)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)buffer_size;
}

static bool wait_until_hid_ready(TickType_t timeout)
{
    const TickType_t start = xTaskGetTickCount();
    while ((!tud_mounted() || !tud_hid_ready()) &&
           xTaskGetTickCount() - start < timeout) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return tud_mounted() && tud_hid_ready();
}

static void send_release_all(void)
{
    static const uint8_t keyboard[8] = {0};
    static const uint8_t mouse[5] = {0};

    if (wait_until_hid_ready(pdMS_TO_TICKS(100))) {
        tud_hid_report(REPORT_ID_KEYBOARD, keyboard, sizeof(keyboard));
    }
    if (wait_until_hid_ready(pdMS_TO_TICKS(100))) {
        tud_hid_report(REPORT_ID_MOUSE, mouse, sizeof(mouse));
    }
}

static void hid_sender_task(void *argument)
{
    (void)argument;
    hid_event_t event;

    while (true) {
        if (xQueueReceive(s_hid_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event.type == BRIDGE_MESSAGE_RELEASE_ALL) {
            send_release_all();
            continue;
        }

        if (!wait_until_hid_ready(pdMS_TO_TICKS(100))) {
            continue;
        }

        if (event.type == BRIDGE_MESSAGE_KEYBOARD_REPORT && event.length == 8) {
            tud_hid_report(REPORT_ID_KEYBOARD, event.payload, event.length);
        } else if (event.type == BRIDGE_MESSAGE_MOUSE_REPORT && event.length == 5) {
            tud_hid_report(REPORT_ID_MOUSE, event.payload, event.length);
        }
    }
}

esp_err_t hid_output_init(void)
{
    s_hid_queue = xQueueCreate(HID_QUEUE_LENGTH, sizeof(hid_event_t));
    if (s_hid_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG();
    usb_config.configuration_descriptor = s_configuration_descriptor;

    esp_err_t error = tinyusb_driver_install(&usb_config);
    if (error != ESP_OK) {
        vQueueDelete(s_hid_queue);
        s_hid_queue = NULL;
        return error;
    }

    if (xTaskCreate(
            hid_sender_task,
            "hid_sender",
            4096,
            NULL,
            8,
            NULL) != pdPASS) {
        ESP_LOGE(TAG, "无法创建 HID 发送任务");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t hid_output_submit(const bridge_frame_t *frame)
{
    if (frame == NULL || s_hid_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    hid_event_t event = {
        .type = frame->type,
        .length = frame->payload_length,
    };

    switch (frame->type) {
    case BRIDGE_MESSAGE_KEYBOARD_REPORT:
        if (frame->payload_length != 8) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(event.payload, frame->payload, 8);
        break;
    case BRIDGE_MESSAGE_MOUSE_REPORT:
        if (frame->payload_length != 5) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(event.payload, frame->payload, 5);
        break;
    case BRIDGE_MESSAGE_RELEASE_ALL:
        event.length = 0;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    return xQueueSend(s_hid_queue, &event, 0) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
}
