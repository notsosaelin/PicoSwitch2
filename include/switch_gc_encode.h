/*
 * Pure NSO GameCube report-0x0A encoder -- zero pico-sdk/TinyUSB dependency,
 * host-compilable and host-tested (see tools/test_switch_gc_report.c),
 * mirroring usb_mode_cycle.h/.c's own pattern for the same reason.
 */
#ifndef SWITCH_GC_ENCODE_H
#define SWITCH_GC_ENCODE_H

#include <stdint.h>

#include "switch_pro.h"  // switch_pro_input_t, SWITCH_MASK_*/SWITCH_EXTRA_*/GC_MASK_*

// Construct the 63-byte report 0x0A input report body from an explicit input state and
// counter value. `out` must point to a buffer of at least 63 bytes and never includes the
// report ID (TinyUSB's tud_hid_n_report() prepends it separately). Layout Confirmed
// (docs/switch2-gc/protocol.md "Input Report 0x0A"); button-bitfield meaning Strong only.
// Native Z / independent L,R trigger detents / continuous analog L,R trigger are sourced
// from `in`'s gc_extra/left_trigger/right_trigger fields -- see include/switch_pro.h's
// GC_MASK_* comment and docs/switch2-gc/mapping.md "Internal normalized model requirements".
// Used by the real Switch 2 console; PC/Steam hosts use switch_gc_encode_report05() below.
void switch_gc_encode_report(const switch_pro_input_t *in, uint8_t counter, uint8_t out[63]);

// Construct the 63-byte report 0x05 input report body -- the common Switch-family format
// PC/Steam hosts actually select for this personality (Confirmed 2026-07-13, see the .c file's
// comment for the live-capture evidence). `counter` is a free-running 32-bit value (matches
// switch_pro2.c's own ns2_build_report_05() counter width, independent of report 0x0A's 8-bit
// one). Native GameCube Z and independent L/R trigger detents have no representable bit in this
// shared format and are simply omitted; continuous analog L/R trigger is carried in the
// GC-specific tail at offsets 0x3C/0x3D.
void switch_gc_encode_report05(const switch_pro_input_t *in, uint32_t counter, uint8_t out[63]);

#endif  // SWITCH_GC_ENCODE_H
