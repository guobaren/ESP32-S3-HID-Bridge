#include "stats_snapshot.h"

#include <string.h>

#include "bridge_protocol.h"
#include "esp_timer.h"

#include "hid_host_mouse.h"
#include "pc_hid_output.h"
#include "uart1_link.h"

void dual_stats_snapshot_add_counter(dual_stats_snapshot_t *snapshot,
                                     uint8_t id, uint64_t value)
{
    if (snapshot == NULL || snapshot->counter_count >= DUAL_STATS_MAX_COUNTERS) {
        if (snapshot != NULL) {
            snapshot->overflow = true;
        }
        return;
    }
    dual_stats_counter_t *counter = &snapshot->counters[snapshot->counter_count++];
    counter->id = id;
    counter->value_type = 1U; /* u64 little endian */
    counter->value = value;
}

void dual_stats_snapshot_add_signed_counter(dual_stats_snapshot_t *snapshot,
                                            uint8_t id, int64_t value)
{
    if (snapshot == NULL) {
        return;
    }
    const uint8_t previous_count = snapshot->counter_count;
    dual_stats_snapshot_add_counter(snapshot, id, (uint64_t)value);
    if (snapshot->counter_count > previous_count) {
        snapshot->counters[snapshot->counter_count - 1U].value_type = 2U;
    }
}

void dual_stats_snapshot_add_queue(dual_stats_snapshot_t *snapshot,
                                   uint8_t id, uint16_t capacity,
                                   uint16_t depth, uint16_t peak,
                                   uint32_t received, uint32_t rejected,
                                   uint32_t dropped)
{
    dual_stats_snapshot_add_queue_unit(snapshot, id, DUAL_STATS_UNIT_ITEMS,
        capacity, depth, peak, received, rejected, dropped);
}

void dual_stats_snapshot_add_queue_unit(dual_stats_snapshot_t *snapshot,
                                        uint8_t id, uint8_t unit,
                                        uint16_t capacity, uint16_t depth,
                                        uint16_t peak, uint32_t received,
                                        uint32_t rejected, uint32_t dropped)
{
    if (snapshot == NULL || snapshot->queue_count >= DUAL_STATS_MAX_QUEUES) {
        if (snapshot != NULL) {
            snapshot->overflow = true;
        }
        return;
    }
    dual_stats_queue_t *queue = &snapshot->queues[snapshot->queue_count++];
    queue->id = id;
    queue->unit = unit;
    queue->capacity = capacity;
    queue->depth = depth;
    queue->peak = peak;
    queue->received = received;
    queue->rejected = rejected;
    queue->dropped = dropped;
}

void dual_stats_snapshot_add_queue_unknown(dual_stats_snapshot_t *snapshot,
                                           uint8_t id, uint16_t capacity,
                                           uint16_t depth, uint16_t peak)
{
    dual_stats_snapshot_add_queue(snapshot, id, capacity, depth, peak,
                                  DUAL_STATS_UNKNOWN_U32,
                                  DUAL_STATS_UNKNOWN_U32,
                                  DUAL_STATS_UNKNOWN_U32);
}

void dual_stats_snapshot_collect(uint8_t role, dual_stats_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->role = role;
    snapshot->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if (role == DUAL_ROLE_MOUSE_HOST) {
        dual_hid_host_collect_stats(snapshot);
    } else if (role == DUAL_ROLE_PC_DEVICE) {
        dual_pc_hid_collect_stats(snapshot);
    }
    if (role == DUAL_ROLE_MOUSE_HOST || role == DUAL_ROLE_PC_DEVICE) {
        dual_uart1_collect_stats(snapshot);
    }
}
