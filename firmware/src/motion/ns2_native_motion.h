#ifndef NS2_NATIVE_MOTION_H
#define NS2_NATIVE_MOTION_H

#include <stdbool.h>
#include <stdint.h>

// A native Switch 2 controller's report-0x000E motion payload is already in the same
// variable-length representation accepted by the console-facing USB report 0x09. Keep that
// representation opaque: the bridge transports it byte-for-byte instead of pretending the
// quaternion/PDU format has been decoded.
#define NS2_NATIVE_MOTION_MAX_DATA 40

typedef struct {
    uint8_t length; // 0x1E or 0x28
    uint8_t data[NS2_NATIVE_MOTION_MAX_DATA];
    uint8_t source_counter;
    uint8_t source_conn_index;
    uint32_t source_generation;
    uint8_t source_verified;
    uint8_t held_after_disconnect;
    uint16_t source_vid;
    uint16_t source_pid;
    uint32_t captured_us;
    uint8_t hybrid_valid;
    uint8_t hybrid_mode;
    uint8_t hybrid_reason;
    uint8_t hybrid_reserved;
    uint32_t hybrid_groups;
    uint8_t hybrid_data[NS2_NATIVE_MOTION_MAX_DATA];
} ns2_native_motion_snapshot_t;

// Core1 producer: publish bytes beginning immediately after report byte 0x0E (the length byte).
// Invalid/truncated lengths are rejected. `report` is the complete 63-byte native BLE payload.
bool ns2_native_motion_publish(uint8_t source_conn_index,
                               uint16_t source_vid, uint16_t source_pid,
                               const uint8_t *report, uint16_t report_length,
                               uint32_t captured_us);

// Token-aware producer form for Bluetooth callbacks.  The legacy function
// above remains for non-Bluetooth fixtures/producers and carries generation 0.
bool ns2_native_motion_publish_generation(uint8_t source_conn_index,
                                          uint32_t source_generation,
                                          uint16_t source_vid,
                                          uint16_t source_pid,
                                          const uint8_t *report,
                                          uint16_t report_length,
                                          uint32_t captured_us);

// Core0 consumer: acquire one coherent snapshot if it is no older than max_age_us.
bool ns2_native_motion_snapshot(ns2_native_motion_snapshot_t *out, uint32_t now_us,
                                uint32_t max_age_us);

// Acquire the most recent genuine 0x1E sample even when a newer interleaved
// 0x28 PDU is the current general snapshot. Used by the opt-in UART mutation
// probe so latching is deterministic rather than timing-dependent.
bool ns2_native_motion_snapshot_30(ns2_native_motion_snapshot_t *out, uint32_t now_us,
                                   uint32_t max_age_us);

// Invalidate the side channel on source disconnect or before a new experiment starts.
void ns2_native_motion_clear(void);

// Source-off transition: retain the most recent genuine length-30 orientation as a stationary
// hold sample. This is distinct from clear(): the USB device remains connected to the console
// after its Bluetooth source powers off, so abruptly changing motion length to zero can leave the
// console extrapolating the last non-zero angular velocity.
void ns2_native_motion_source_disconnected(uint32_t disconnected_us);

// Apply the disconnect hold only when the stored native snapshot belongs to
// this exact source lifecycle.  Returns false for an old callback after index
// reuse, leaving the replacement source untouched.
bool ns2_native_motion_source_disconnected_generation(
    uint8_t source_conn_index,
    uint32_t source_generation,
    uint32_t disconnected_us);

// Convert BTstack's transport connection index to the same output slot policy used by ns2_seam.
// Classic indices 0..3 map directly; BLE indices begin at 4 and fold to slot 0 in the supported
// one-controller configuration.
uint8_t ns2_native_motion_output_slot(uint8_t source_conn_index);

#endif
