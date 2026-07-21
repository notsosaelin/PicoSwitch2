#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_protocol_trace.h"

static ns2_protocol_trace_status_t status(void) {
    ns2_protocol_trace_status_t value;
    ns2_protocol_trace_get_status(&value);
    return value;
}

int main(void) {
    uint8_t payload[NS2_PROTOCOL_TRACE_PAYLOAD_MAX + 8];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;

    ns2_protocol_trace_clear();
    ns2_protocol_trace_set_enabled(false);
    ns2_protocol_trace_record(10, 0, NS2_TRACE_BULK_COMMAND,
                              NS2_TRACE_CONSOLE_TO_DEVICE, 0x10, 0x01,
                              payload, sizeof(payload));
    assert(status().count == 0);

    ns2_protocol_trace_set_enabled(true);
    ns2_protocol_trace_record(20, 1, NS2_TRACE_BULK_COMMAND,
                              NS2_TRACE_CONSOLE_TO_DEVICE, 0x10, 0x01,
                              payload, sizeof(payload));
    ns2_protocol_trace_record(25, 1, NS2_TRACE_BULK_RESPONSE,
                              NS2_TRACE_DEVICE_TO_CONSOLE, 0x10, 0x01,
                              NULL, 0);

    ns2_protocol_trace_record_t record;
    assert(ns2_protocol_trace_get(0, &record));
    assert(record.sequence == 0 && record.timestamp_us == 20);
    assert(record.personality == 1 && record.kind == NS2_TRACE_BULK_COMMAND);
    assert(record.direction == NS2_TRACE_CONSOLE_TO_DEVICE);
    assert(record.id == 0x10 && record.subcommand == 0x01);
    assert(record.total_length == sizeof(payload));
    assert(record.captured_length == NS2_PROTOCOL_TRACE_PAYLOAD_MAX);
    assert(memcmp(record.payload, payload, NS2_PROTOCOL_TRACE_PAYLOAD_MAX) == 0);
    assert(ns2_protocol_trace_get(1, &record));
    assert(record.sequence == 1 && record.captured_length == 0);
    assert(!ns2_protocol_trace_get(2, &record));

    ns2_protocol_trace_clear();
    for (uint32_t i = 0; i < NS2_PROTOCOL_TRACE_CAPACITY + 3u; i++) {
        uint8_t value = (uint8_t)i;
        ns2_protocol_trace_record(100 + i, 2, NS2_TRACE_HID_OUTPUT,
                                  NS2_TRACE_CONSOLE_TO_DEVICE, 1, 0,
                                  &value, 1);
    }
    ns2_protocol_trace_status_t wrapped = status();
    assert(wrapped.count == NS2_PROTOCOL_TRACE_CAPACITY);
    assert(wrapped.overwritten == 3);
    assert(wrapped.next_sequence == NS2_PROTOCOL_TRACE_CAPACITY + 3u);
    assert(ns2_protocol_trace_get(0, &record));
    assert(record.sequence == 3 && record.payload[0] == 3);
    assert(ns2_protocol_trace_get(NS2_PROTOCOL_TRACE_CAPACITY - 1u, &record));
    assert(record.sequence == NS2_PROTOCOL_TRACE_CAPACITY + 2u);

    ns2_protocol_trace_set_enabled(false);
    ns2_protocol_trace_clear();
    ns2_protocol_trace_status_t cleared = status();
    assert(!cleared.enabled && cleared.count == 0 && cleared.overwritten == 0);
    assert(cleared.next_sequence == 0);

    puts("ns2_protocol_trace: all tests passed");
    return 0;
}
