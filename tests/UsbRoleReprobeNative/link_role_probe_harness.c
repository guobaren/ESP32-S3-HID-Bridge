#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include "link_recovery_logic.h"

#define LINK_UART 1
#define LINK_PROFILE_PHASE_IDLE 0
#define pdMS_TO_TICKS(value) (value)
#define ESP_LOGI(...) ((void)0)
static void *s_rx_task = (void *)1, *s_tx_task = (void *)2, *s_uart_event_queue = (void *)3;
static bool s_role_probe_requested, s_role_probe_rx_parked, s_role_probe_tx_parked;
static uint8_t s_role, s_peer_role, s_usb_state, s_peer_usb_state;
static uint32_t s_generation, s_next_sequence, s_next_flow_id, s_mouse_connection_id;
static bool s_peer_online, s_peer_generation_initialized, s_peer_sequence_initialized, s_role_ack_logged;
static int64_t s_last_peer_rx_us, s_recovery_started_us;
static bool s_gone_completed_valid, s_gone_completed_accepted;
static uint32_t s_gone_completed_event_id, s_gone_completed_peer_generation;
static uint32_t s_profile_length, s_profile_crc32, s_profile_transfer_id, s_profile_offset;
static bool s_profile_pending, s_profile_peer_generation_valid, s_profile_waiting_host;
static uint8_t s_profile_phase;
static int64_t s_profile_last_commit_us, s_profile_wait_host_last_probe_us;
static bool s_profile_commit_receipt_seen, s_profile_request_cleanup_ready;
static bool s_profile_request_waiting_offer, s_profile_request_waiting_device, s_profile_request_offer_seen;
static int64_t s_profile_request_accepted_us;
static uint32_t s_profile_retransmit_attempts, s_request_offer_retries;
static link_flow_t s_request_flow, s_offer_flow, s_commit_flow, s_gone_flow;
#define DUAL_USB_STATE_WAITING 0
static int64_t fake_now;
static bool allow_park, allow_write, allow_drain;
static unsigned writes, queue_resets, delays;
static int64_t esp_timer_get_time(void) { return fake_now; }
static void notify_tx_task(void) {}
static void vTaskDelay(unsigned ticks)
{
    (void)ticks;
    fake_now += 100000;
    ++delays;
    if (!s_role_probe_requested) {
        s_role_probe_rx_parked = s_role_probe_tx_parked = false;
    } else if (allow_park) {
        s_role_probe_rx_parked = true;
        if (delays >= 2) s_role_probe_tx_parked = true;
    }
}
static bool send_status_frame(uint8_t type, const uint8_t *payload, uint8_t length)
{
    assert(s_role_probe_tx_parked && s_role_probe_rx_parked && s_role_probe_requested);
    assert(queue_resets == 0 && s_generation == 77U);
    assert(type == DUAL_MESSAGE_PHYSICAL_RELEASE || type == DUAL_MESSAGE_SOFTWARE_RELEASE);
    if (type == DUAL_MESSAGE_PHYSICAL_RELEASE) assert(payload && length == 1U);
    else assert(payload == NULL && length == 0);
    ++writes;
    return allow_write;
}
static esp_err_t uart_wait_tx_done(int uart, unsigned ticks)
{
    (void)ticks;
    assert(uart == LINK_UART && queue_resets == 0 && writes >= 2);
    return allow_drain ? ESP_OK : ESP_ERR_TIMEOUT;
}
static void reset_tx_queues(void)
{
    assert(s_role_probe_rx_parked && s_role_probe_tx_parked && writes >= 2);
    ++queue_resets;
}
static esp_err_t uart_flush_input(int uart) { assert(uart == LINK_UART); return ESP_OK; }
static void xQueueReset(void *queue) { assert(queue == s_uart_event_queue); }
static uint32_t esp_random(void) { return 99U; }
static void dual_status_led_set_peer_connected(bool value) { assert(!value); }
static void dual_status_led_set_peer_usb_ready(bool value) { assert(!value); }
static void dual_status_led_set_host_profile_ready(bool value) { assert(!value); }

/* PRODUCTION_BEGIN_ROLE_PROBE */

static void setup(void)
{
    s_role = DUAL_ROLE_MOUSE_HOST;
    s_peer_role = DUAL_ROLE_PC_DEVICE;
    s_generation = 77U;
    s_role_probe_requested = s_role_probe_rx_parked = s_role_probe_tx_parked = false;
    s_peer_online = s_peer_generation_initialized = s_peer_sequence_initialized = true;
    s_role_ack_logged = s_gone_completed_valid = s_profile_pending = true;
    s_profile_request_cleanup_ready = s_profile_request_waiting_device = true;
    s_profile_length = 100U;
    s_next_sequence = 44U;
    s_request_flow.state = LINK_FLOW_PENDING;
    s_commit_flow.state = LINK_FLOW_PENDING;
    allow_park = allow_write = allow_drain = true;
    fake_now = 1;
    writes = queue_resets = delays = 0;
}

int main(void)
{
    setup();
    allow_park = false;
    assert(dual_uart1_begin_role_probe() == ESP_ERR_TIMEOUT);
    assert(s_role_probe_requested && s_generation == 77U && queue_resets == 0 && writes == 0);
    puts("生产UART会话重探测：收发任务未交出所有权时超时且不改 generation：PASS");
    setup();
    allow_write = false;
    assert(dual_uart1_begin_role_probe() == ESP_FAIL);
    assert(s_role_probe_requested && s_generation == 77U && queue_resets == 0);
    puts("生产UART会话重探测：最终释放写入失败时保留暂停：PASS");
    setup();
    allow_drain = false;
    assert(dual_uart1_begin_role_probe() == ESP_ERR_TIMEOUT);
    assert(s_role_probe_requested && s_generation == 77U && queue_resets == 0);
    allow_drain = true;
    assert(dual_uart1_begin_role_probe() == ESP_OK);
    assert(!s_role_probe_requested && !s_role_probe_rx_parked && !s_role_probe_tx_parked);
    assert(s_generation == 99U && s_role == 0 && s_peer_role == 0 && !s_peer_online);
    assert(!s_profile_pending && !s_profile_request_cleanup_ready && !s_profile_request_waiting_device);
    assert(!s_gone_completed_valid && !s_peer_sequence_initialized && !s_role_ack_logged);
    assert(s_profile_length == 0 && s_next_sequence == 0 && queue_resets == 1);
    assert(s_request_flow.state == LINK_FLOW_IDLE && s_commit_flow.state == LINK_FLOW_IDLE);
    puts("生产UART会话重探测：释放排空后才清队换代，超时重试与旧事务作废：PASS");
    return 0;
}
