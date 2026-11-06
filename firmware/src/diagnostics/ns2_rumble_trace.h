#ifndef NS2_RUMBLE_TRACE_H
#define NS2_RUMBLE_TRACE_H

#include <stdbool.h>
#include <stdint.h>

// End-to-end observability for the console -> handheld rumble path.
//
// WHY THIS EXISTS
//
// The AYN Thor companion bridge has never produced any rumble. "Where does the
// signal disappear" is a chain of six questions, and answering them one flash
// at a time is expensive. This module records the two firmware-side ends of the
// chain so a single UART read (`rumble`) answers all of the firmware half at
// once:
//
//   console 0x02 output report -> ns2_hid_out_report() decode   [console_*]
//   report.c slot 0 -> feedback -> Android bridge output report [bridge_*]
//
// If console_nonzero is 0, the console never asked for rumble (or the report
// never arrived) and nothing downstream matters. If console_nonzero is nonzero
// but bridge_nonzero is 0, the value was decoded but never handed to the
// bridge -- look at input ownership (find_player_index) and the feedback
// generation. If bridge_sent advances while the handheld stays silent, the
// firmware half is complete and the remaining question is entirely on the
// Android side (see the companion app's own diagnostics).
//
// PURE OBSERVABILITY. Nothing here feeds back into rumble decode, the feedback
// state, or the transmitted report.
//
// CROSS-CORE: the console side is written from core0 (USB) and the bridge side
// from core1 (Bluetooth); both are read from core0. The fields are independent
// 32-bit counters and single bytes, so a torn read can only ever mix two
// adjacent samples of a diagnostic, never corrupt state. That is deliberately
// cheaper than a critical section on the USB output path.

typedef struct {
    // Console -> adapter.
    uint32_t console_reports;  // 0x02 output reports decoded
    uint32_t console_nonzero;  // ...of which carried a non-zero amplitude
    uint8_t console_left;      // most recently decoded amplitudes (0..255)
    uint8_t console_right;

    // Adapter -> Android companion bridge.
    uint32_t bridge_sent;      // feedback output reports successfully queued
    uint32_t bridge_failed;    // ...and ones the transport rejected
    uint32_t bridge_nonzero;   // sends carrying a non-zero amplitude
    uint8_t bridge_left;       // last values handed to the bridge
    uint8_t bridge_right;
    uint8_t bridge_player;
    uint8_t bridge_motion_wanted;
} ns2_rumble_trace_t;

// Called from ns2_hid_out_report() with the decoded per-motor amplitudes.
void ns2_rumble_trace_console(uint8_t left, uint8_t right);

// Called once per attempted Android-bridge feedback report, whether or not the
// transport accepted it.
void ns2_rumble_trace_bridge(uint8_t left, uint8_t right, uint8_t player,
                             bool motion_wanted, bool sent);

void ns2_rumble_trace_get(ns2_rumble_trace_t *out);
void ns2_rumble_trace_reset(void);

#endif  // NS2_RUMBLE_TRACE_H
