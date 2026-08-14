#ifndef NS2_INPUT_ARBITER_H
#define NS2_INPUT_ARBITER_H

// Deterministic, transport-independent ownership policy for the NS2 input seam.
//
// The Bluetooth stack may have several HID links alive at once, but the console
// personality has one authoritative input stream (slot 0).  This module owns
// the source registry and makes that one-owner rule testable without pulling in
// Pico SDK, BTstack, or the report cross-core seam.

#include <stdbool.h>
#include <stdint.h>

#define NS2_INPUT_ARBITER_MAX_SOURCES 8u
#define NS2_INPUT_SOURCE_NAME_MAX 32u
#define NS2_INPUT_SOURCE_ID_NONE 0u

// Source classes, ordered by automatic-selection preference.
//
// A controller paired directly to the adapter is what a user reaches for first,
// so it outranks the companion app's software bridge. The bridge exists to make
// a handheld usable when there is nothing else, not to take the console away
// from real hardware. This ordering is consulted ONLY while the user has not
// made an explicit choice; an explicit selection is never overridden.
// UNKNOWN is what a lifecycle hook registers before any report has arrived: the
// companion bridge is identified from its HID descriptor, which the connection
// hook cannot see. Ranking it lowest means an unidentified source can take an
// idle console but can never take one away from a source that has identified
// itself. The first report reclassifies it and ownership is re-evaluated then.
#define NS2_INPUT_SOURCE_CLASS_UNKNOWN 0u
#define NS2_INPUT_SOURCE_CLASS_BRIDGE 1u
#define NS2_INPUT_SOURCE_CLASS_DIRECT 2u

typedef struct {
    uint8_t transport;             // input_transport_t value, kept opaque here
    uint8_t dev_addr;              // transport connection index
    int8_t instance;               // driver instance within the connection
    uint8_t stable_addr[6];        // Bluetooth address when available
    uint8_t stable_addr_valid;
    uint32_t connection_generation; // monotonically assigned HID lifecycle token
} ns2_input_source_key_t;

typedef struct {
    ns2_input_source_key_t key;
    uint32_t id;                   // opaque UI/API handle; never reused in a boot
    uint32_t generation;           // registry generation for this live source
    uint16_t vendor_id;
    uint16_t product_id;
    char name[NS2_INPUT_SOURCE_NAME_MAX];
    uint8_t present;
    uint8_t source_class;          // NS2_INPUT_SOURCE_CLASS_*
} ns2_input_source_info_t;

typedef struct {
    uint8_t accepted;              // this source may publish to console slot 0
    uint8_t transition_applied;    // caller must have emitted a neutral boundary
    uint8_t fresh_report;          // first complete report after a selection
    // The arbiter changed owner by policy rather than by user request. It is a
    // pure object with no access to the report seam, so the caller owes the
    // console a neutral boundary when this is set, exactly as it does for an
    // explicit selection.
    uint8_t auto_switched;
} ns2_input_route_decision_t;

typedef struct {
    uint32_t active_id;
    uint32_t pending_id;
    uint32_t transition_count;
    uint8_t explicit_active;
    uint8_t awaiting_fresh;
    uint8_t source_count;
    ns2_input_source_info_t sources[NS2_INPUT_ARBITER_MAX_SOURCES];
} ns2_input_arbiter_status_t;

// This is intentionally a plain C object so host tests can instantiate several
// independent arbiters.  Firmware owns one static instance from ns2_active_input.c.
typedef struct {
    ns2_input_source_info_t sources[NS2_INPUT_ARBITER_MAX_SOURCES];
    uint32_t next_id;
    uint32_t next_generation;
    uint32_t active_id;
    uint32_t transition_count;
    uint32_t applied_request;
    uint8_t explicit_active;
    uint8_t awaiting_fresh;

    // A management command is produced on core 0 and consumed at the next
    // input boundary on core 1.  The bridge is deliberately one-command-wide.
    volatile uint32_t pending_id;
    volatile uint32_t pending_request;

    // Core 1 is the sole registry writer.  Core 0 status readers use this
    // seqlock to avoid observing half-written source metadata.
    volatile uint32_t status_sequence;
} ns2_input_arbiter_t;

void ns2_input_arbiter_init(ns2_input_arbiter_t *arbiter);

// Queue an explicit active-source request.  id=0 deliberately selects no
// source and leaves the console neutral.  The request is committed by
// ns2_input_arbiter_submit() at an input boundary.
bool ns2_input_arbiter_request_active(ns2_input_arbiter_t *arbiter,
                                      uint32_t id);

// Cross-core producer form.  Unlike request_active(), this only queues the
// token and deliberately does not inspect the core-1-owned registry.  Firmware
// validates the token from a seqlock status snapshot before calling it.
bool ns2_input_arbiter_queue_active(ns2_input_arbiter_t *arbiter,
                                    uint32_t id);

// Register/update a source and decide whether its complete report may publish.
//
// While the user has not made an explicit choice, ownership follows source class
// (see NS2_INPUT_SOURCE_CLASS_*): with nothing else connected the companion
// bridge takes the console, and a controller paired directly to the adapter
// takes it back automatically when one appears. Once the user selects a source
// explicitly, that choice is final and no arrival or departure overrides it.
bool ns2_input_arbiter_submit(ns2_input_arbiter_t *arbiter,
                              const ns2_input_source_key_t *key,
                              const char *name,
                              uint16_t vendor_id,
                              uint16_t product_id,
                              uint8_t source_class,
                              ns2_input_route_decision_t *decision);

// Remove exactly the source represented by key.  A key includes the lifecycle
// generation, so a recycled connection index cannot remove a newer source.
bool ns2_input_arbiter_disconnect(ns2_input_arbiter_t *arbiter,
                                  const ns2_input_source_key_t *key,
                                  bool *was_active);

// Resolve a live source key to its opaque id, or 0 when it is not registered.
uint32_t ns2_input_arbiter_source_id(const ns2_input_arbiter_t *arbiter,
                                     const ns2_input_source_key_t *key);

// Legacy fast read-only gate for report paths that carry only the transport's
// global connection index.  Token-aware callbacks must use the generation
// form below; this index-only form remains for non-Bluetooth/legacy callers.
bool ns2_input_arbiter_is_active_connection(const ns2_input_arbiter_t *arbiter,
                                            uint8_t dev_addr);

// Token-aware form used by callbacks that captured the HID lifecycle.  A zero
// generation retains the legacy index-only behavior for non-Bluetooth callers.
bool ns2_input_arbiter_is_active_connection_generation(
    const ns2_input_arbiter_t *arbiter,
    uint8_t dev_addr,
    uint32_t connection_generation);

// Snapshot the source registry for bounded diagnostics/UI responses.
void ns2_input_arbiter_get_status(const ns2_input_arbiter_t *arbiter,
                                  ns2_input_arbiter_status_t *status);

#endif  // NS2_INPUT_ARBITER_H
