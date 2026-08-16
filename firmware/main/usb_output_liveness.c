#include "usb_output_liveness.h"

#include <limits.h>
#include <stddef.h>

static uint32_t saturating_add_ms(uint32_t accumulated_ms, uint32_t elapsed_ms)
{
    if (UINT32_MAX - accumulated_ms < elapsed_ms) {
        return UINT32_MAX;
    }
    return accumulated_ms + elapsed_ms;
}

usb_output_liveness_event_t usb_output_liveness_update(
    usb_output_liveness_t *state,
    bool ready,
    uint32_t elapsed_ms,
    uint32_t unavailable_timeout_ms)
{
    if (state == NULL) {
        return USB_OUTPUT_LIVENESS_NO_CHANGE;
    }

    if (ready) {
        state->startup_elapsed_ms = 0;
        state->unavailable_elapsed_ms = 0;
        state->has_been_ready = true;
        state->initial_unavailable_reported = false;
        if (!state->available) {
            state->available = true;
            return USB_OUTPUT_LIVENESS_BECAME_AVAILABLE;
        }
        return USB_OUTPUT_LIVENESS_NO_CHANGE;
    }

    if (!state->has_been_ready) {
        if (state->initial_unavailable_reported) {
            return USB_OUTPUT_LIVENESS_NO_CHANGE;
        }
        state->startup_elapsed_ms = saturating_add_ms(
            state->startup_elapsed_ms,
            elapsed_ms);
        if (state->startup_elapsed_ms < USB_OUTPUT_STARTUP_ENUMERATION_GRACE_MS) {
            return USB_OUTPUT_LIVENESS_NO_CHANGE;
        }
        state->initial_unavailable_reported = true;
        state->startup_elapsed_ms = 0;
        return USB_OUTPUT_LIVENESS_BECAME_UNAVAILABLE;
    }

    if (!state->available) {
        return USB_OUTPUT_LIVENESS_NO_CHANGE;
    }

    state->unavailable_elapsed_ms = saturating_add_ms(
        state->unavailable_elapsed_ms,
        elapsed_ms);
    if (state->unavailable_elapsed_ms < unavailable_timeout_ms) {
        return USB_OUTPUT_LIVENESS_NO_CHANGE;
    }

    state->available = false;
    state->unavailable_elapsed_ms = 0;
    return USB_OUTPUT_LIVENESS_BECAME_UNAVAILABLE;
}
