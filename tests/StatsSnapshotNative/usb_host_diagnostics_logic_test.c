#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "usb_host_diagnostics.h"

int main(void)
{
    dual_usb_host_diag_state_t state;
    memset(&state, 0, sizeof(state));

    dual_usb_host_diag_state_t zero_timestamp_state;
    memset(&zero_timestamp_state, 0, sizeof(zero_timestamp_state));

    assert(DUAL_STATS_MAX_COUNTERS >= 83U);
    assert(state.pcdet_count == 0U);
    assert(state.pcdet_wake_count == 0U);
    assert(state.pcdet_wake_last_ms == 0U);

    dual_usb_host_diag_record_connection_change(
        &zero_timestamp_state, 1U, 1U, 0U);
    dual_usb_host_diag_set_stage(
        &zero_timestamp_state, DUAL_USB_HOST_DIAG_STAGE_DEBOUNCE, 0U);
    assert(zero_timestamp_state.connection_change_count == 1U);
    assert(zero_timestamp_state.enum_last_stage ==
           DUAL_USB_HOST_DIAG_STAGE_DEBOUNCE);
    assert(zero_timestamp_state.enum_last_ms == 0U);
    assert(zero_timestamp_state.enum_last_raw_error ==
           DUAL_USB_HOST_DIAG_NO_RAW_ERROR);

    dual_usb_host_diag_record_pcdet(&state, false, 100U);
    assert(state.pcdet_count == 1U);
    assert(state.pcdet_wake_count == 0U);
    assert(state.pcdet_last_ms == 100U);
    assert(state.pcdet_wake_last_ms == 0U);

    dual_usb_host_diag_record_pcdet(&state, true, 250U);
    assert(state.pcdet_count == 2U);
    assert(state.pcdet_wake_count == 1U);
    assert(state.pcdet_wake_last_ms == 250U);

    dual_usb_host_diag_record_attempt(&state, 300U);
    dual_usb_host_diag_record_failure(
        &state, DUAL_USB_HOST_DIAG_STAGE_HID_GET_REPORT_DESCRIPTOR_USB,
        -17, 310U);
    assert(state.enum_attempt_count == 1U);
    assert(state.enum_fail_count == 1U);
    assert(state.enum_last_fail_stage ==
           DUAL_USB_HOST_DIAG_STAGE_HID_GET_REPORT_DESCRIPTOR_USB);
    assert(state.enum_last_fail_raw_error == -17);

    dual_usb_host_diag_record_success(&state, 400U);
    assert(state.enum_success_count == 1U);
    assert(state.enum_last_stage == DUAL_USB_HOST_DIAG_STAGE_ENUMERATE_SUCCESS);
    assert(state.enum_last_fail_stage ==
           DUAL_USB_HOST_DIAG_STAGE_HID_GET_REPORT_DESCRIPTOR_USB);
    assert(state.enum_last_fail_raw_error == -17);
    assert(state.enum_last_fail_ms == 310U);

    dual_usb_host_diag_record_failure(
        &state, DUAL_USB_HOST_DIAG_STAGE_DEBOUNCE,
        DUAL_USB_HOST_DIAG_NO_RAW_ERROR, 500U);
    assert(state.enum_last_fail_stage == DUAL_USB_HOST_DIAG_STAGE_DEBOUNCE);
    assert(state.enum_last_fail_raw_error == DUAL_USB_HOST_DIAG_NO_RAW_ERROR);
    puts("usb_host_diagnostics_logic: PASS (time-zero stage and no-raw sentinel)");
    return 0;
}
