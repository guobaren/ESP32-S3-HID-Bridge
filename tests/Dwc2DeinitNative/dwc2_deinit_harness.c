#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_USBHOST_MAX_BUS 2U
#define USB_ERR_INVAL 22
#define USB_ERR_TIMEOUT 14
#define ESP_OK 0
#define USB_OTG_HCCHAR_CHENA (1UL << 31)
#define USB_OTG_HCCHAR_CHDIS (1UL << 30)
#define USB_OTG_HCCHAR_EPDIR (1UL << 15)
#define USB_OTG_GRSTCTL_AHBIDL (1UL << 31)
#define USB_OTG_GAHBCFG_GINT (1UL << 0)

typedef int esp_err_t;

typedef struct {
    volatile uint32_t HCCHAR;
} DWC2_HostChannelTypeDef;

typedef struct {
    volatile uint32_t GAHBCFG;
    volatile uint32_t GINTMSK;
    volatile uint32_t GRSTCTL;
} DWC2_GlobalTypeDef;

struct usbh_bus {
    struct {
        uint8_t hcd_id;
        uintptr_t reg_base;
    } hcd;
};

static DWC2_GlobalTypeDef g_global;
static DWC2_HostChannelTypeDef g_channels[16];
static uint32_t g_hprt;
static bool s_dwc2_registers_ready[CONFIG_USBHOST_MAX_BUS];
static unsigned g_channel_count;
static unsigned g_reset_calls;
static unsigned g_irq_disable_calls;
static unsigned g_sleep_ms;
static int g_reset_result;
static bool g_reset_clears_channels;
static bool g_reset_sets_ahbidle;
static int g_irq_disable_result;
static uint32_t g_reset_seen_hcchar[16];
static char g_log[2048];

#define USB_OTG_GLB ((DWC2_GlobalTypeDef *)(bus->hcd.reg_base))
#define USB_OTG_HC(i) (&g_channels[(i)])
#define USB_OTG_HPRT g_hprt
#define USB_LOG_ERR(...) test_log(__VA_ARGS__)
#define USB_LOG_INFO(...) test_log(__VA_ARGS__)

static void test_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const size_t used = strlen(g_log);
    if (used < sizeof(g_log)) {
        (void)vsnprintf(g_log + used, sizeof(g_log) - used, format, args);
    }
    va_end(args);
}

static const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "TEST_ERROR";
}

static esp_err_t dual_cherryusb_host_low_level_disable_irq(struct usbh_bus *bus)
{
    (void)bus;
    ++g_irq_disable_calls;
    return g_irq_disable_result;
}

static void usb_osal_msleep(uint32_t milliseconds)
{
    g_sleep_ms += milliseconds;
}

static int dwc2_reset(struct usbh_bus *bus)
{
    ++g_reset_calls;
    for (unsigned channel = 0U; channel < g_channel_count; ++channel) {
        g_reset_seen_hcchar[channel] = g_channels[channel].HCCHAR;
    }
    if (g_reset_result != 0) {
        return g_reset_result;
    }
    if (g_reset_clears_channels) {
        for (unsigned channel = 0U; channel < g_channel_count; ++channel) {
            g_channels[channel].HCCHAR &= ~USB_OTG_HCCHAR_CHENA;
        }
    }
    USB_OTG_GLB->GRSTCTL = g_reset_sets_ahbidle ? USB_OTG_GRSTCTL_AHBIDL : 0U;
    return 0;
}

/* __PRODUCTION_HELPER__ */

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "FAIL: %s\n", message); \
            return false; \
        } \
    } while (0)

static struct usbh_bus make_bus(void)
{
    struct usbh_bus bus = { { 0U, (uintptr_t)&g_global } };
    return bus;
}

static void reset_fixture(unsigned channels)
{
    memset(&g_global, 0, sizeof(g_global));
    memset(g_channels, 0, sizeof(g_channels));
    memset(g_reset_seen_hcchar, 0, sizeof(g_reset_seen_hcchar));
    memset(g_log, 0, sizeof(g_log));
    memset(s_dwc2_registers_ready, 0, sizeof(s_dwc2_registers_ready));
    g_channel_count = channels;
    g_reset_calls = 0U;
    g_irq_disable_calls = 0U;
    g_sleep_ms = 0U;
    g_reset_result = 0;
    g_reset_clears_channels = true;
    g_reset_sets_ahbidle = true;
    g_irq_disable_result = ESP_OK;
    g_hprt = 0U;
    s_dwc2_registers_ready[0] = true;
}

static bool case_idle_channels_are_untouched(void)
{
    reset_fixture(3U);
    struct usbh_bus bus = make_bus();
    g_channels[0].HCCHAR = USB_OTG_HCCHAR_EPDIR | 0x1234U;
    g_channels[1].HCCHAR = 0x00405678U;
    g_channels[2].HCCHAR = USB_OTG_HCCHAR_EPDIR | 0x00001234U;
    const uint32_t before[3] = {
        g_channels[0].HCCHAR, g_channels[1].HCCHAR, g_channels[2].HCCHAR
    };

    CHECK(dwc2_deinit_halt_channels(&bus, 0U, g_channel_count) == 0,
          "idle channels should finish without reset");
    CHECK(g_reset_calls == 0U, "idle channels must not trigger reset");
    for (unsigned channel = 0U; channel < g_channel_count; ++channel) {
        CHECK(g_channels[channel].HCCHAR == before[channel],
              "idle channel HCCHAR, including direction, must remain unchanged");
    }
    (void)puts("PASS idle channels remain untouched and are not enabled");
    return true;
}

static bool case_stuck_channel_resets_and_verifies(void)
{
    reset_fixture(2U);
    struct usbh_bus bus = make_bus();
    g_global.GAHBCFG = USB_OTG_GAHBCFG_GINT | 0x40U;
    g_global.GINTMSK = 0xFFFFFFFFU;
    g_channels[0].HCCHAR = USB_OTG_HCCHAR_CHENA | USB_OTG_HCCHAR_EPDIR | 0x1234U;
    g_channels[1].HCCHAR = USB_OTG_HCCHAR_EPDIR | 0x5678U;

    CHECK(dwc2_deinit_halt_channels(&bus, 0U, g_channel_count) == 0,
          "stuck channel should recover when reset clears it");
    CHECK(g_reset_calls == 1U, "stuck channel recovery must reset once");
    CHECK((g_reset_seen_hcchar[0] & (USB_OTG_HCCHAR_CHENA | USB_OTG_HCCHAR_CHDIS)) ==
              (USB_OTG_HCCHAR_CHENA | USB_OTG_HCCHAR_CHDIS),
          "halt request must preserve CHENA while setting CHDIS");
    CHECK((g_reset_seen_hcchar[0] & USB_OTG_HCCHAR_EPDIR) != 0U,
          "halt request must preserve EPDIR");
    CHECK(g_channels[1].HCCHAR == (USB_OTG_HCCHAR_EPDIR | 0x5678U),
          "idle channel must not be re-enabled or altered during reset");
    CHECK((g_global.GAHBCFG & USB_OTG_GAHBCFG_GINT) == 0U && g_global.GINTMSK == 0U,
          "global IRQ and mask must remain closed after reset");
    CHECK(g_irq_disable_calls == 1U && g_sleep_ms >= 1U,
          "recovery must disable IRQ again after a one millisecond delay");
    CHECK(strstr(g_log, "DWC2_DEINIT_RECOVER stage=channel_halt_reset raw=0") != NULL,
          "successful recovery stage must be logged");
    (void)puts("PASS stuck channel requests CHDIS without changing fields, resets, and verifies");
    return true;
}

static bool case_reset_failure_keeps_failure(void)
{
    reset_fixture(1U);
    struct usbh_bus bus = make_bus();
    g_channels[0].HCCHAR = USB_OTG_HCCHAR_CHENA | USB_OTG_HCCHAR_EPDIR;
    g_reset_result = -7;

    CHECK(dwc2_deinit_halt_channels(&bus, 0U, g_channel_count) == -USB_ERR_TIMEOUT,
          "reset failure must return timeout");
    CHECK(g_reset_calls == 1U && g_irq_disable_calls == 0U,
          "reset failure must stop before final verification");
    CHECK(strstr(g_log, "stage=channel_halt_reset raw=-14") != NULL,
          "reset failure must log the original stage and timeout");
    (void)puts("PASS reset failure returns timeout before teardown can continue");
    return true;
}

static bool case_reset_must_leave_ahb_idle(void)
{
    reset_fixture(1U);
    struct usbh_bus bus = make_bus();
    g_channels[0].HCCHAR = USB_OTG_HCCHAR_CHENA | USB_OTG_HCCHAR_EPDIR;
    g_reset_sets_ahbidle = false;

    CHECK(dwc2_deinit_halt_channels(&bus, 0U, g_channel_count) == -USB_ERR_TIMEOUT,
          "reset success without AHBIDL must fail closed");
    CHECK(g_reset_calls == 1U && g_irq_disable_calls == 1U,
          "AHBIDL failure must be detected during final verification");
    CHECK(strstr(g_log, "reason=ahb_not_idle") != NULL,
          "AHBIDL failure reason must be logged");
    (void)puts("PASS reset success without AHBIDL fails closed");
    return true;
}

static bool case_reset_must_clear_all_channels(void)
{
    reset_fixture(2U);
    struct usbh_bus bus = make_bus();
    g_channels[0].HCCHAR = USB_OTG_HCCHAR_CHENA | USB_OTG_HCCHAR_EPDIR;
    g_channels[1].HCCHAR = USB_OTG_HCCHAR_CHENA | 0x3456U;
    g_reset_clears_channels = false;

    CHECK(dwc2_deinit_halt_channels(&bus, 0U, g_channel_count) == -USB_ERR_TIMEOUT,
          "reset success with active channel must fail closed");
    CHECK(g_reset_calls == 1U && strstr(g_log, "reason=channel_active") != NULL,
          "active channel final verification must fail and log");
    (void)puts("PASS reset success with a still-active channel fails closed");
    return true;
}

static bool case_irq_reclose_failure_fails_closed(void)
{
    reset_fixture(1U);
    struct usbh_bus bus = make_bus();
    g_channels[0].HCCHAR = USB_OTG_HCCHAR_CHENA | USB_OTG_HCCHAR_EPDIR;
    g_irq_disable_result = -5;

    CHECK(dwc2_deinit_halt_channels(&bus, 0U, g_channel_count) == -USB_ERR_TIMEOUT,
          "post-reset IRQ disable failure must fail closed");
    CHECK(g_irq_disable_calls == 1U &&
              strstr(g_log, "stage=channel_halt_final_irq_disable") != NULL,
          "post-reset IRQ disable failure must have a distinct stage");
    (void)puts("PASS post-reset IRQ re-disable failure fails closed");
    return true;
}

static bool case_multiple_active_channels_reset_once(void)
{
    reset_fixture(4U);
    struct usbh_bus bus = make_bus();
    g_channels[0].HCCHAR = USB_OTG_HCCHAR_CHENA | USB_OTG_HCCHAR_EPDIR | 0x111U;
    g_channels[1].HCCHAR = USB_OTG_HCCHAR_EPDIR | 0x222U;
    g_channels[2].HCCHAR = USB_OTG_HCCHAR_CHENA | 0x333U;
    g_channels[3].HCCHAR = USB_OTG_HCCHAR_CHENA | USB_OTG_HCCHAR_EPDIR | 0x444U;

    CHECK(dwc2_deinit_halt_channels(&bus, 0U, g_channel_count) == 0,
          "multiple stuck channels should recover with one controller reset");
    CHECK(g_reset_calls == 1U, "multiple stuck channels must reset only once");
    CHECK((g_reset_seen_hcchar[0] & USB_OTG_HCCHAR_CHDIS) != 0U,
          "the timed-out active channel must receive a CHDIS request before reset");
    CHECK(g_channels[1].HCCHAR == (USB_OTG_HCCHAR_EPDIR | 0x222U),
          "idle channel must remain untouched when other channels reset");
    (void)puts("PASS multiple active channels use one reset and leave idle channels unchanged");
    return true;
}

int main(void)
{
    if (!case_idle_channels_are_untouched() ||
        !case_stuck_channel_resets_and_verifies() ||
        !case_reset_failure_keeps_failure() ||
        !case_reset_must_leave_ahb_idle() ||
        !case_reset_must_clear_all_channels() ||
        !case_irq_reclose_failure_fails_closed() ||
        !case_multiple_active_channels_reset_once()) {
        return 1;
    }
    (void)puts("PASS all DWC2 deinit native assertions");
    return 0;
}
