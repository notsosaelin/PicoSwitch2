#ifndef ANDROID_CONTROLLER_HID_H
#define ANDROID_CONTROLLER_HID_H

#include <stdint.h>

// ============================================================================
// PicoSwitch2 Android Controller Bridge — canonical HID contract
// ============================================================================
//
// ============================================================================
// BRIDGE CONTRACT VERSION -- READ BEFORE EDITING THE DESCRIPTOR
// ============================================================================
//
// One integer identifying the bridge contract this build implements. Both ends
// report it at runtime so a version skew is visible immediately instead of being
// inferred from which features stopped working.
//
// ## Why this exists (real incident, 2026-08-15)
//
// C/GameChat changed the descriptor from 14 buttons + 2 pad bits to 15 + 1.
// The companion APK was updated; the adapter kept running older firmware.
// android_bridge_identify() does an EXACT 161-byte match, so it failed and the
// firmware fell back to the v1 generic profile. Buttons, sticks, triggers and
// the hat kept working -- they are v1 fields -- while battery, motion and
// rumble/player-LED all disappeared together, because every one of them is
// gated on that single match. Source-level parity checks all passed: they
// compare the source tree to the source tree and cannot see what is flashed.
//
// ## WHEN TO BUMP
//
// Bump ANDROID_BRIDGE_CONTRACT_VERSION for any change a peer can observe:
//
//   * descriptor bytes of any kind (report count, usage minimum/maximum, field
//     width, logical range, collection structure, report IDs);
//   * wire layout (offsets, field sizes, endianness);
//   * units or semantics of an existing field (e.g. the motion timestamp moving
//     from milliseconds to 100 us ticks);
//   * capabilities implied by the profile (adding/removing motion, battery,
//     output);
//   * the output report's contents or meaning.
//
// Do NOT bump for implementation-only changes: comments, formatting, internal
// refactors, test edits, or anything that leaves the bytes on the wire and their
// meaning identical.
//
// Bumping is cheap. Failing to bump costs a debugging session like the one above.
//
// ## HOW THIS IS ENFORCED
//
// A SHA-256 over all 161 descriptor bytes is registered per contract version in
// BridgeContract.DESCRIPTOR_DIGESTS. Change ANY byte and the digest moves, so a
// change without a bump fails:
//
//   * tools/check_android_descriptor_parity.py  (this C descriptor)
//   * BridgeContractTest                        (the Kotlin mirror)
//
// Language-to-language parity alone is not enough: editing both sides together
// keeps them equal while still changing the wire. The digest is what catches
// that. Both checks print the digest to register and remind you to reflash.
//
// ## History
//
//   1  v1: buttons 1..14, sticks, triggers, hat. No vendor extension.
//   2  v2: vendor extension added -- motion, battery, flags, millisecond motion
//      timestamp -- plus output report 2 (rumble, player LED, motion-wanted).
//   3  buttons 1..15 (15 = C / GameChat, inside the same two bytes) and the
//      motion timestamp redefined to 100 us ticks.
//   4  current: buttons 1..17 (16 = GL, 17 = GR -- the Pro Controller 2 grip
//      buttons). Unlike C, these did NOT fit: contract 3 had exactly one pad bit
//      left and two buttons were needed, so the button field grew from two bytes
//      to three and EVERY LATER FIELD MOVED BY ONE BYTE. That is the first
//      offset-shifting change since v2, which is why the digest guard matters
//      here more than it ever has -- a v3 app against v4 firmware would not
//      merely lose a feature, it would read motion and battery from the wrong
//      offsets. The exact-match identify makes that impossible: a v3 descriptor
//      simply is not this one, so the firmware falls back to the v1 profile and
//      the version mismatch is reported.
//
// The Kotlin side mirrors this as BridgeContract.VERSION and
// tools/check_android_descriptor_parity.py fails the build if the two disagree.
#define ANDROID_BRIDGE_CONTRACT_VERSION 4u

// Single source of truth for the descriptor and wire layout. The firmware parser
// test (tools/test_bthid_android_controller.c) compiles this directly; the Kotlin
// encoder (android/companion/.../controller/ControllerState.kt) mirrors the same
// bytes and is pinned against these values by its own golden tests.
//
// v1 (buttons/sticks/triggers/hat) is hardware-validated end to end on an AYN
// Thor. v2 ADDS a vendor-defined extension for feature parity with a real
// controller — motion, battery, rumble, and player LED — and is designed so the
// v1 field positions are byte-identical and cannot regress:
//
//   * The extension is APPENDED to input report 1 after the v1 fields. The
//     firmware parses the descriptor with a real HID parser that computes each
//     item's bit offset, so the v1 axis/button/hat locations are unchanged and
//     the accepted report length adapts automatically. A v1 app (9-byte payload,
//     v1 descriptor) therefore keeps working against v2 firmware with no version
//     negotiation: capability is discovered from the descriptor, not assumed.
//   * The extension lives on a vendor-defined usage page, so the generic
//     Generic-Desktop/Button parse ignores it. Its presence IS the bridge's
//     identity — the firmware enables motion/battery ingest and rumble/LED output
//     only for a device that declares this block. That is deliberately NOT keyed
//     on Bluetooth VID/PID: an Android handheld reports its phone identity, which
//     varies per OEM and cannot authorize output.
//
// Wire layout, INPUT report 1 (Android -> PicoSwitch2), 27 bytes:
//   [0]      report ID (1)
//   [1..6]   X, Y, Z, Rz, Rx, Ry           (sticks, then triggers; 0..255)   v1
//   [7..9]   buttons 1..17 + 7 pad bits                                        v4
//   [10]     hat (low nibble, 8 = neutral) + 4 pad bits                       v1
//   [11..22] gyro X,Y,Z then accel X,Y,Z   (int16 little-endian)              v2
//   [23]     battery level 0..100                                             v2
//   [24]     flags (see ANDROID_BRIDGE_FLAG_*)                                v2
//   [25..26] motion timestamp, 100 us ticks (uint16 LE, free-running/wrapping) v2
//
// Button usage 15 is the Switch 2 "C" (GameChat) button. It was appended inside
// the two button bytes contract 2 already had -- 14 buttons + 2 pad became 15 +
// 1 -- so every later field kept its byte offset and only the button count
// changed. The firmware needs no change for it: the generic sequential profile
// already maps usage 15 to JP_BUTTON_A3, which NS2_BASE_BUTTON_MAP routes to
// NS2_DST_C and ns2_seam.c raises as SWITCH_EXTRA_C.
//
// Button usages 16 and 17 are the Pro Controller 2 GRIP buttons, GL and GR.
// They are real inputs on that controller (switch_pro2_encode.c writes them to
// report 0x09 byte2 0x08/0x04, and ns2_build_report_05 to byte 0x7 0x02/0x01),
// and until contract 4 the bridge had no way to carry them at all. Adding them
// is what forced the field to three bytes: contract 3 had ONE pad bit left.
//
// Their destinations already existed and are reused verbatim rather than
// invented here -- NS2_BASE_BUTTON_MAP already routes JP_BUTTON_A4 to
// NS2_DST_GL and JP_BUTTON_A5 to NS2_DST_GR, and ns2_kbm.c raises those as
// SWITCH_EXTRA_GL/GR. The only firmware change is the bridge's own button map
// (gamepad_quirks_android_bridge), which names usages 16/17 as A4/A5. It is
// deliberately NOT the shared SEQ_BUTTON_MAP: that table is used by every
// generic pad, and a device declaring 17 buttons for its own reasons must not
// suddenly acquire grip presses.
//
// Wire layout, OUTPUT report 2 (PicoSwitch2 -> Android), 5 bytes:
//   [0] report ID (2)
//   [1] rumble left amplitude  0..255
//   [2] rumble right amplitude 0..255
//   [3] player LED: 0 = none/off, 1..8 = console player number
//   [4] flags (bit0 = motion wanted; the app may idle its sensors when clear)
//
// Motion units (chosen to match what the seam already expects from a DualSense,
// so the Android source reuses the hardware-validated translation path):
//   accel: 8192 counts per g      (ns2_motion_seam_apply halves it to the
//                                  genuine Pro2 carrier's 4096 counts/g)
//   gyro:  16.384 counts per dps  (passes 1:1 end to end)
// The app converts Android SI sensor units with the two constants below.

#define ANDROID_CONTROLLER_REPORT_ID 0x01u
#define ANDROID_CONTROLLER_OUTPUT_REPORT_ID 0x02u

// v1 payload/wire lengths (retained: the v1 descriptor and any v1 app remain valid).
#define ANDROID_CONTROLLER_PAYLOAD_LEN 9u
#define ANDROID_CONTROLLER_WIRE_REPORT_LEN (1u + ANDROID_CONTROLLER_PAYLOAD_LEN)

// v2 payload/wire lengths. One byte longer since contract 4: the button field
// is three bytes, not two.
#define ANDROID_CONTROLLER_V2_PAYLOAD_LEN 26u
#define ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN (1u + ANDROID_CONTROLLER_V2_PAYLOAD_LEN)
#define ANDROID_CONTROLLER_OUTPUT_PAYLOAD_LEN 4u
#define ANDROID_CONTROLLER_OUTPUT_WIRE_LEN (1u + ANDROID_CONTROLLER_OUTPUT_PAYLOAD_LEN)

// Wire offsets of the v2 extension, INCLUDING the leading report-ID byte.
// All shifted by one at contract 4; see the wire layout above.
#define ANDROID_BRIDGE_OFF_GYRO 11u    // 3 x int16 LE
#define ANDROID_BRIDGE_OFF_ACCEL 17u   // 3 x int16 LE
#define ANDROID_BRIDGE_OFF_BATTERY 23u
#define ANDROID_BRIDGE_OFF_FLAGS 24u
// uint16 LE, 100 us ticks, free-running and wrapping (6.5536 s period).
// NOT milliseconds: at the 125 Hz report cadence a 1 ms quantum is 12.5% of the
// interval, which is the same order as the arrival jitter this timestamp exists
// to eliminate. 100 us keeps the quantization an order of magnitude below the
// effect being corrected while still fitting the existing 16-bit field.
#define ANDROID_BRIDGE_OFF_TIMESTAMP 25u
#define ANDROID_BRIDGE_TIMESTAMP_TICK_US 100u

// Input flags (wire byte 24).
#define ANDROID_BRIDGE_FLAG_CHARGING 0x01u
#define ANDROID_BRIDGE_FLAG_MOTION_VALID 0x02u
#define ANDROID_BRIDGE_FLAG_BATTERY_VALID 0x04u

// Output flags (wire byte 4).
#define ANDROID_BRIDGE_OUT_FLAG_MOTION_WANTED 0x01u

// Vendor-defined usage page and usages that identify this bridge.
#define ANDROID_BRIDGE_USAGE_PAGE 0xFF00u
#define ANDROID_BRIDGE_USAGE_GYRO_X 0x20u
#define ANDROID_BRIDGE_USAGE_ACCEL_X 0x23u
#define ANDROID_BRIDGE_USAGE_BATTERY 0x30u
#define ANDROID_BRIDGE_USAGE_FLAGS 0x31u
#define ANDROID_BRIDGE_USAGE_TIMESTAMP 0x32u
#define ANDROID_BRIDGE_USAGE_RUMBLE_LEFT 0x40u
#define ANDROID_BRIDGE_USAGE_RUMBLE_RIGHT 0x41u
#define ANDROID_BRIDGE_USAGE_PLAYER_LED 0x42u
#define ANDROID_BRIDGE_USAGE_OUT_FLAGS 0x43u

// Sensor conversion (see "Motion units" above). Exact, so both sides agree.
#define ANDROID_BRIDGE_ACCEL_COUNTS_PER_G 8192.0
#define ANDROID_BRIDGE_GYRO_COUNTS_PER_DPS 16.384
// Convenience factors for Android's SI sensor units.
#define ANDROID_BRIDGE_ACCEL_COUNTS_PER_MS2 (ANDROID_BRIDGE_ACCEL_COUNTS_PER_G / 9.80665)
#define ANDROID_BRIDGE_GYRO_COUNTS_PER_RAD_S \
    (ANDROID_BRIDGE_GYRO_COUNTS_PER_DPS * 57.2957795)
// Ranges published with the event so downstream consumers can reason about scale.
#define ANDROID_BRIDGE_ACCEL_RANGE_MILLI_G 4000u
#define ANDROID_BRIDGE_GYRO_RANGE_DPS 2000u

// Canonical v1 descriptor (unchanged; retained for the compatibility test).
static const uint8_t ANDROID_CONTROLLER_HID_DESCRIPTOR[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, ANDROID_CONTROLLER_REPORT_ID,

    0x09, 0x30,       // X
    0x09, 0x31,       // Y
    0x09, 0x32,       // Z
    0x09, 0x35,       // Rz
    0x09, 0x33,       // Rx
    0x09, 0x34,       // Ry
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xFF, 0x00, // Logical Maximum (255)
    0x75, 0x08,       // Report Size (8)
    0x95, 0x06,       // Report Count (6)
    0x81, 0x02,       // Input (Data, Variable, Absolute)

    0x05, 0x09,       // Usage Page (Button)
    0x19, 0x01,       // Usage Minimum (1)
    0x29, 0x0E,       // Usage Maximum (14)
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x0E,
    0x81, 0x02,
    0x75, 0x01,       // Two padding bits
    0x95, 0x02,
    0x81, 0x03,       // Input (Constant)

    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x39,       // Usage (Hat Switch)
    0x15, 0x00,
    0x25, 0x07,
    0x35, 0x00,
    0x46, 0x3B, 0x01, // Physical Maximum (315 degrees)
    0x65, 0x14,       // Unit (degrees)
    0x75, 0x04,
    0x95, 0x01,
    0x81, 0x42,       // Input (Data, Variable, Absolute, Null State)
    0x75, 0x04,       // Four padding bits
    0x95, 0x01,
    0x81, 0x03,
    0xC0
};

// Canonical v2 descriptor: v1 items byte-for-byte, then the vendor extension
// (motion/battery input) and the rumble/LED output report.
static const uint8_t ANDROID_CONTROLLER_V2_HID_DESCRIPTOR[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, ANDROID_CONTROLLER_REPORT_ID,

    0x09, 0x30,       // X
    0x09, 0x31,       // Y
    0x09, 0x32,       // Z
    0x09, 0x35,       // Rz
    0x09, 0x33,       // Rx
    0x09, 0x34,       // Ry
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x06,
    0x81, 0x02,

    0x05, 0x09,       // Usage Page (Button)
    0x19, 0x01,
    0x29, 0x11,       // Usage Maximum (17) -- 15 = C, 16 = GL, 17 = GR
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x11,       // Report Count (17)
    0x81, 0x02,
    0x75, 0x01,
    0x95, 0x07,       // seven pad bits; the field is three bytes wide
    0x81, 0x03,

    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x39,       // Usage (Hat Switch)
    0x15, 0x00,
    0x25, 0x07,
    0x35, 0x00,
    0x46, 0x3B, 0x01,
    0x65, 0x14,
    0x75, 0x04,
    0x95, 0x01,
    0x81, 0x42,
    0x75, 0x04,
    0x95, 0x01,
    0x81, 0x03,

    // ---- v2 vendor extension: motion (6 x int16), battery, flags, timestamp ----
    0x06, 0x00, 0xFF, // Usage Page (Vendor Defined 0xFF00)
    0x65, 0x00,       // Unit (None) -- clear the hat's degrees unit
    0x09, 0x20,       // Gyro X
    0x09, 0x21,       // Gyro Y
    0x09, 0x22,       // Gyro Z
    0x09, 0x23,       // Accel X
    0x09, 0x24,       // Accel Y
    0x09, 0x25,       // Accel Z
    0x16, 0x00, 0x80, // Logical Minimum (-32768)
    0x26, 0xFF, 0x7F, // Logical Maximum (32767)
    0x75, 0x10,       // Report Size (16)
    0x95, 0x06,       // Report Count (6)
    0x81, 0x02,       // Input (Data, Variable, Absolute)

    0x09, 0x30,       // Battery level
    0x09, 0x31,       // Flags
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x02,
    0x81, 0x02,

    0x09, 0x32,       // Motion timestamp (100 us ticks)
    0x15, 0x00,
    0x27, 0xFF, 0xFF, 0x00, 0x00, // Logical Maximum (65535)
    0x75, 0x10,
    0x95, 0x01,
    0x81, 0x02,

    // ---- v2 output report: rumble + player LED + flags ----
    0x85, ANDROID_CONTROLLER_OUTPUT_REPORT_ID,
    0x09, 0x40,       // Rumble left
    0x09, 0x41,       // Rumble right
    0x09, 0x42,       // Player LED
    0x09, 0x43,       // Flags
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x04,
    0x91, 0x02,       // Output (Data, Variable, Absolute)

    0xC0
};

static const uint8_t ANDROID_CONTROLLER_NEUTRAL_REPORT[ANDROID_CONTROLLER_WIRE_REPORT_LEN] = {
    ANDROID_CONTROLLER_REPORT_ID,
    128, 128, 128, 128, // sticks
    0, 0,               // triggers
    0, 0,               // buttons 1..14
    8                    // neutral hat + padding
};

static const uint8_t
ANDROID_CONTROLLER_V2_NEUTRAL_REPORT[ANDROID_CONTROLLER_V2_WIRE_REPORT_LEN] = {
    ANDROID_CONTROLLER_REPORT_ID,
    128, 128, 128, 128, // sticks
    0, 0,               // triggers
    0, 0, 0,            // buttons 1..17 (three bytes since contract 4)
    8,                  // neutral hat + padding
    0, 0, 0, 0, 0, 0,   // gyro X,Y,Z
    0, 0, 0, 0, 0, 0,   // accel X,Y,Z
    0,                  // battery level
    0,                  // flags (no motion, no battery)
    0, 0                // timestamp
};

#endif // ANDROID_CONTROLLER_HID_H
