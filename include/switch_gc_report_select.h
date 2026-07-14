/*
 * Pure decision logic for the NSO GameCube 0x03/0x0A "Select Input Report" command. Zero
 * pico-sdk/TinyUSB dependency, host-compilable and host-tested (see
 * tools/test_switch_gc_report_select.c), mirroring hid_out_normalize.h/.c's own pattern.
 */
#ifndef SWITCH_GC_REPORT_SELECT_H
#define SWITCH_GC_REPORT_SELECT_H

#include <stdint.h>

#define GC_REPORT_ID_NONE 0

// Given the currently-armed report ID and a 0x03/0x0A command's payload (`c`/`n` as passed to
// the vendor bulk dispatcher; `c[8]` is the requested report ID per ndeadly's documented
// request layout: "0x0 (1B) Report ID, 0x1 (3B) unused"), return the report ID that should be
// armed afterward. Two supported values, both Confirmed by direct hardware evidence (see
// docs/switch2-gc/protocol.md "Input Report 0x05"/"Input Report 0x0A"): 0x05 (what PC/Steam
// hosts actually request) and 0x0A (GC's own native format, expected for a real console).
// Anything else leaves `current` unchanged -- matches ndeadly's documented "invalid report IDs
// are ignored" semantics.
uint8_t switch_gc_select_report(uint8_t current, const uint8_t *c, uint32_t n);

#endif  // SWITCH_GC_REPORT_SELECT_H
