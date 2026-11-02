#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_role_logic.h"

#define DUAL_STATUS_LED_ROLE_NONE 0
#define USB_PROBE_NONE 0
static bool s_host_started, s_host_cleanup_pending, s_role_reprobe_pending;
static bool s_manual_profile, s_profile_offer_valid, s_profile_reuse_pending;
static uint8_t s_peer_role_claim;
static int s_profile_receiver, s_probe_mode;
static int64_t s_profile_started_us, s_role_retry_after_us;
static dual_role_watch_t s_role_watch;
static char calls[32], failed_stage;
static size_t call_count;

static esp_err_t stage(char value)
{
    calls[call_count++] = value;
    calls[call_count] = 0;
    return value == failed_stage ? ESP_FAIL : ESP_OK;
}
static esp_err_t dual_pc_hid_disable_reconfigure(void) { return stage('W'); }
static esp_err_t dual_hid_host_stop_for_role_probe(void) { return stage('H'); }
static esp_err_t dual_pc_hid_prepare_for_profile(void) { return stage('D'); }
static esp_err_t dual_uart0_control_start(int role, void *a, void *b, void *c)
{
    assert(role == DUAL_ROLE_UNRESOLVED && a == NULL && b == NULL && c == NULL);
    return stage('U');
}
static esp_err_t dual_uart1_begin_role_probe(void) { return stage('C'); }
static esp_err_t start_device_probe(void) { return stage('P'); }
static void on_profile_published(void) {}
static void hid_profile_receiver_init(int *receiver, void (*callback)(void), void *context)
{
    assert(receiver == context && callback == on_profile_published);
    *receiver = 0;
}
static void dual_status_led_set_role(int role) { assert(role == DUAL_STATUS_LED_ROLE_NONE); }

/* PRODUCTION_RESTART_ROLE_PROBE */

static void setup(char failure)
{
    memset(calls, 0, sizeof(calls));
    call_count = 0;
    failed_stage = failure;
    s_host_started = true;
    s_host_cleanup_pending = false;
    s_role_reprobe_pending = true;
    s_manual_profile = s_profile_offer_valid = s_profile_reuse_pending = true;
    s_peer_role_claim = DUAL_ROLE_MOUSE_HOST;
    s_profile_receiver = 9;
    s_role_retry_after_us = s_profile_started_us = 123;
    s_role_watch = (dual_role_watch_t){123, true};
}

int main(void)
{
    const char *sequences[] = {"W", "WH", "WHD", "WHDU", "WHDUC"};
    const char failures[] = {'W', 'H', 'D', 'U', 'C'};
    for (size_t i = 0; i < sizeof(failures); ++i) {
        setup(failures[i]);
        assert(restart_role_probe() == ESP_FAIL);
        assert(strcmp(calls, sequences[i]) == 0);
        assert(s_role_reprobe_pending && s_manual_profile);
        assert(s_profile_receiver == 9);
        if (failures[i] == 'H') assert(s_host_cleanup_pending);
        if (i >= 2) assert(!s_host_started && !s_host_cleanup_pending);
        printf("生产角色清理函数：阶段 %c 失败，后续步骤未执行：PASS\n", failures[i]);
    }
    setup(0);
    assert(restart_role_probe() == ESP_OK);
    assert(strcmp(calls, "WHDUCP") == 0);
    assert(!s_role_reprobe_pending && !s_manual_profile && !s_host_started);
    assert(!s_profile_offer_valid && !s_profile_reuse_pending && s_profile_receiver == 0);
    assert(s_role_watch.absent_since_us == 0 && !s_role_watch.usb_seen);
    assert(s_role_retry_after_us == 0 && s_profile_started_us == 0);
    /* 清理失败后的重试必须保留 Host 资源所有权，成功后才开放 Device 探测。 */
    setup('H');
    assert(restart_role_probe() == ESP_FAIL && s_host_cleanup_pending);
    failed_stage = 0;
    call_count = 0;
    assert(restart_role_probe() == ESP_OK && strcmp(calls, "WHDUCP") == 0);
    setup('P');
    assert(restart_role_probe() == ESP_FAIL && !s_role_reprobe_pending);
    assert(!s_manual_profile && strcmp(calls, "WHDUCP") == 0);
    puts("生产角色清理函数：完整顺序、重试、首次探测失败退出旧会话：PASS");
    return 0;
}
