#include "ns2_protocol_trace.h"

#include <string.h>

#ifdef NS2_UART_DIAG

static ns2_protocol_trace_record_t s_records[NS2_PROTOCOL_TRACE_CAPACITY];
static uint16_t s_head;
static uint16_t s_count;
static uint32_t s_overwritten;
static uint32_t s_next_sequence;
static bool s_enabled;

void ns2_protocol_trace_clear(void) {
    s_head = 0;
    s_count = 0;
    s_overwritten = 0;
    s_next_sequence = 0;
}

void ns2_protocol_trace_set_enabled(bool enabled) {
    s_enabled = enabled;
}

void ns2_protocol_trace_get_status(ns2_protocol_trace_status_t *status) {
    if (!status) return;
    status->enabled = s_enabled;
    status->count = s_count;
    status->capacity = NS2_PROTOCOL_TRACE_CAPACITY;
    status->overwritten = s_overwritten;
    status->next_sequence = s_next_sequence;
}

void ns2_protocol_trace_record(uint32_t timestamp_us, uint8_t personality,
                               ns2_protocol_trace_kind_t kind,
                               ns2_protocol_trace_direction_t direction,
                               uint8_t id, uint8_t subcommand,
                               const uint8_t *payload, size_t length) {
    if (!s_enabled) return;

    ns2_protocol_trace_record_t *record = &s_records[s_head];
    memset(record, 0, sizeof(*record));
    record->sequence = s_next_sequence++;
    record->timestamp_us = timestamp_us;
    record->total_length = length > UINT16_MAX ? UINT16_MAX : (uint16_t)length;
    record->personality = personality;
    record->kind = (uint8_t)kind;
    record->direction = (uint8_t)direction;
    record->id = id;
    record->subcommand = subcommand;
    size_t captured = length;
    if (captured > NS2_PROTOCOL_TRACE_PAYLOAD_MAX)
        captured = NS2_PROTOCOL_TRACE_PAYLOAD_MAX;
    if (!payload) captured = 0;
    record->captured_length = (uint8_t)captured;
    if (captured) memcpy(record->payload, payload, captured);

    s_head = (uint16_t)((s_head + 1u) % NS2_PROTOCOL_TRACE_CAPACITY);
    if (s_count < NS2_PROTOCOL_TRACE_CAPACITY)
        s_count++;
    else
        s_overwritten++;
}

bool ns2_protocol_trace_get(uint16_t oldest_first_index,
                            ns2_protocol_trace_record_t *record) {
    if (!record || oldest_first_index >= s_count) return false;
    uint16_t oldest = (uint16_t)((s_head + NS2_PROTOCOL_TRACE_CAPACITY - s_count) %
                                 NS2_PROTOCOL_TRACE_CAPACITY);
    uint16_t slot = (uint16_t)((oldest + oldest_first_index) %
                               NS2_PROTOCOL_TRACE_CAPACITY);
    *record = s_records[slot];
    return true;
}

#endif
