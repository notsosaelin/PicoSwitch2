// bthid_android_bridge.h - PicoSwitch2 Android Controller Bridge HID extension.
//
// The Android companion app presents an ordinary Bluetooth HID gamepad (buttons,
// sticks, triggers, hat) and additionally declares a vendor-defined extension
// that carries the remaining controller features: motion, battery, rumble, and
// player LED. See tools/fixtures/android_controller_hid.h for the canonical
// descriptor and wire layout, and docs/bluetooth/android-controller-bridge.md.
//
// Why this is NOT a gamepad quirk: quirks describe a device whose report layout
// deviates from what its descriptor implies, and they are selected by VID/PID or
// name. This is the opposite -- a device that HONESTLY DECLARES extra capability
// in its descriptor. Detection is therefore descriptor-driven and identity-free,
// which matters because an Android handheld reports its own phone VID/PID (it
// varies per OEM and cannot authorize output). Presence of the vendor block is
// the capability; nothing here keys on Bluetooth identity.
//
// Everything in this module is pure: parsing fills a map from descriptor items,
// extraction reads a byte buffer into a normalized input_event_t, and the
// feedback encoder produces an output payload. That keeps it host-testable
// (tools/test_bthid_android_bridge.c) without BTstack or hardware.
#ifndef BTHID_ANDROID_BRIDGE_H
#define BTHID_ANDROID_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/input_event.h"

// Maximum feedback payload emitted by android_bridge_encode_feedback(). Exposed
// so callers can size a stack buffer without pulling in the fixture header; the
// implementation static-asserts it against the canonical contract.
#define ANDROID_BRIDGE_FEEDBACK_MAX_LEN 4u

// Descriptor-derived location map for the vendor extension. All offsets are wire
// byte offsets INCLUDING the leading report-ID byte, matching how the generic
// driver already records byteIndex.
typedef struct {
    bool has_motion;        // gyro + accel fields all located
    bool has_battery;       // battery + flags fields located
    bool has_timestamp;
    bool has_output;        // rumble/LED output report declared
    uint8_t gyro_offset;    // 3 x int16 little-endian
    uint8_t accel_offset;   // 3 x int16 little-endian
    uint8_t battery_offset;
    uint8_t flags_offset;
    uint8_t timestamp_offset;
    uint8_t output_report_id;
    uint8_t output_len;     // payload length, excluding the report ID
    // Full wire length of the extended input report. The shared HID parser only
    // measures the fields it admits (the v1 ones), so the driver must enforce
    // this instead, or a report truncated inside the extension would be accepted
    // as a complete snapshot.
    uint8_t min_report_len;
} android_bridge_ext_t;

// Per-connection runtime state (motion sequence continuity across reports).
typedef struct {
    uint32_t sequence;
    uint16_t last_timestamp;
    bool has_last;
} android_bridge_state_t;

// True when the descriptor declared enough of the extension to be usable.
static inline bool android_bridge_ext_present(const android_bridge_ext_t *ext) {
    return ext && (ext->has_motion || ext->has_battery || ext->has_output);
}

// Identify the bridge from the raw HID descriptor and populate `out` with the
// contract's field offsets. Returns true only for an EXACT match against the
// canonical v2 descriptor.
//
// Why exact-match instead of walking parsed descriptor items: the shared HID
// parser filters items to a small allowlist of usage pages and has a fixed
// 50-entry pool. Widening that filter to admit a vendor page would apply to
// EVERY Bluetooth controller, and a device whose own vendor blob happened to use
// these usages could exhaust the pool and break its parse. Both ends of this
// bridge ship together and the descriptor is versioned, so an exact match is
// both sufficient and has zero blast radius on other devices. A future v3
// descriptor is simply another accepted constant.
bool android_bridge_identify(const uint8_t *descriptor, uint16_t descriptor_len,
                             android_bridge_ext_t *out);

// Identification trace.
//
// Battery, motion, rumble and the player LED are ALL gated on this one exact
// match, so "none of the v2 features work but buttons do" is the single symptom
// that identification failed. Inferring that from missing feedback is indirect
// and was misleading in practice: it cannot distinguish "never called",
// "rejected on length", "rejected on content", and "matched but the console
// never asked for anything". This records the answer directly.
//
// `first_mismatch` is -1 unless a content rejection occurred, in which case it
// is the byte offset that differed, with the expected and received values. That
// turns a silent capability loss into a one-line diff.
typedef struct {
    uint32_t calls;            // identify attempts
    uint32_t matched;          // exact matches -> v2 profile active
    uint32_t rejected_null;    // null descriptor
    uint32_t rejected_length;  // wrong descriptor length
    uint32_t rejected_content; // right length, wrong bytes
    uint16_t last_len;         // length of the most recent descriptor seen
    uint16_t expected_len;     // length this firmware requires
    int32_t first_mismatch;    // byte offset, or -1
    uint8_t expected_byte;
    uint8_t actual_byte;
    uint8_t active_profile;    // 0 = none yet, 1 = v1 generic, 2 = v2 bridge
} android_bridge_identify_trace_t;

const android_bridge_identify_trace_t *android_bridge_identify_trace(void);
void android_bridge_identify_trace_reset(void);

// Populate motion/battery on an already button/axis-populated event. Silently
// does nothing when the extension is absent or the report is too short, so a v1
// app (no extension, 10-byte report) is unaffected.
void android_bridge_extract(const android_bridge_ext_t *ext,
                            android_bridge_state_t *state,
                            const uint8_t *data, uint16_t len,
                            input_event_t *event);

// Encode the outbound feedback payload (rumble + player LED + flags). Returns the
// payload length written, or 0 when the extension declares no output report.
// `player_led` is 0 for none, otherwise the console player number (1..8).
uint8_t android_bridge_encode_feedback(const android_bridge_ext_t *ext,
                                       uint8_t rumble_left,
                                       uint8_t rumble_right,
                                       uint8_t player_led,
                                       bool motion_wanted,
                                       uint8_t *out, uint8_t out_capacity);

#endif  // BTHID_ANDROID_BRIDGE_H
