/* 独立 USB 全速 CDC / WinUSB 吞吐探针：没有 HID、Wi-Fi、移动或串口数据转发。 */
#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "esp_check.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#if !CONFIG_USB_PROBE_VENDOR
#include "tinyusb_cdc_acm.h"
#endif
#include "tusb.h"

#define MAGIC 0x31504255U
#define BLOCK 4096U
typedef struct { uint32_t magic, type, sequence, length, crc; } header_t;
typedef struct { uint64_t bytes, elapsed_us, crc_errors, sequence_errors, blocks; } stats_t;
_Static_assert(sizeof(header_t) == 20, "协议头必须为20字节");
_Static_assert(sizeof(stats_t) == 40, "统计必须为40字节");
static uint8_t payload[BLOCK];
static stats_t down;
static uint32_t expected;
static int64_t started;
static TaskHandle_t io_task;

#if CONFIG_USB_PROBE_VENDOR
#if CFG_TUD_API_V0_19_COMPAT
void tud_vendor_rx_cb(uint8_t index, const uint8_t *buffer, uint16_t length)
#else
void tud_vendor_rx_cb(uint8_t index, const uint8_t *buffer, uint32_t length)
#endif
{
    (void)index; (void)buffer; (void)length;
    if (io_task != NULL) xTaskNotifyGive(io_task);
}
void tud_vendor_tx_cb(uint8_t index, uint32_t length)
{
    (void)index; (void)length;
    if (io_task != NULL) xTaskNotifyGive(io_task);
}
#else
static void received(int interface, cdcacm_event_t *event)
{
    (void)interface;
    (void)event;
    /* TinyUSB任务通知应用任务，避免100Hz系统中10ms轮询限制USB OUT。 */
    if (io_task != NULL) xTaskNotifyGive(io_task);
}
#endif

static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
#if CONFIG_USB_PROBE_VENDOR
    .bcdUSB = 0x0210, .bDeviceClass = 0, .bDeviceSubClass = 0, .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64, .idVendor = 0x303a, .idProduct = 0x400a,
#else
    .bcdUSB = 0x0200, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64, .idVendor = 0x303a, .idProduct = 0x4009,
#endif
#if CONFIG_USB_PROBE_VENDOR
    .bcdDevice = 0x0101,
#else
    .bcdDevice = 0x0100,
#endif
    .iManufacturer = 1, .iProduct = 2,
    .iSerialNumber = 3, .bNumConfigurations = 1
};
#if CONFIG_USB_PROBE_VENDOR
static const char *strings[] = { (const char[]){0x09, 0x04},
    "USB Probe", "S3 WinUSB Bandwidth Probe", "USBPROBE-VENDOR1", "Data" };
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(0, 4, 0x01, 0x81, 64)
};
#else
static const char *strings[] = { (const char[]){0x09, 0x04},
    "USB Probe", "S3 USB Bandwidth Probe", "USBPROBE-V1", "Data" };
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64)
};
#endif

static bool read_all(void *data, size_t length)
{
    uint8_t *p = data;
    int64_t last = esp_timer_get_time();
    while (length) {
        size_t n = 0;
        if (!tud_mounted() || esp_timer_get_time() - last > 30000000) return false;
#if CONFIG_USB_PROBE_VENDOR
        n = tud_vendor_read(p, length);
#else
        if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, p, length, &n) != ESP_OK) return false;
#endif
        if (n) { p += n; length -= n; last = esp_timer_get_time(); }
        else (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
    return true;
}

static bool write_all(const void *data, size_t length)
{
    const uint8_t *p = data;
    int64_t last = esp_timer_get_time();
    while (length) {
        if (!tud_mounted() || esp_timer_get_time() - last > 30000000) return false;
#if CONFIG_USB_PROBE_VENDOR
        size_t n = tud_vendor_write(p, length);
#else
        size_t n = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, p, length);
#endif
        if (n) { p += n; length -= n; last = esp_timer_get_time(); }
#if CONFIG_USB_PROBE_VENDOR
        (void)tud_vendor_write_flush();
        if (!n) (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
#else
        (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        if (!n) vTaskDelay(1);
#endif
    }
    return true;
}

static bool send_packet(uint32_t type, uint32_t seq, const void *data, uint32_t len)
{
    header_t h = {MAGIC, type, seq, len, esp_rom_crc32_le(0, data, len)};
    return write_all(&h, sizeof(h)) && (!len || write_all(data, len));
}

static void transmit(uint32_t duration_ms)
{
    stats_t s = {0};
    uint32_t seq = 0;
    int64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < (int64_t)duration_ms * 1000) {
        for (uint32_t i = 0; i < BLOCK; i++) payload[i] = (uint8_t)(seq + i);
        if (!send_packet(6, seq++, payload, BLOCK)) return;
        s.bytes += BLOCK; s.blocks++;
    }
    s.elapsed_us = esp_timer_get_time() - start;
    (void)send_packet(5, 0, &s, sizeof(s));
}

void app_main(void)
{
#if CONFIG_USB_PROBE_VENDOR
    extern void usb_probe_vendor_descriptors_link(void);
    usb_probe_vendor_descriptors_link();
#endif
    io_task = xTaskGetCurrentTaskHandle();
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &device;
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    cfg.descriptor.full_speed_config = configuration;
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
#if !CONFIG_USB_PROBE_VENDOR
    tinyusb_config_cdcacm_t cdc = {
        .cdc_port = TINYUSB_CDC_ACM_0, .callback_rx = received
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc));
#endif
    for (;;) {
        if (!tud_mounted()) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        header_t h;
        if (!read_all(&h, sizeof(h))) continue;
        if (h.magic != MAGIC || h.length > BLOCK) continue;
        if (h.length && !read_all(payload, h.length)) continue;
        bool valid = h.crc == esp_rom_crc32_le(0, payload, h.length);
        if (h.type == 1 && valid) {
#if CONFIG_USB_PROBE_VENDOR
            const char hello[] = "USB_BANDWIDTH_PROBE_V1;FS;VENDOR64";
#else
            const char hello[] = "USB_BANDWIDTH_PROBE_V1;FS;BULK64";
#endif
            (void)send_packet(1, 0, hello, sizeof(hello) - 1);
        } else if (h.type == 10 && valid && h.length == 0) {
            memset(&down, 0, sizeof(down)); expected = 0; started = esp_timer_get_time();
            (void)send_packet(10, 0, NULL, 0);
        } else if (h.type == 2) {
            if (!valid) down.crc_errors++;
            if (h.sequence != expected || h.length != BLOCK) down.sequence_errors++;
            expected = h.sequence + 1;
            down.bytes += h.length; down.blocks++;
        } else if (h.type == 3 && valid && h.length == 0) {
            down.elapsed_us = esp_timer_get_time() - started;
            (void)send_packet(5, 0, &down, sizeof(down));
        } else if (h.type == 4 && valid && h.length == 4) {
            uint32_t duration;
            memcpy(&duration, payload, 4);
            if (duration >= 1000 && duration <= 60000) transmit(duration);
        }
    }
}
