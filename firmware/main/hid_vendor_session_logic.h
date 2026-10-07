#ifndef HID_VENDOR_SESSION_LOGIC_H
#define HID_VENDOR_SESSION_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#define HID_VENDOR_SESSION_BEGIN_RETRY_INTERVAL_US 100000LL
#define HID_VENDOR_SESSION_BEGIN_MAX_ATTEMPTS 10U

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

static inline bool hid_vendor_session_ack_matches(
    uint32_t expected_p_generation, uint32_t expected_m_generation,
    uint32_t expected_epoch, uint32_t ack_p_generation,
    uint32_t ack_m_generation, uint32_t ack_epoch)
{
    return expected_p_generation != 0U && expected_m_generation != 0U &&
        expected_epoch != 0U && expected_p_generation == ack_p_generation &&
        expected_m_generation == ack_m_generation && expected_epoch == ack_epoch;
}

static inline bool hid_vendor_session_begin_retry_due(
    bool attached, bool ready, uint32_t attempts, uint32_t max_attempts,
    int64_t last_attempt_us, int64_t now_us)
{
    return attached && !ready && attempts < max_attempts &&
        (last_attempt_us == 0 || now_us < last_attempt_us ||
         now_us - last_attempt_us >= HID_VENDOR_SESSION_BEGIN_RETRY_INTERVAL_US);
}

static inline bool hid_vendor_session_pending_matches(
    uint16_t pending_transaction_id, uint32_t pending_session_generation,
    uint16_t item_transaction_id, uint32_t item_session_generation)
{
    return item_transaction_id != 0U && item_session_generation != 0U &&
        pending_transaction_id == item_transaction_id &&
        pending_session_generation == item_session_generation;
}

/*
 * The PC may send its first vendor request immediately after SET_CONFIGURATION.
 * During a profile change, admit that request only after the current clone is
 * really mounted and the current profile operation is still waiting for its
 * final ACK.  A not-yet-installed clone and a canceled/stale operation remain
 * closed.
 */
static inline bool hid_vendor_session_control_entry_ready(
    bool installed, bool clone_active, bool mounted, bool reconfiguring,
    bool waiting_host, bool waiting_final_ack, bool disconnect_requested,
    uint32_t operation_epoch, uint32_t installed_operation_epoch)
{
    if (!installed || !clone_active || !mounted || disconnect_requested) {
        return false;
    }
    if (!reconfiguring) {
        return true;
    }
    return (waiting_host || waiting_final_ack) && operation_epoch != 0U &&
        operation_epoch == installed_operation_epoch;
}

#endif
