#include "dual_status_led.h"

#include <stdint.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define DUAL_STATUS_LED_GPIO 48
#define DUAL_STATUS_LED_REFRESH_MS 10U
#define DUAL_STATUS_LED_RED 32U
#define DUAL_STATUS_LED_BASE_BLUE 32U
#define DUAL_STATUS_LED_GREEN 32U

static const char *TAG = "dual_status_led";
static led_strip_handle_t s_strip;
static TaskHandle_t s_task;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static dual_status_led_state_t s_state;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void notify_task(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

static void set_color(dual_status_led_color_t color)
{
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    switch (color) {
    case DUAL_STATUS_LED_COLOR_RED:
        red = DUAL_STATUS_LED_RED;
        break;
    case DUAL_STATUS_LED_COLOR_GREEN:
        green = DUAL_STATUS_LED_GREEN;
        break;
    case DUAL_STATUS_LED_COLOR_BLUE:
        blue = DUAL_STATUS_LED_BASE_BLUE;
        break;
    case DUAL_STATUS_LED_COLOR_FLASH_OFF:
        break;
    case DUAL_STATUS_LED_COLOR_OFF:
    default:
        break;
    }
    esp_err_t result = led_strip_set_pixel(s_strip, 0, red, green, blue);
    if (result == ESP_OK) {
        result = led_strip_refresh(s_strip);
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "状态灯刷新失败：%s", esp_err_to_name(result));
    }
}

static void status_led_task(void *argument)
{
    (void)argument;
    dual_status_led_color_t last_color = DUAL_STATUS_LED_COLOR_OFF;
    bool first_update = true;
    const TickType_t wait_ticks = pdMS_TO_TICKS(DUAL_STATUS_LED_REFRESH_MS);
    while (true) {
        dual_status_led_state_t snapshot;
        portENTER_CRITICAL(&s_state_lock);
        snapshot = s_state;
        portEXIT_CRITICAL(&s_state_lock);
        const dual_status_led_color_t color =
            dual_status_led_logic_color(&snapshot, now_ms());
        if (first_update || color != last_color) {
            set_color(color);
            last_color = color;
            first_update = false;
        }
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

esp_err_t dual_status_led_init(void)
{
    if (s_strip != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    dual_status_led_logic_init(&s_state);
    const led_strip_config_t strip_config = {
        .strip_gpio_num = DUAL_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
    };
    esp_err_t result = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (result != ESP_OK) {
        return result;
    }
    result = xTaskCreate(status_led_task, "dual_status_led", 2048, NULL, 4, &s_task) == pdPASS
        ? ESP_OK
        : ESP_ERR_NO_MEM;
    if (result != ESP_OK) {
        led_strip_del(s_strip);
        s_strip = NULL;
        return result;
    }
    ESP_LOGI(TAG, "板载WS2812B状态灯已启用，GPIO=%d", DUAL_STATUS_LED_GPIO);
    return ESP_OK;
}

void dual_status_led_set_role(dual_status_led_role_t role)
{
    portENTER_CRITICAL(&s_state_lock);
    dual_status_led_logic_set_role(&s_state, role);
    portEXIT_CRITICAL(&s_state_lock);
    notify_task();
}

void dual_status_led_set_pc_mounted(bool mounted)
{
    portENTER_CRITICAL(&s_state_lock);
    dual_status_led_logic_set_pc_mounted(&s_state, mounted);
    portEXIT_CRITICAL(&s_state_lock);
    notify_task();
}

void dual_status_led_set_host_mouse_ready(bool ready)
{
    portENTER_CRITICAL(&s_state_lock);
    dual_status_led_logic_set_host_mouse_ready(&s_state, ready);
    portEXIT_CRITICAL(&s_state_lock);
    notify_task();
}

void dual_status_led_set_peer_connected(bool connected)
{
    portENTER_CRITICAL(&s_state_lock);
    dual_status_led_logic_set_peer_connected(&s_state, connected);
    portEXIT_CRITICAL(&s_state_lock);
    notify_task();
}

void dual_status_led_set_peer_usb_ready(bool ready)
{
    portENTER_CRITICAL(&s_state_lock);
    dual_status_led_logic_set_peer_usb_ready(&s_state, ready);
    portEXIT_CRITICAL(&s_state_lock);
    notify_task();
}

void dual_status_led_set_flow_error(bool failed)
{
    portENTER_CRITICAL(&s_state_lock);
    dual_status_led_logic_set_flow_error(&s_state, failed);
    portEXIT_CRITICAL(&s_state_lock);
    notify_task();
}

void dual_status_led_notify_software_success(uint32_t timestamp_ms)
{
    portENTER_CRITICAL(&s_state_lock);
    dual_status_led_logic_notify_software_success(&s_state, timestamp_ms);
    portEXIT_CRITICAL(&s_state_lock);
    notify_task();
}
