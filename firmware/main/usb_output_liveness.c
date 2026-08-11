#include "usb_output_liveness.h"

#include <limits.h>
#include <stddef.h>

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
        state->unavailable_elapsed_ms = 0;
        if (!state->available) {
            state->available = true;
            return USB_OUTPUT_LIVENESS_BECAME_AVAILABLE;
        }
        return USB_OUTPUT_LIVENESS_NO_CHANGE;
    }

    if (!state->available) {
        return USB_OUTPUT_LIVENESS_NO_CHANGE;
    }

    if (UINT32_MAX - state->unavailable_elapsed_ms < elapsed_ms) {
        state->unavailable_elapsed_ms = UINT32_MAX;
    } else {
        state->unavailable_elapsed_ms += elapsed_ms;
    }

    if (state->unavailable_elapsed_ms < unavailable_timeout_ms) {
        return USB_OUTPUT_LIVENESS_NO_CHANGE;
    }

    state->available = false;
    state->unavailable_elapsed_ms = 0;
    return USB_OUTPUT_LIVENESS_BECAME_UNAVAILABLE;
}
