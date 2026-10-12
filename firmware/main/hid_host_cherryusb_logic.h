#ifndef HID_HOST_CHERRYUSB_LOGIC_H
#define HID_HOST_CHERRYUSB_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHERRYUSB_CONTROL_SETUP_LENGTH 8U
#define CHERRYUSB_HEARTBEAT_HISTORY_CAPACITY 8U

typedef enum {
    CHERRY_CONTROL_STAGE_OK = 0,
    CHERRY_CONTROL_STAGE_LIFECYCLE_LOCK,
    CHERRY_CONTROL_STAGE_EP0_LOCK,
    CHERRY_CONTROL_STAGE_DEADLINE,
    CHERRY_CONTROL_STAGE_URB,
    CHERRY_CONTROL_STAGE_VALIDATION,
    CHERRY_CONTROL_STAGE_DEVICE_STATE,
    CHERRY_CONTROL_STAGE_OTHER,
    CHERRY_CONTROL_STAGE_COUNT,
} cherry_control_stage_t;

typedef enum {
    CHERRY_HEARTBEAT_OUTCOME_FAILURE = 0,
    CHERRY_HEARTBEAT_OUTCOME_BUSY,
    CHERRY_HEARTBEAT_OUTCOME_NO_DEVICE,
} cherry_heartbeat_outcome_t;

/* 只有明确的设备状态快照才能归入 no_device；错误码本身不代表物理断开。 */
static inline cherry_heartbeat_outcome_t cherry_heartbeat_classify_outcome(
    cherry_control_stage_t stage,
    bool device_state_valid,
    bool connected,
    bool stopping)
{
    if (stage == CHERRY_CONTROL_STAGE_LIFECYCLE_LOCK ||
        stage == CHERRY_CONTROL_STAGE_EP0_LOCK) {
        return CHERRY_HEARTBEAT_OUTCOME_BUSY;
    }
    if (stage == CHERRY_CONTROL_STAGE_DEVICE_STATE && device_state_valid &&
        !connected && !stopping) {
        return CHERRY_HEARTBEAT_OUTCOME_NO_DEVICE;
    }
    return CHERRY_HEARTBEAT_OUTCOME_FAILURE;
}

typedef struct {
    uint32_t sequence;
    int64_t uptime_us;
    cherry_control_stage_t stage;
    int32_t esp_error;
    bool raw_urb_result_valid;
    int32_t raw_urb_result;
    bool raw_actual_length_valid;
    uint32_t raw_actual_length;
    bool payload_length_valid;
    uint32_t payload_length;
    bool descriptor_header_valid;
    uint8_t descriptor_length;
    uint8_t descriptor_type;
    uint32_t total_latency_us;
    bool lifecycle_wait_valid;
    uint32_t lifecycle_wait_us;
    bool ep0_wait_valid;
    uint32_t ep0_wait_us;
    bool urb_time_valid;
    uint32_t urb_time_us;
    bool device_state_valid;
    uint32_t device_epoch;
    bool connected;
    bool stopping;
    uint32_t input_reports;
    uint32_t input_bytes;
    bool input_age_valid;
    uint32_t input_age_ms;
    uint32_t transfer_errors;
    uint32_t submit_errors;
    uint32_t consecutive_failures;
    int64_t streak_duration_us;
} cherry_heartbeat_failure_t;

typedef struct {
    uint32_t heartbeat_sequence;
    uint32_t success_count;
    uint32_t failure_count;
    uint32_t busy_count;
    uint32_t no_device_count;
    uint32_t stage_counts[CHERRY_CONTROL_STAGE_COUNT];
    uint32_t non_success_count;
    cherry_heartbeat_failure_t history[CHERRYUSB_HEARTBEAT_HISTORY_CAPACITY];
    uint8_t history_count;
    uint8_t history_next;
    bool streak_active;
    uint32_t consecutive_failures;
    int64_t streak_start_us;
    uint32_t recovery_count;
    uint32_t last_recovery_sequence;
    uint32_t last_recovery_failures;
    int64_t last_recovery_streak_us;
} cherry_heartbeat_diagnostics_t;

typedef struct {
    uint32_t sequence;
    uint32_t consecutive_failures;
    int64_t streak_duration_us;
} cherry_heartbeat_recovery_t;

static inline uint32_t cherry_heartbeat_next_sequence(
    cherry_heartbeat_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL) {
        return 0U;
    }
    return ++diagnostics->heartbeat_sequence;
}

static inline void cherry_heartbeat_record_failure(
    cherry_heartbeat_diagnostics_t *diagnostics,
    cherry_heartbeat_failure_t failure,
    cherry_heartbeat_outcome_t outcome)
{
    if (diagnostics == NULL) {
        return;
    }
    if (failure.stage <= CHERRY_CONTROL_STAGE_OK ||
        failure.stage >= CHERRY_CONTROL_STAGE_COUNT) {
        failure.stage = CHERRY_CONTROL_STAGE_OTHER;
    }

    ++diagnostics->non_success_count;
    ++diagnostics->stage_counts[failure.stage];
    if (outcome == CHERRY_HEARTBEAT_OUTCOME_BUSY) {
        ++diagnostics->busy_count;
    } else if (outcome == CHERRY_HEARTBEAT_OUTCOME_NO_DEVICE) {
        ++diagnostics->no_device_count;
    } else {
        ++diagnostics->failure_count;
    }

    if (!diagnostics->streak_active) {
        diagnostics->streak_active = true;
        diagnostics->streak_start_us = failure.uptime_us;
        diagnostics->consecutive_failures = 0U;
    }
    ++diagnostics->consecutive_failures;
    failure.consecutive_failures = diagnostics->consecutive_failures;
    failure.streak_duration_us = failure.uptime_us >= diagnostics->streak_start_us
        ? failure.uptime_us - diagnostics->streak_start_us : 0;

    diagnostics->history[diagnostics->history_next] = failure;
    diagnostics->history_next = (uint8_t)(
        (diagnostics->history_next + 1U) % CHERRYUSB_HEARTBEAT_HISTORY_CAPACITY);
    if (diagnostics->history_count < CHERRYUSB_HEARTBEAT_HISTORY_CAPACITY) {
        ++diagnostics->history_count;
    }
}

static inline bool cherry_heartbeat_record_success(
    cherry_heartbeat_diagnostics_t *diagnostics,
    uint32_t sequence,
    int64_t uptime_us,
    cherry_heartbeat_recovery_t *recovery)
{
    if (diagnostics == NULL) {
        return false;
    }
    ++diagnostics->success_count;
    if (!diagnostics->streak_active) {
        return false;
    }

    const int64_t duration = uptime_us >= diagnostics->streak_start_us
        ? uptime_us - diagnostics->streak_start_us : 0;
    const uint32_t failures = diagnostics->consecutive_failures;
    ++diagnostics->recovery_count;
    diagnostics->last_recovery_sequence = sequence;
    diagnostics->last_recovery_failures = failures;
    diagnostics->last_recovery_streak_us = duration;
    diagnostics->streak_active = false;
    diagnostics->consecutive_failures = 0U;
    diagnostics->streak_start_us = 0;
    if (recovery != NULL) {
        recovery->sequence = sequence;
        recovery->consecutive_failures = failures;
        recovery->streak_duration_us = duration;
    }
    return true;
}

static inline const cherry_heartbeat_failure_t *cherry_heartbeat_history_at(
    const cherry_heartbeat_diagnostics_t *diagnostics,
    size_t chronological_index)
{
    if (diagnostics == NULL || chronological_index >= diagnostics->history_count) {
        return NULL;
    }
    const size_t oldest = (diagnostics->history_next +
        CHERRYUSB_HEARTBEAT_HISTORY_CAPACITY - diagnostics->history_count) %
        CHERRYUSB_HEARTBEAT_HISTORY_CAPACITY;
    const size_t index = (oldest + chronological_index) %
        CHERRYUSB_HEARTBEAT_HISTORY_CAPACITY;
    return &diagnostics->history[index];
}

static inline bool cherryusb_ep0_payload_length(
    uint32_t raw_actual_length,
    size_t requested_length,
    size_t output_capacity,
    size_t *payload_length)
{
    if (payload_length == NULL ||
        raw_actual_length < CHERRYUSB_CONTROL_SETUP_LENGTH) {
        return false;
    }
    const size_t payload =
        (size_t)(raw_actual_length - CHERRYUSB_CONTROL_SETUP_LENGTH);
    if (payload > requested_length || payload > output_capacity) {
        return false;
    }
    *payload_length = payload;
    return true;
}

static inline uint16_t cherryusb_hid_report_wvalue(
    uint8_t report_type,
    uint8_t report_id)
{
    return (uint16_t)(((uint16_t)report_type << 8U) | report_id);
}

static inline uint16_t cherryusb_hid_report_windex(uint8_t interface_number)
{
    return interface_number;
}

#endif
