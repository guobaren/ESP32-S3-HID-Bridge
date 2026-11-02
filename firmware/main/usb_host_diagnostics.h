#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stats_snapshot.h"

#define DUAL_USB_HOST_DIAG_FLAGS_AVAILABLE (UINT32_C(1) << 0)
#define DUAL_USB_HOST_DIAG_FLAGS_BACKEND_CHERRYUSB (UINT32_C(1) << 1)
#define DUAL_USB_HOST_DIAG_FLAGS_SCHEMA_SHIFT 4U
#define DUAL_USB_HOST_DIAG_FLAGS_SCHEMA_MASK (UINT32_C(0x0F) << DUAL_USB_HOST_DIAG_FLAGS_SCHEMA_SHIFT)
#define DUAL_USB_HOST_DIAG_FLAGS_CAP_DWC2_IRQ (UINT32_C(1) << 8)
#define DUAL_USB_HOST_DIAG_FLAGS_CAP_HUB_EVENTS (UINT32_C(1) << 9)
#define DUAL_USB_HOST_DIAG_FLAGS_CAP_INPUT_HEARTBEAT (UINT32_C(1) << 10)
#define DUAL_USB_HOST_DIAG_FLAGS_REGISTER_VALID (UINT32_C(1) << 11)
#define DUAL_USB_HOST_DIAG_FLAGS_LIFECYCLE_VALID (UINT32_C(1) << 12)
#define DUAL_USB_HOST_DIAG_FLAGS_INPUT_VALID (UINT32_C(1) << 13)
#define DUAL_USB_HOST_DIAG_FLAGS_INITIALIZED (UINT32_C(1) << 14)
#define DUAL_USB_HOST_DIAG_FLAGS_STOPPING (UINT32_C(1) << 15)
#define DUAL_USB_HOST_DIAG_FLAGS_HPORT_PRESENT (UINT32_C(1) << 16)
#define DUAL_USB_HOST_DIAG_FLAGS_CONNECTED (UINT32_C(1) << 17)
#define DUAL_USB_HOST_DIAG_FLAGS_PROFILE_READY (UINT32_C(1) << 18)

#define DUAL_USB_HOST_DIAG_NO_RAW_ERROR INT32_MIN

typedef enum {
    DUAL_USB_HOST_DIAG_STAGE_UNKNOWN = 0,
    DUAL_USB_HOST_DIAG_STAGE_DISCONNECT_EVENT,
    DUAL_USB_HOST_DIAG_STAGE_DEBOUNCE,
    DUAL_USB_HOST_DIAG_STAGE_PORT_RESET,
    DUAL_USB_HOST_DIAG_STAGE_PORT_ENABLE_CHECK,
    DUAL_USB_HOST_DIAG_STAGE_ENUMERATE_USB,
    DUAL_USB_HOST_DIAG_STAGE_ENUMERATE_SUCCESS,
    DUAL_USB_HOST_DIAG_STAGE_HUB_PORT_STATUS_USB,
    DUAL_USB_HOST_DIAG_STAGE_HUB_CLEAR_FEATURE_USB,
    DUAL_USB_HOST_DIAG_STAGE_HUB_RESET_USB,
    DUAL_USB_HOST_DIAG_STAGE_PORT_ENABLE_FAILURE,
    DUAL_USB_HOST_DIAG_STAGE_HID_CLASS_ALLOC_USB,
    DUAL_USB_HOST_DIAG_STAGE_HID_DESCRIPTOR_VALIDATE_USB,
    DUAL_USB_HOST_DIAG_STAGE_HID_REPORT_DESCRIPTOR_HEADER_USB,
    DUAL_USB_HOST_DIAG_STAGE_HID_INTERFACE_LOOKUP_USB,
    DUAL_USB_HOST_DIAG_STAGE_HID_GET_REPORT_DESCRIPTOR_USB,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_URB_RAW,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_LIFECYCLE_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_STATE_ALLOC_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_FULL_DESCRIPTOR_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_DESCRIPTOR_LENGTH_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_ENDPOINT_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_INPUT_BUFFER_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_TIMER_CREATE_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_INTERFACE_OPEN_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_PROFILE_LOCK_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_PROFILE_ADD_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_TIMER_START_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_ARGUMENTS_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_PROFILE_CONTROL_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_PROFILE_VALIDATE_ESP,
    DUAL_USB_HOST_DIAG_STAGE_HID_INIT_PROFILE_BUILD_ESP,
} dual_usb_host_diag_stage_t;

typedef struct {
    volatile uint32_t pcdet_count;
    volatile uint32_t pcdet_wake_count;
    volatile uint32_t discint_count;
    volatile uint32_t pcdet_last_ms;
    volatile uint32_t pcdet_wake_last_ms;
    volatile uint32_t discint_last_ms;
    volatile uint32_t hub_wake_count;
    volatile uint32_t connection_change_count;
    volatile uint32_t connection_last_status_change;
    volatile uint32_t connection_last_ms;
    volatile uint32_t enum_attempt_count;
    volatile uint32_t enum_success_count;
    volatile uint32_t enum_fail_count;
    volatile uint32_t enum_last_stage;
    volatile int32_t enum_last_raw_error;
    volatile uint32_t enum_last_ms;
    volatile uint32_t enum_last_fail_stage;
    volatile int32_t enum_last_fail_raw_error;
    volatile uint32_t enum_last_fail_ms;
    volatile uint32_t input_count;
    volatile uint32_t input_last_ms;
    volatile uint32_t heartbeat_ok_count;
    volatile uint32_t heartbeat_fail_count;
} dual_usb_host_diag_state_t;

typedef struct {
    bool snapshot_valid;
    bool initialized;
    bool stopping;
    bool hport_present;
    bool connected;
    bool profile_ready;
    uint32_t device_epoch;
} dual_usb_host_diag_backend_state_t;

static inline void dual_usb_host_diag_record_pcdet(
    dual_usb_host_diag_state_t *state,
    bool connected_wake,
    uint32_t now_ms)
{
    __atomic_store_n(&state->pcdet_last_ms, now_ms, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&state->pcdet_count, 1U, __ATOMIC_RELAXED);
    if (connected_wake) {
        __atomic_store_n(&state->pcdet_wake_last_ms, now_ms, __ATOMIC_RELAXED);
        (void)__atomic_add_fetch(&state->pcdet_wake_count, 1U, __ATOMIC_RELAXED);
    }
}

static inline void dual_usb_host_diag_record_discint(
    dual_usb_host_diag_state_t *state,
    uint32_t now_ms)
{
    __atomic_store_n(&state->discint_last_ms, now_ms, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&state->discint_count, 1U, __ATOMIC_RELAXED);
}

static inline void dual_usb_host_diag_record_hub_wake(dual_usb_host_diag_state_t *state)
{
    (void)__atomic_add_fetch(&state->hub_wake_count, 1U, __ATOMIC_RELAXED);
}

static inline void dual_usb_host_diag_record_connection_change(
    dual_usb_host_diag_state_t *state,
    uint16_t status,
    uint16_t change,
    uint32_t now_ms)
{
    __atomic_store_n(&state->connection_last_status_change,
                     ((uint32_t)status << 16U) | change, __ATOMIC_RELAXED);
    __atomic_store_n(&state->connection_last_ms, now_ms, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&state->connection_change_count, 1U, __ATOMIC_RELAXED);
}

static inline void dual_usb_host_diag_set_stage(
    dual_usb_host_diag_state_t *state,
    uint32_t stage,
    uint32_t now_ms)
{
    __atomic_store_n(&state->enum_last_raw_error,
                     DUAL_USB_HOST_DIAG_NO_RAW_ERROR, __ATOMIC_RELAXED);
    __atomic_store_n(&state->enum_last_stage, stage, __ATOMIC_RELAXED);
    __atomic_store_n(&state->enum_last_ms, now_ms, __ATOMIC_RELAXED);
}

static inline void dual_usb_host_diag_record_attempt(
    dual_usb_host_diag_state_t *state,
    uint32_t now_ms)
{
    (void)now_ms;
    (void)__atomic_add_fetch(&state->enum_attempt_count, 1U, __ATOMIC_RELAXED);
}

static inline void dual_usb_host_diag_record_failure(
    dual_usb_host_diag_state_t *state,
    uint32_t stage,
    int32_t raw_error,
    uint32_t now_ms)
{
    dual_usb_host_diag_set_stage(state, stage, now_ms);
    __atomic_store_n(&state->enum_last_raw_error, raw_error, __ATOMIC_RELAXED);
    __atomic_store_n(&state->enum_last_fail_stage, stage, __ATOMIC_RELAXED);
    __atomic_store_n(&state->enum_last_fail_raw_error, raw_error, __ATOMIC_RELAXED);
    __atomic_store_n(&state->enum_last_fail_ms, now_ms, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&state->enum_fail_count, 1U, __ATOMIC_RELAXED);
}

static inline void dual_usb_host_diag_record_success(
    dual_usb_host_diag_state_t *state,
    uint32_t now_ms)
{
    dual_usb_host_diag_set_stage(state, DUAL_USB_HOST_DIAG_STAGE_ENUMERATE_SUCCESS, now_ms);
    (void)__atomic_add_fetch(&state->enum_success_count, 1U, __ATOMIC_RELAXED);
}

static inline void dual_usb_host_diag_record_input(
    dual_usb_host_diag_state_t *state,
    uint32_t now_ms)
{
    __atomic_store_n(&state->input_last_ms, now_ms, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&state->input_count, 1U, __ATOMIC_RELAXED);
}

static inline void dual_usb_host_diag_record_heartbeat(
    dual_usb_host_diag_state_t *state,
    bool success)
{
    if (success) {
        (void)__atomic_add_fetch(&state->heartbeat_ok_count, 1U, __ATOMIC_RELAXED);
    } else {
        (void)__atomic_add_fetch(&state->heartbeat_fail_count, 1U, __ATOMIC_RELAXED);
    }
}

uint32_t dual_usb_host_diag_now_ms(void);
void dual_usb_host_diag_pcdet(bool connected_wake);
void dual_usb_host_diag_discint(void);
void dual_usb_host_diag_hub_wake(void);
void dual_usb_host_diag_connection_change(uint16_t status, uint16_t change);
void dual_usb_host_diag_attempt(void);
void dual_usb_host_diag_stage(uint32_t stage);
void dual_usb_host_diag_failure(uint32_t stage, int32_t raw_error);
void dual_usb_host_diag_success(void);
void dual_usb_host_diag_input(uint32_t now_ms);
void dual_usb_host_diag_heartbeat(bool success);
void dual_usb_host_diag_collect(dual_stats_snapshot_t *snapshot);
bool dual_hid_host_cherryusb_diag_state(dual_usb_host_diag_backend_state_t *state);
bool dual_usb_host_diag_read_dwc2_registers(uint8_t bus_id,
                                            uint32_t *hprt,
                                            uint32_t *gintsts,
                                            uint32_t *gintmsk);
void dual_usb_host_diag_set_dwc2_active(uint8_t bus_id,
                                        uintptr_t reg_base,
                                        bool active);
