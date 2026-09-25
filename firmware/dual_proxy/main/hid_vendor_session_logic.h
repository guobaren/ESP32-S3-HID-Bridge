#ifndef HID_VENDOR_SESSION_LOGIC_H
#define HID_VENDOR_SESSION_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* Session tokens let asynchronous HID++ workers reject work dequeued before
 * a new physical Profile replaced the USB clone. Zero remains invalid. */
static inline uint32_t hid_vendor_session_advance(uint32_t current)
{
    ++current;
    return current == 0U ? 1U : current;
}

static inline bool hid_vendor_session_matches(uint32_t item, uint32_t current)
{
    return item != 0U && item == current;
}

#endif
