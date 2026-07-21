/*
 * Bounded, opt-in console protocol trace storage.
 *
 * Producers run on USB/core0 and only copy one fixed-size record into RAM.
 * UART formatting and transmission happen later, outside the USB callbacks.
 */
#ifndef NS2_PROTOCOL_TRACE_H
#define NS2_PROTOCOL_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS2_PROTOCOL_TRACE_CAPACITY 128u
#define NS2_PROTOCOL_TRACE_PAYLOAD_MAX 24u

typedef enum {
    NS2_TRACE_EP0_SETUP = 1,
    NS2_TRACE_EP0_RESPONSE,
    NS2_TRACE_BULK_COMMAND,
    NS2_TRACE_BULK_RESPONSE,
    NS2_TRACE_HID_OUTPUT,
} ns2_protocol_trace_kind_t;

typedef enum {
    NS2_TRACE_CONSOLE_TO_DEVICE = 0,
    NS2_TRACE_DEVICE_TO_CONSOLE = 1,
} ns2_protocol_trace_direction_t;

typedef struct {
    uint32_t sequence;
    uint32_t timestamp_us;
    uint16_t total_length;
    uint8_t personality;
    uint8_t kind;
    uint8_t direction;
    uint8_t id;
    uint8_t subcommand;
    uint8_t captured_length;
    uint8_t payload[NS2_PROTOCOL_TRACE_PAYLOAD_MAX];
} ns2_protocol_trace_record_t;

typedef struct {
    bool enabled;
    uint16_t count;
    uint16_t capacity;
    uint32_t overwritten;
    uint32_t next_sequence;
} ns2_protocol_trace_status_t;

#ifdef NS2_UART_DIAG

void ns2_protocol_trace_clear(void);
void ns2_protocol_trace_set_enabled(bool enabled);
void ns2_protocol_trace_get_status(ns2_protocol_trace_status_t *status);

// `id` and `subcommand` are kind-specific summary fields. The complete packet
// prefix is retained in payload, along with its original (possibly larger)
// total length. Core0-only by design; no locks or blocking operations.
void ns2_protocol_trace_record(uint32_t timestamp_us, uint8_t personality,
                               ns2_protocol_trace_kind_t kind,
                               ns2_protocol_trace_direction_t direction,
                               uint8_t id, uint8_t subcommand,
                               const uint8_t *payload, size_t length);

// Read a stable oldest-first index. Callers must stop tracing before walking a
// multi-record snapshot; the UART `trace dump` command enforces that rule.
bool ns2_protocol_trace_get(uint16_t oldest_first_index,
                            ns2_protocol_trace_record_t *record);

#else

static inline void ns2_protocol_trace_clear(void) {}
static inline void ns2_protocol_trace_set_enabled(bool enabled) { (void)enabled; }
static inline void ns2_protocol_trace_get_status(ns2_protocol_trace_status_t *status) {
    if (!status) return;
    status->enabled = false;
    status->count = 0;
    status->capacity = NS2_PROTOCOL_TRACE_CAPACITY;
    status->overwritten = 0;
    status->next_sequence = 0;
}
static inline void ns2_protocol_trace_record(
    uint32_t timestamp_us, uint8_t personality, ns2_protocol_trace_kind_t kind,
    ns2_protocol_trace_direction_t direction, uint8_t id, uint8_t subcommand,
    const uint8_t *payload, size_t length) {
    (void)timestamp_us;
    (void)personality;
    (void)kind;
    (void)direction;
    (void)id;
    (void)subcommand;
    (void)payload;
    (void)length;
}
static inline bool ns2_protocol_trace_get(uint16_t oldest_first_index,
                                           ns2_protocol_trace_record_t *record) {
    (void)oldest_first_index;
    (void)record;
    return false;
}

#endif

#endif  // NS2_PROTOCOL_TRACE_H
