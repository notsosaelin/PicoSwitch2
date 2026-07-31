#ifndef NS2_AMIIBO_V3_RUNTIME_H
#define NS2_AMIIBO_V3_RUNTIME_H

// Host-replayable figure-v3 NFC state machine.
//
// This is the controller-facing command state machine that used to live as
// `ns2_v3_serve()` plus twenty file-scope statics inside
// src/switch_pro2/switch_pro2.c. There it could not be exercised without USB,
// flash, BTstack, or physical time, so every deterministic v3 bug -- the split
// 88-byte 0x14, the missing 0x1E transition, the stale capability generation,
// the fixed record pages -- had to be found on hardware.
//
// The 540-byte NTAG215 path has been replayable through
// ns2_virtual_nfc_runtime_dispatch() from the start. This module gives v3 the
// same shape: all state is in one caller-owned struct, time is a parameter, and
// the three side effects that reach flash/store are reached through a small
// host interface a test can fake.
//
// Nothing here owns USB framing, tracing, or transport. The caller supplies the
// image and sends the response.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns2_amiibo_v3.h"
#include "ns2_amiibo_v3_write.h"
#include "ns2_virtual_nfc.h"

#define NS2_AMIIBO_V3_PREFIX_SIZE 60u
#define NS2_AMIIBO_V3_DEVICE_RESULT_SIZE (19u + NS2_AMIIBO_V3_SRAM_SIZE)
#define NS2_AMIIBO_V3_WRITE_COMPLETE_MS 700u
#define NS2_AMIIBO_V3_REPLY_MAX 64u
// Source view selector: 0 = raw 2 KB image, 1 = NTAG215-compatibility view.
#define NS2_AMIIBO_V3_SERVE_MODE_MAX 1u

// Why an operation failed, independent of the console-facing status.
//
// A genuine controller reports one value for every failure -- status 0x07 with
// detail 0x41 -- and the console turns that into 2115-0096. During the v3
// investigation the identical pair was produced first by a real tag-removal
// timing bug and later by a fail-closed record-layout rejection, and the second
// was misdiagnosed as the first. The wire cannot distinguish them, so the
// firmware must.
typedef enum {
    NS2_AMIIBO_V3_ERROR_NONE = 0,
    // 0x06 read descriptor failed its structural gate, or arrived while the
    // tag was logically ejected.
    NS2_AMIIBO_V3_ERROR_READ_DESCRIPTOR,
    // 0x1E sector-aware descriptor rejected, or the result would not fit.
    NS2_AMIIBO_V3_ERROR_SECTOR_READ,
    // 0x14 arrived while ejected, or its header/declared length is unusable.
    NS2_AMIIBO_V3_ERROR_STAGE_FRAMING,
    // 0x14 arrived with no operation active, no envelope classified, or the
    // status not in the active state.
    NS2_AMIIBO_V3_ERROR_STAGE_NOT_ACTIVE,
    // The staging buffer rejected the chunk (range, conflict, or overrun).
    NS2_AMIIBO_V3_ERROR_STAGE_CHUNK,
    // 0x08 or 0x20 arrived without the preconditions its transaction needs.
    NS2_AMIIBO_V3_ERROR_COMMIT_STATE,
    // The staged envelope failed validation. `result` carries which rule.
    NS2_AMIIBO_V3_ERROR_COMMIT_VALIDATION,
    // Validation passed but the store refused the write, normally because a
    // portal upload replaced the selected image mid-transaction.
    NS2_AMIIBO_V3_ERROR_COMMIT_APPLY,
} ns2_amiibo_v3_error_t;

const char *ns2_amiibo_v3_error_string(ns2_amiibo_v3_error_t error);

// The three effects that reach durable state. Kept behind an interface so the
// command flow is byte-for-byte the same in firmware and in a host replay, and
// so a test can inject an apply failure or a pending flash write without
// touching real flash.
typedef struct {
    // Returns true when the store accepted the console write for `generation`.
    bool (*apply_console_write)(void *ctx,
                                const uint8_t image[NS2_AMIIBO_V3_SIZE],
                                uint32_t generation);
    void (*set_presented)(void *ctx, bool presented);
    bool (*persist_pending)(void *ctx);
    void *ctx;
} ns2_amiibo_v3_host_t;

typedef struct {
    // --- transaction state ---
    uint8_t report_state;      // event counter mod 8, not a semantic state
    uint8_t nfc_status;        // 09 ready, 04 active, 07 error, 15/16/18 results
    uint8_t nfc_detail;
    bool operation_active;
    bool device_cmd_staged;
    bool write_mode;
    bool extended_mode;
    size_t extended_expected_size;
    ns2_amiibo_v3_extended_sequence_t extended_sequence;
    ns2_virtual_nfc_write_t write;
    bool write_committed;
    bool write_persisted;
    bool eject_waiting_for_persist;
    bool tag_ejected;
    uint32_t represent_after_ms;
    bool write_event_pending;
    uint32_t write_event_due_ms;
    uint32_t operation_generation;
    uint32_t observed_generation;
    bool observed_generation_valid;

    // --- served buffer ---
    uint8_t op_buffer[NS2_AMIIBO_V3_SECTOR_READ_MAX_SIZE];
    size_t op_buffer_size;

    // --- diagnostics ---
    uint32_t device_cmd_staged_count;
    uint32_t device_result_count;
    uint32_t write_chunk_count;
    uint32_t write_commit_count;
    uint32_t write_error_count;
    uint32_t extended_chunk_count;
    uint32_t extended_completion_count;
    uint32_t error_count;
    ns2_amiibo_v3_error_t last_error;
    uint8_t last_error_sub;      // the subcommand that failed
    uint8_t last_error_result;   // ns2_virtual_nfc_result_t, when applicable
    uint16_t last_error_offset;  // stage offset, when applicable
    uint32_t last_error_ms;

    // --- tag signature (RAM-only; captured from a physical tag over UART) ---
    uint8_t signature[NS2_AMIIBO_V3_SIGNATURE_SIZE];
    bool signature_set;

    // --- reverse-engineering overlays ---
    // These deliberately live inside the runtime so a replay reproduces exactly
    // what hardware served, including whatever probe was active at the time.
    uint8_t serve_mode;
    uint8_t status_probe_value[NS2_NFC_STATUS_PAYLOAD_SIZE];
    uint8_t status_probe_mask[NS2_NFC_STATUS_PAYLOAD_SIZE];
    uint8_t hdr_probe_value[NS2_AMIIBO_V3_PREFIX_SIZE];
    uint8_t hdr_probe_mask[NS2_AMIIBO_V3_PREFIX_SIZE];
    uint8_t reply_sub;           // 0 = disabled
    uint8_t reply_len;
    uint8_t reply_data[NS2_AMIIBO_V3_REPLY_MAX];
} ns2_amiibo_v3_runtime_t;

typedef struct {
    uint8_t payload[NS2_NFC_READ_CHUNK_PAYLOAD_SIZE];
    size_t payload_size;
    uint8_t response_direction;  // 0x04 bare ACK, 0x01 data reply
} ns2_amiibo_v3_effects_t;

void ns2_amiibo_v3_runtime_init(ns2_amiibo_v3_runtime_t *runtime);
// Full transaction reset without discarding the report counter, the observed
// generation, the signature, or the probe overlays.
void ns2_amiibo_v3_runtime_reset_transaction(ns2_amiibo_v3_runtime_t *runtime);

// Handle one command 0x01 subcommand.
//
// `image` is the caller's mutable working copy of the selected 2048-byte tag;
// a committed write mutates it in place before the host is asked to apply it.
// `generation` is the store generation that copy came from: a change since the
// last call means fresh media, and any in-flight transaction is abandoned
// rather than allowed to commit over the newer image.
//
// Always fills `effects`. Returns true when a response should be sent, which is
// every recognized and unrecognized subcommand alike -- an unknown subcommand
// is bare-ACKed so the trace still shows it.
bool ns2_amiibo_v3_runtime_step(ns2_amiibo_v3_runtime_t *runtime,
                                const ns2_amiibo_v3_host_t *host,
                                uint32_t now_ms, uint32_t generation,
                                uint8_t subcommand, const uint8_t *request,
                                size_t request_size,
                                uint8_t image[NS2_AMIIBO_V3_SIZE],
                                ns2_amiibo_v3_effects_t *effects);

// Time-driven work with no command: the deferred write-complete report edge and
// the persist-gated eject. Called by step(), and separately by the caller when
// it wants the runtime to advance without traffic.
void ns2_amiibo_v3_runtime_tick(ns2_amiibo_v3_runtime_t *runtime,
                                const ns2_amiibo_v3_host_t *host,
                                uint32_t now_ms);

uint8_t ns2_amiibo_v3_runtime_report_state(
    const ns2_amiibo_v3_runtime_t *runtime);

// --- overlay accessors (used by the UART diagnostic surface) ---
bool ns2_amiibo_v3_runtime_set_signature(ns2_amiibo_v3_runtime_t *runtime,
                                         const uint8_t *bytes, size_t len);
bool ns2_amiibo_v3_runtime_status_probe_set(ns2_amiibo_v3_runtime_t *runtime,
                                            uint8_t index,
                                            const uint8_t *bytes, uint8_t len);
bool ns2_amiibo_v3_runtime_hdr_probe_set(ns2_amiibo_v3_runtime_t *runtime,
                                         uint8_t index, const uint8_t *bytes,
                                         uint8_t len);
bool ns2_amiibo_v3_runtime_set_reply(ns2_amiibo_v3_runtime_t *runtime,
                                     uint8_t sub, const uint8_t *data,
                                     uint8_t len);

#endif /* NS2_AMIIBO_V3_RUNTIME_H */
