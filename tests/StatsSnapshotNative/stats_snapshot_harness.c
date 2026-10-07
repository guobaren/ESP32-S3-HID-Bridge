#include <stdio.h>
#include <string.h>

#include "bridge_protocol.h"
#include "stats_snapshot.h"

#define TEST_SEQUENCE 0xA17CU

static void add_counter(dual_stats_snapshot_t *snapshot, uint8_t id, uint8_t type, uint64_t value)
{
    dual_stats_counter_t *counter = &snapshot->counters[snapshot->counter_count++];
    counter->id = id;
    counter->value_type = type;
    counter->value = value;
}

static void add_queue(dual_stats_snapshot_t *snapshot, uint8_t id, uint8_t unit,
                      uint16_t capacity, uint16_t depth, uint16_t peak,
                      uint32_t received, uint32_t rejected, uint32_t dropped)
{
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

static void fill_snapshot(uint8_t role, dual_stats_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->role = role;
    snapshot->uptime_ms = 1234567U;
    if (role == DUAL_ROLE_MOUSE_HOST) {
        for (uint8_t id = 1; id <= 36; ++id) {
            if (id == 19U) {
                add_counter(snapshot, id, 2U, (uint64_t)(int64_t)-12345);
            } else if (id == 20U) {
                add_counter(snapshot, id, 2U, (uint64_t)(int64_t)-6789);
            } else {
                add_counter(snapshot, id, 1U, (uint64_t)id * 1000U);
            }
        }
        add_counter(snapshot, DUAL_STAT_M_VENDOR_SESSION_STATE, 1U,
            (UINT64_C(33) << DUAL_M_VENDOR_SESSION_EPOCH_SHIFT) |
            DUAL_M_VENDOR_SESSION_ACTIVE |
            DUAL_M_VENDOR_SESSION_FIRST_REQUEST |
            DUAL_M_VENDOR_SESSION_PEER_CURRENT);
        add_queue(snapshot, 1U, 1U, 96U, 4U, 31U, 1200U, 3U, 2U);
        add_queue(snapshot, 2U, 1U, 8U, 1U, 7U, 310U, 2U, 1U);
        add_queue(snapshot, 3U, 1U, 4U, 0U, 3U, 81U, 0U, 0U);
    } else {
        for (uint8_t id = 37; id <= 48; ++id) {
            add_counter(snapshot, id, 1U, (uint64_t)id * 1000U);
        }
        add_counter(snapshot, DUAL_STAT_P_USB_STATE, 1U,
            (UINT64_C(7) << DUAL_P_USB_STATE_EPOCH_SHIFT) |
            ((uint64_t)DUAL_P_USB_RESULT_MOUNTED_ACKED <<
                DUAL_P_USB_STATE_RESULT_SHIFT) |
            DUAL_P_USB_STATE_ATTACHED | DUAL_P_USB_STATE_INSTALLED |
            DUAL_P_USB_STATE_CLONE_ACTIVE | DUAL_P_USB_STATE_MOUNTED);
        add_queue(snapshot, 10U, 1U, 32U, 2U, 11U, 225U, 4U, 3U);
        add_queue(snapshot, 11U, 1U, 64U, 3U, 23U, 910U, 5U, 1U);
        add_queue(snapshot, 12U, 1U, 8U, 0U, 5U, 78U, 0U, 0U);
    }

    for (uint8_t id = 49; id <= 66; ++id) {
        add_counter(snapshot, id, 1U, (uint64_t)id * 1000U);
    }
    for (uint8_t id = 4; id <= 8; ++id) {
        add_queue(snapshot, id, 1U, 32U, (uint16_t)(id - 4U), 16U,
                  1000U + id, 2U, 1U);
    }
    add_queue(snapshot, 9U, 1U, 128U, 3U, 42U, UINT32_MAX, UINT32_MAX, 17U);
    add_queue(snapshot, 13U, 2U, 16384U, 23U, 1024U,
              UINT32_MAX, UINT32_MAX, UINT32_MAX);
}

static void print_frame(uint8_t role, uint8_t kind, uint8_t index,
                        uint8_t page_count, const uint8_t *payload, uint8_t payload_length)
{
    dual_frame_t frame = {
        .version = DUAL_PROXY_PROTOCOL_VERSION,
        .type = DUAL_MESSAGE_STATS_SNAPSHOT_RESPONSE,
        .sequence = TEST_SEQUENCE,
        .payload_length = payload_length,
    };
    memcpy(frame.payload, payload, payload_length);
    uint8_t wire[9U + DUAL_PROXY_MAX_PAYLOAD];
    size_t wire_length = 0;
    if (dual_frame_serialize(&frame, wire, sizeof(wire), &wire_length) != ESP_OK) {
        return;
    }
    printf("%u,%u,%u,%u,", role, kind, index, page_count);
    for (size_t byte = 0; byte < wire_length; ++byte) {
        printf("%02X", wire[byte]);
    }
    putchar('\n');
}

static void emit_snapshot(uint8_t role)
{
    dual_stats_snapshot_t snapshot;
    fill_snapshot(role, &snapshot);
    const uint8_t counter_pages = (uint8_t)((snapshot.counter_count + 4U) / 5U);
    const uint8_t queue_pages = (uint8_t)((snapshot.queue_count + 1U) / 2U);
    uint8_t payload[DUAL_PROXY_MAX_PAYLOAD];
    uint8_t payload_length = 0;
    for (uint8_t index = 0; index < counter_pages; ++index) {
        if (dual_stats_snapshot_serialize_page(&snapshot, DUAL_STATS_PAGE_COUNTERS,
                index, counter_pages, 0U, payload, sizeof(payload), &payload_length)) {
            print_frame(role, DUAL_STATS_PAGE_COUNTERS, index, counter_pages, payload, payload_length);
        }
    }
    for (uint8_t index = 0; index < queue_pages; ++index) {
        if (dual_stats_snapshot_serialize_page(&snapshot, DUAL_STATS_PAGE_QUEUES,
                index, queue_pages, 0U, payload, sizeof(payload), &payload_length)) {
            print_frame(role, DUAL_STATS_PAGE_QUEUES, index, queue_pages, payload, payload_length);
        }
    }
}

int main(void)
{
    emit_snapshot(DUAL_ROLE_PC_DEVICE);
    emit_snapshot(DUAL_ROLE_MOUSE_HOST);
    return 0;
}
