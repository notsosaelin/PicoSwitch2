#ifndef NS2_VIRTUAL_NFC_RUNTIME_H
#define NS2_VIRTUAL_NFC_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns2_virtual_nfc.h"

typedef enum {
    NS2_VIRTUAL_NFC_EVENT_NONE = 0,
    NS2_VIRTUAL_NFC_EVENT_SCAN_READY,
    NS2_VIRTUAL_NFC_EVENT_OPERATION_READY,
    NS2_VIRTUAL_NFC_EVENT_WRITE_COMPLETE,
    NS2_VIRTUAL_NFC_EVENT_ERROR,
} ns2_virtual_nfc_event_t;

// Command-driven virtual reader state. There is no idle polling: delayed work
// is limited to one report-state transition after scan, begin, commit, or a
// rejected operation. A committed Stop emits removal immediately only when
// the write is already durable; otherwise removal waits for flash
// verification. The report field is an event counter modulo eight, not a
// fixed semantic state.
typedef struct {
    uint8_t operation_buffer[NS2_NFC_READ_BUFFER_SIZE];
    ns2_virtual_nfc_write_t write;
    uint32_t transition_due_ms;
    uint16_t operation_buffer_size;
    uint8_t report_state;
    uint8_t pending_report_state;
    uint8_t nfc_status;
    uint8_t nfc_detail;
    ns2_virtual_nfc_event_t pending_event;
    bool transition_pending;
    bool scan_active;
    bool operation_active;
    bool write_mode;
    bool write_committed;
    bool write_persisted;
    bool eject_waiting_for_persist;
    bool tag_was_present;
    // The browser-loaded image and its presentation to the console are
    // separate states. A completed write keeps the mutable image resident but
    // logically removes the tag; the next scan presents the selected image
    // again without discarding its console-written state.
    bool tag_ejected;
    // Random-UID presentation ("random amiibo mode"): each fresh scan
    // encounter overlays a new NTAG UID/BCC (and, when the source dump
    // carries one, a UID-derived PWD/PACK) onto the presented image so
    // per-tag game cooldowns treat it as a different physical amiibo. The
    // overlay stays stable within one encounter; the integration layer
    // discards console writes made under a random UID.
    bool randomize_uid;
    bool session_uid_valid;
    uint8_t session_uid[7];
    uint32_t uid_rng_state;
} ns2_virtual_nfc_runtime_t;

typedef struct {
    uint8_t payload[NS2_NFC_READ_CHUNK_PAYLOAD_SIZE];
    size_t payload_size;
    uint8_t response_direction;
    uint8_t write_record_count;
    uint16_t write_data_bytes;
    bool write_committed;
} ns2_virtual_nfc_response_t;

void ns2_virtual_nfc_runtime_init(ns2_virtual_nfc_runtime_t *runtime);
// Enable/disable random-UID presentation and mix caller entropy into the
// session RNG. Safe to call before every dispatch; toggling the mode
// invalidates the current session UID so the next application draws fresh.
void ns2_virtual_nfc_runtime_set_randomize_uid(
    ns2_virtual_nfc_runtime_t *runtime, bool enabled, uint32_t entropy);
void ns2_virtual_nfc_runtime_tick(ns2_virtual_nfc_runtime_t *runtime,
                                  uint32_t now_ms);
// The integration layer clears this immediately after an atomic console
// commit and sets it only after the dual-bank flash snapshot verifies. A Stop
// received in between is acknowledged but its TagRemoved edge is deferred.
void ns2_virtual_nfc_runtime_set_write_persisted(
    ns2_virtual_nfc_runtime_t *runtime, bool persisted);
uint8_t ns2_virtual_nfc_runtime_report_state(
    const ns2_virtual_nfc_runtime_t *runtime);

// If the selected image changes between the command snapshot and the atomic
// store update, the integration layer rejects the compare-and-swap and reports
// that failure back to this command state machine.
void ns2_virtual_nfc_runtime_write_apply_failed(
    ns2_virtual_nfc_runtime_t *runtime, uint32_t now_ms);

// Handles scan/status/read plus the capture-derived write surface:
// 0x06 selects read mode for an all-zero UID and write mode for the exact
// selected UID; 0x14 fills a bounded 454-byte staging buffer; 0x08 atomically
// validates and commits it. Unknown commands return false. Bare ACKs use
// direction 0x04; data replies use 0x01.
bool ns2_virtual_nfc_runtime_dispatch(
    ns2_virtual_nfc_runtime_t *runtime, uint32_t now_ms, uint8_t subcommand,
    const uint8_t *request, size_t request_size, bool tag_present,
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    const uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE],
    ns2_virtual_nfc_response_t *response);

#endif
