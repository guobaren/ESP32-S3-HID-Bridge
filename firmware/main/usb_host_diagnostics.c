#include "usb_host_diagnostics.h"

#include "esp_timer.h"

static dual_usb_host_diag_state_t s_usb_host_diag;

#define LOAD_DIAG(field) \
    __atomic_load_n(&s_usb_host_diag.field, __ATOMIC_ACQUIRE)

uint32_t dual_usb_host_diag_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

void dual_usb_host_diag_pcdet(bool connected_wake)
{
    dual_usb_host_diag_record_pcdet(&s_usb_host_diag, connected_wake,
                                    dual_usb_host_diag_now_ms());
}

void dual_usb_host_diag_discint(void)
{
    dual_usb_host_diag_record_discint(&s_usb_host_diag,
                                      dual_usb_host_diag_now_ms());
}

void dual_usb_host_diag_hub_wake(void)
{
    dual_usb_host_diag_record_hub_wake(&s_usb_host_diag);
}

void dual_usb_host_diag_connection_change(uint16_t status, uint16_t change)
{
    dual_usb_host_diag_record_connection_change(
        &s_usb_host_diag, status, change, dual_usb_host_diag_now_ms());
}

void dual_usb_host_diag_attempt(void)
{
    dual_usb_host_diag_record_attempt(&s_usb_host_diag,
                                      dual_usb_host_diag_now_ms());
}

void dual_usb_host_diag_stage(uint32_t stage)
{
    dual_usb_host_diag_set_stage(&s_usb_host_diag, stage,
                                 dual_usb_host_diag_now_ms());
}

void dual_usb_host_diag_failure(uint32_t stage, int32_t raw_error)
{
    dual_usb_host_diag_record_failure(&s_usb_host_diag, stage, raw_error,
                                      dual_usb_host_diag_now_ms());
}

void dual_usb_host_diag_success(void)
{
    dual_usb_host_diag_record_success(&s_usb_host_diag,
                                      dual_usb_host_diag_now_ms());
}

void dual_usb_host_diag_input(uint32_t now_ms)
{
    dual_usb_host_diag_record_input(&s_usb_host_diag, now_ms);
}

void dual_usb_host_diag_heartbeat(bool success)
{
    dual_usb_host_diag_record_heartbeat(&s_usb_host_diag, success);
}

void dual_usb_host_diag_collect(dual_stats_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    dual_usb_host_diag_backend_state_t backend = {0};
    const bool lifecycle_valid = dual_hid_host_cherryusb_diag_state(&backend) &&
        backend.snapshot_valid;
    const uint32_t pcdet_count = LOAD_DIAG(pcdet_count);
    const uint32_t discint_count = LOAD_DIAG(discint_count);
    const uint32_t hub_wake_count = LOAD_DIAG(hub_wake_count);
    const uint32_t connection_change_count = LOAD_DIAG(connection_change_count);
    const uint32_t enum_attempt_count = LOAD_DIAG(enum_attempt_count);
    const uint32_t enum_success_count = LOAD_DIAG(enum_success_count);
    const uint32_t enum_fail_count = LOAD_DIAG(enum_fail_count);
    const uint32_t input_count = LOAD_DIAG(input_count);
    const uint32_t input_last_ms = LOAD_DIAG(input_last_ms);
    const uint32_t heartbeat_ok_count = LOAD_DIAG(heartbeat_ok_count);
    const uint32_t heartbeat_fail_count = LOAD_DIAG(heartbeat_fail_count);
    uint32_t hprt = 0U;
    uint32_t gintsts = 0U;
    uint32_t gintmsk = 0U;
    const bool register_valid = dual_usb_host_diag_read_dwc2_registers(
        0U, &hprt, &gintsts, &gintmsk);

    uint32_t flags = DUAL_USB_HOST_DIAG_FLAGS_AVAILABLE |
        DUAL_USB_HOST_DIAG_FLAGS_BACKEND_CHERRYUSB |
        (UINT32_C(1) << DUAL_USB_HOST_DIAG_FLAGS_SCHEMA_SHIFT) |
        DUAL_USB_HOST_DIAG_FLAGS_CAP_DWC2_IRQ |
        DUAL_USB_HOST_DIAG_FLAGS_CAP_HUB_EVENTS |
        DUAL_USB_HOST_DIAG_FLAGS_CAP_INPUT_HEARTBEAT;
    if (register_valid) {
        flags |= DUAL_USB_HOST_DIAG_FLAGS_REGISTER_VALID;
    }
    if (lifecycle_valid) {
        flags |= DUAL_USB_HOST_DIAG_FLAGS_LIFECYCLE_VALID;
        flags |= backend.initialized ? DUAL_USB_HOST_DIAG_FLAGS_INITIALIZED : 0U;
        flags |= backend.stopping ? DUAL_USB_HOST_DIAG_FLAGS_STOPPING : 0U;
        flags |= backend.hport_present ? DUAL_USB_HOST_DIAG_FLAGS_HPORT_PRESENT : 0U;
        flags |= backend.connected ? DUAL_USB_HOST_DIAG_FLAGS_CONNECTED : 0U;
        flags |= backend.profile_ready ? DUAL_USB_HOST_DIAG_FLAGS_PROFILE_READY : 0U;
    }
    const bool input_valid = input_count != 0U;
    if (input_valid) {
        flags |= DUAL_USB_HOST_DIAG_FLAGS_INPUT_VALID;
    }

    const uint32_t connection_last = LOAD_DIAG(connection_last_status_change);
    const uint32_t enum_last_stage = LOAD_DIAG(enum_last_stage);
    const int32_t enum_last_raw_error = LOAD_DIAG(enum_last_raw_error);
    const uint32_t enum_last_ms = LOAD_DIAG(enum_last_ms);
    const uint32_t enum_last_fail_stage = LOAD_DIAG(enum_last_fail_stage);
    const int32_t enum_last_fail_raw_error = LOAD_DIAG(enum_last_fail_raw_error);
    const uint32_t enum_last_fail_ms = LOAD_DIAG(enum_last_fail_ms);
    const uint32_t current_ms = dual_usb_host_diag_now_ms();
    const uint32_t input_age_ms = input_valid
        ? current_ms - input_last_ms : UINT32_MAX;

    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_DIAG_FLAGS, flags);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_DEVICE_EPOCH,
        lifecycle_valid ? backend.device_epoch : 0U);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_HPRT,
        register_valid ? hprt : 0U);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_GINTSTS,
        register_valid ? gintsts : 0U);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_GINTMSK,
        register_valid ? gintmsk : 0U);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_PCDET_COUNT, pcdet_count);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_PCDET_WAKE_COUNT,
        LOAD_DIAG(pcdet_wake_count));
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_DISCINT_COUNT, discint_count);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_PCDET_LAST_MS,
        pcdet_count != 0U ? LOAD_DIAG(pcdet_last_ms) : 0U);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_DISCINT_LAST_MS,
        discint_count != 0U ? LOAD_DIAG(discint_last_ms) : 0U);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_HUB_WAKE_COUNT, hub_wake_count);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_CONNECTION_CHANGE_COUNT,
        connection_change_count);
    dual_stats_snapshot_add_counter(snapshot,
        DUAL_STAT_USB_HOST_CONNECTION_LAST_STATUS_CHANGE,
        connection_change_count != 0U ? connection_last : 0U);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_CONNECTION_LAST_MS,
        connection_change_count != 0U ? LOAD_DIAG(connection_last_ms) : 0U);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_ENUM_ATTEMPT_COUNT,
        enum_attempt_count);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_ENUM_SUCCESS_COUNT,
        enum_success_count);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_ENUM_FAIL_COUNT,
        enum_fail_count);
    const bool latest_event_valid = enum_attempt_count != 0U ||
        enum_fail_count != 0U || connection_change_count != 0U;
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_ENUM_LAST_STAGE,
        latest_event_valid ? enum_last_stage : 0U);
    dual_stats_snapshot_add_signed_counter(snapshot, DUAL_STAT_USB_HOST_ENUM_LAST_RAW_ERROR,
        latest_event_valid
            ? enum_last_raw_error : DUAL_USB_HOST_DIAG_NO_RAW_ERROR);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_ENUM_LAST_MS,
        latest_event_valid ? enum_last_ms : 0U);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_ENUM_LAST_FAIL_STAGE,
        enum_fail_count != 0U ? enum_last_fail_stage : 0U);
    dual_stats_snapshot_add_signed_counter(snapshot,
        DUAL_STAT_USB_HOST_ENUM_LAST_FAIL_RAW_ERROR,
        enum_fail_count != 0U ? enum_last_fail_raw_error :
            DUAL_USB_HOST_DIAG_NO_RAW_ERROR);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_ENUM_LAST_FAIL_MS,
        enum_fail_count != 0U ? enum_last_fail_ms : 0U);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_INPUT_COUNT, input_count);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_INPUT_AGE_MS,
        input_age_ms);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_HEARTBEAT_OK,
        heartbeat_ok_count);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_HEARTBEAT_FAIL,
        heartbeat_fail_count);
    const uint32_t pcdet_wake_count = LOAD_DIAG(pcdet_wake_count);
    dual_stats_snapshot_add_counter(snapshot, DUAL_STAT_USB_HOST_PCDET_WAKE_LAST_MS,
        pcdet_wake_count != 0U ? LOAD_DIAG(pcdet_wake_last_ms) : 0U);
}

#undef LOAD_DIAG
