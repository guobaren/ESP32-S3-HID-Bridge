#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define STATUS_LED_REFRESH_INTERVAL_MS 100
#define STATUS_LED_DISCONNECTED_BLINK_INTERVAL_MS 500

static const char *TAG = "status_led";
static led_strip_handle_t s_strip;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_usb_connected;
static bool s_ble_connected;

static void set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_set_pixel(s_strip, 0, red, green, blue));
    ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_refresh(s_strip));
}

static void status_led_task(void *argument)
{
    (void)argument;
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_blink_change = last_wake;
    bool red_visible = true;

    while (true) {
        bool usb_connected;
        bool ble_connected;
        portENTER_CRITICAL(&s_state_lock);
        usb_connected = s_usb_connected;
        ble_connected = s_ble_connected;
        portEXIT_CRITICAL(&s_state_lock);

        if (usb_connected) {
            set_color(0, 32, 0);
            red_visible = true;
            last_blink_change = xTaskGetTickCount();
        } else if (ble_connected) {
            set_color(0, 0, 32);
            red_visible = true;
            last_blink_change = xTaskGetTickCount();
        } else {
            TickType_t now = xTaskGetTickCount();
            if (now - last_blink_change >= pdMS_TO_TICKS(STATUS_LED_DISCONNECTED_BLINK_INTERVAL_MS)) {
                red_visible = !red_visible;
                last_blink_change = now;
            }
            set_color(red_visible ? 32 : 0, 0, 0);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(STATUS_LED_REFRESH_INTERVAL_MS));
    }
}

esp_err_t status_led_init(void)
{
#if !CONFIG_HID_BRIDGE_STATUS_LED_ENABLE
    return ESP_OK;
#else
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_HID_BRIDGE_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
    };
    esp_err_t result = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (result != ESP_OK) {
        return result;
    }
    if (xTaskCreate(status_led_task, "status_led", 2048, NULL, 4, NULL) != pdPASS) {
        led_strip_del(s_strip);
        s_strip = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "RGB 状态指示灯已启用，GPIO=%d", CONFIG_HID_BRIDGE_STATUS_LED_GPIO);
    return ESP_OK;
#endif
}

void status_led_set_usb_connected(bool connected)
{
#if CONFIG_HID_BRIDGE_STATUS_LED_ENABLE
    portENTER_CRITICAL(&s_state_lock);
    s_usb_connected = connected;
    portEXIT_CRITICAL(&s_state_lock);
#else
    (void)connected;
#endif
}

void status_led_set_ble_connected(bool connected)
{
#if CONFIG_HID_BRIDGE_STATUS_LED_ENABLE
    portENTER_CRITICAL(&s_state_lock);
    s_ble_connected = connected;
    portEXIT_CRITICAL(&s_state_lock);
#else
    (void)connected;
#endif
}
