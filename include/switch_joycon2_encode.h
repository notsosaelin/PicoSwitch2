/*
 * Pure Joy-Con 2 report encoder -- zero pico-sdk/TinyUSB dependency,
 * host-compilable, mirroring switch_gc_encode.h's own pattern.
 */
#ifndef SWITCH_JOYCON2_ENCODE_H
#define SWITCH_JOYCON2_ENCODE_H

#include <stdint.h>

#include "switch_pro.h"  // switch_pro_input_t, SWITCH_MASK_*/SWITCH_EXTRA_*

// Which physical side this personality is presenting as. Confirmed distinct USB identities
// (docs/switch2-joycon2/protocol.md "USB identity" -- PID 0x2067 = Left, 0x2066 = Right) and
// distinct wire report IDs (7 = Left, 8 = Right, both Confirmed from a real USBPcap capture
// cross-validated against ndeadly/switch2_controller_research's own captures). There is
// deliberately no third "both sides merged" value: a genuine wired Joy-Con pair is a real USB hub
// (the Charging Grip) with two independently-addressed child devices, and this project's single
// Pico USB peripheral can only hold one USB address at a time (confirmed at the register level --
// see that document's "Why not simultaneous L+R") -- so this enum only ever represents plain
// single-side operation, by design, not as an interim step toward something more.
typedef enum {
    JOYCON2_SIDE_LEFT = 0,
    JOYCON2_SIDE_RIGHT,
} joycon2_side_t;

// Construct the 63-byte report 0x07 (Left) / 0x08 (Right) input report body -- the
// console-facing "extended" report, per side. `out` must point to a buffer of at least 63
// bytes and never includes the report ID (TinyUSB's tud_hid_n_report() prepends it separately).
// Layout Confirmed (docs/switch2-joycon2/protocol.md "Wire input/output report contents", sourced
// from ndeadly/switch2_controller_research's hid_reports.md, cross-checked against real decrypted
// BLE captures). Button bitfield meaning Confirmed per side. Mouse data, NFC state, and motion
// data are always zero/absent -- this project has no source data for any of them yet.
// `source_buttons` is the normalized physical JP_BUTTON_* bitmap, deliberately bypassing the
// configurable Pro2 semantic remap for the explicit sideways face/shoulder/trigger layout.
// Capture and C/GameChat still come from `in` so their configured Pro2 sources are preserved.
void switch_joycon2_encode_report(const switch_pro_input_t *in, uint32_t source_buttons,
                                   joycon2_side_t side, uint8_t counter, uint8_t out[63]);

// Construct the 63-byte report 0x05 input report body -- the common Switch-family format shared
// by every Switch 2 controller type (Confirmed, ndeadly's hid_reports.md "Input Report 0x05" --
// same table this project's own switch_gc_encode_report05() already implements a subset of).
// `counter` is a free-running 32-bit value. Populates the side-specific L/R/ZL/ZR bits this
// project's existing GameCube encoder leaves at zero (a lone Joy-Con genuinely has one of each).
// Uses the same sideways translation as report 0x07/0x08.
void switch_joycon2_encode_report05(const switch_pro_input_t *in, uint32_t source_buttons,
                                     joycon2_side_t side, uint32_t counter, uint8_t out[63]);

#endif  // SWITCH_JOYCON2_ENCODE_H
