#include "stats_snapshot.h"

#define COUNTER_RECORD_LENGTH 10U
#define COUNTERS_PER_PAGE ((DUAL_PROXY_MAX_PAYLOAD - DUAL_STATS_SNAPSHOT_HEADER_LENGTH) / COUNTER_RECORD_LENGTH)
#define QUEUES_PER_PAGE ((DUAL_PROXY_MAX_PAYLOAD - DUAL_STATS_SNAPSHOT_HEADER_LENGTH) / DUAL_STATS_QUEUE_RECORD_LENGTH)

static void write_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8U) & 0xffU);
    out[2] = (uint8_t)((value >> 16U) & 0xffU);
    out[3] = (uint8_t)(value >> 24U);
}

static void write_u64(uint8_t *out, uint64_t value)
{
    for (uint8_t index = 0; index < 8U; ++index) {
        out[index] = (uint8_t)(value >> (index * 8U));
    }
}

bool dual_stats_snapshot_serialize_page(const dual_stats_snapshot_t *snapshot,
                                        uint8_t kind, uint8_t page_index,
                                        uint8_t page_count, uint8_t status,
                                        uint8_t *payload, size_t capacity,
                                        uint8_t *payload_length)
{
    if (snapshot == NULL || payload == NULL || payload_length == NULL ||
        capacity < DUAL_STATS_SNAPSHOT_HEADER_LENGTH || page_count == 0U ||
        page_index >= page_count) {
        return false;
    }
    const uint8_t items_per_page = kind == DUAL_STATS_PAGE_COUNTERS
        ? COUNTERS_PER_PAGE : QUEUES_PER_PAGE;
    const uint8_t start = (uint8_t)(page_index * items_per_page);
    uint8_t total;
    if (kind == DUAL_STATS_PAGE_COUNTERS) {
        total = snapshot->counter_count;
    } else if (kind == DUAL_STATS_PAGE_QUEUES) {
        total = snapshot->queue_count;
    } else {
        return false;
    }
    if (start >= total) {
        return false;
    }
    uint8_t count = (uint8_t)(total - start);
    if (count > items_per_page) {
        count = items_per_page;
    }
    const size_t record_size = kind == DUAL_STATS_PAGE_COUNTERS
        ? COUNTER_RECORD_LENGTH : DUAL_STATS_QUEUE_RECORD_LENGTH;
    const size_t payload_size = DUAL_STATS_SNAPSHOT_HEADER_LENGTH +
        (size_t)count * record_size;
    if (capacity < payload_size || payload_size > DUAL_PROXY_MAX_PAYLOAD) {
        return false;
    }
    payload[0] = DUAL_STATS_SNAPSHOT_SCHEMA_VERSION;
    payload[1] = snapshot->role;
    payload[2] = kind;
    payload[3] = page_index;
    payload[4] = page_count;
    payload[5] = status;
    payload[6] = count;
    payload[7] = 0U;
    write_u32(&payload[8], snapshot->uptime_ms);
    if (kind == DUAL_STATS_PAGE_COUNTERS) {
        for (uint8_t item = 0; item < count; ++item) {
            const dual_stats_counter_t *counter = &snapshot->counters[start + item];
            uint8_t *record = &payload[DUAL_STATS_SNAPSHOT_HEADER_LENGTH +
                                      (size_t)item * COUNTER_RECORD_LENGTH];
            record[0] = counter->id;
            record[1] = counter->value_type;
            write_u64(&record[2], counter->value);
        }
    } else {
        for (uint8_t item = 0; item < count; ++item) {
            const dual_stats_queue_t *queue = &snapshot->queues[start + item];
            uint8_t *record = &payload[DUAL_STATS_SNAPSHOT_HEADER_LENGTH +
                                      (size_t)item * DUAL_STATS_QUEUE_RECORD_LENGTH];
            record[0] = queue->id;
            record[1] = queue->unit;
            write_u16(&record[2], queue->capacity);
            write_u16(&record[4], queue->depth);
            write_u16(&record[6], queue->peak);
            write_u32(&record[8], queue->received);
            write_u32(&record[12], queue->rejected);
            write_u32(&record[16], queue->dropped);
        }
    }
    *payload_length = (uint8_t)payload_size;
    return true;
}
