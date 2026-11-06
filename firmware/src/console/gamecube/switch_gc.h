/*
 * NSO GameCube Controller (VID 057E / PID 2073) USB emulation.
 * Compiled/active only when the firmware is built with -DNS2_PRO (mirrors
 * switch_pro2.h's own scoping -- see usb.h's usb_personality_t: GameCube is a
 * runtime-selectable personality alongside Switch 2 Pro Controller 2, not a
 * separate build axis).
 *
 * Hardware-validated for enumeration, report 0x0A input, native controls,
 * vendor initialization, and rumble. Evidence and remaining questions:
 * docs/switch2-gc/{protocol,mapping,open-questions}.md.
 *
 * USB layout (Confirmed, byte-exact, cross-validated against two independent
 * genuine units -- see docs/switch2-gc/protocol.md "USB descriptor topology"):
 *   IF0 HID    - report 0x0A on EP 0x81 IN, report 0x03 (rumble) on EP 0x01 OUT
 *   IF1 Vendor - command/factory channel on EP 0x02 bulk OUT / 0x82 bulk IN
 */
#ifndef SWITCH_GC_H
#define SWITCH_GC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "switch_gc_encode.h"  // switch_gc_encode_report (pure, host-testable), switch_pro_input_t

// NS2_DIAG: GameCube USB handshake progress (core0), blinked on the LED by ns2_bt_host.c (core1)
// -- N rapid flashes, repeating, where N is the stage reached so far. Deliberately a SEPARATE
// variable from switch_pro2.h's g_ns2_stage (not reused), scoped to fire only while GameCube
// personality is active, per explicit instruction to keep the two personalities' flash counts
// from ever being confused with each other. Stage numbers (see switch_gc.c's own increment sites
// for the exact hook):
//   0 none (slow ~0.75s heartbeat) / 1 device descriptor read / 2 config descriptor read /
//   3 SET_CONFIGURATION / 4 first EP0 vendor identity request (bRequest 2/3/4) /
//   5-14 first vendor bulk command received, then 5+N where N = total distinct bulk commands
//   received so far (capped at 9) -- lets the flash count directly answer "how many commands did
//   the console send before it stopped", not just "at least one arrived" /
//   20 the console has sent at least one command 0x03 (Init USB / Select Input Report) family
//   command, regardless of whether its specific sub-command/report-id was recognized -- answers
//   "did it even attempt the final step" once 14 is reached /
//   21 the console specifically sent 0x03/sub 0x0A (Select Input Report), as opposed to only
//   ever sending other 0x03-family commands -- if g_gc_bad_report_id is also nonzero-meaningful
//   (see below) at this stage, that's the exact requested ID this dispatcher didn't accept /
//   255 full success sentinel (report selected, streaming armed) -- shown as a SOLID (not
//   counted) LED, never confusable with the 5-14/20/21 counting range.
extern volatile uint8_t g_gc_stage;

// NS2_DIAG: valid only when g_gc_stage == 21 -- the raw report-ID byte the console requested via
// 0x03/sub 0x0A that this dispatcher did not recognize (not 0x05 or 0x0A). Blinked as a two-digit
// tens/ones burst by ns2_bt_host.c, not the flat N-flash scheme above, so an arbitrary byte value
// can be read directly off the LED.
extern volatile uint8_t g_gc_bad_report_id;

// Descriptor accessors, used by usb_descriptors.c's personality dispatch.
const uint8_t *switch_gc_device_descriptor(void);
const uint8_t *switch_gc_config_descriptor(void);
const uint8_t *switch_gc_hid_report_descriptor(void);  // 97 bytes -- Confirmed, see .c for citation
const char   **switch_gc_string_table(size_t *count);

// Microsoft OS 1.0 string (index 0xEE) -- lets Windows auto-bind WinUSB to
// the vendor interface (IF1), added 2026-07-13 after hardware testing showed
// Windows otherwise reports error code 28 ("no compatible drivers") for the
// whole composite device. See the .c file's comment block for why this is a
// Stage B fix and not scope creep into Stage D.
const uint16_t *switch_gc_ms_os_string_descriptor(void);

// core0 lifecycle, mirroring ns2_init()/ns2_task()'s shape exactly (same
// personality-lifecycle contract, different protocol). switch_gc_init() runs
// once when this personality becomes active (on entry from the mode cycle,
// not just at boot); switch_gc_task() runs every main-loop iteration while
// this personality is active.
void switch_gc_init(void);
void switch_gc_task(void);

// Called once, right before switching AWAY from this personality (or the
// reverse -- see usb.c's transition sequence), to quiesce all GameCube-side
// runtime state so nothing leaks into a fresh session next time this
// personality becomes active again. Cheap/idempotent: just re-runs
// switch_gc_init()'s state reset today, kept as its own entry point in case
// Stage C+ need asymmetric enter/leave behavior later.
void switch_gc_reset(void);

// Thin wrapper around switch_gc_encode_report() (switch_gc_encode.h): reads the live shared
// input state and this personality's own report counter (module-static, reset by
// switch_gc_init()/switch_gc_reset()/switch_gc_mount() -- see the .c file). `p` must point
// to a buffer of at least 63 bytes. Streamed when a host selects report 0x0A.
void switch_gc_build_report(uint8_t *p);

// Thin wrapper around switch_gc_encode_report05() (switch_gc_encode.h), same shape as
// switch_gc_build_report() but for report 0x05 -- the format PC/Steam hosts actually select
// (Confirmed 2026-07-13 via live capture; see switch_gc_encode_report05()'s own comment).
void switch_gc_build_report05(uint8_t *p);

// HID OUT report entry point (report 0x03, rumble). `report_id`/`data`/`len` are the
// TinyUSB transport already normalized by usb_descriptors.c's tud_hid_set_report_cb()
// dispatcher (Phase 4, 2026-07-13): `report_id` is always the real report ID regardless of
// whether the transfer arrived via interrupt OUT or control SET_REPORT, and `data`/`len`
// never include it. Report framing is 4 bytes plus 59 reserved bytes. The state decoder accepts
// both captured state-byte positions, ignores idle ZLPs, and bounds every ON pulse. See
// docs/switch2-gc/open-questions.md for unresolved semantics.
void switch_gc_hid_out_report(uint8_t report_id, const uint8_t *data, uint16_t len);

// EP0 vendor control request entry point, called from the centralized
// tud_vendor_control_xfer_cb dispatcher in usb_descriptors.c. Handles the Windows WinUSB
// auto-bind request, plus bRequest 0x02/0x03/0x04 -- the real Switch 2 console's
// post-SET_CONFIGURATION identity handshake, which it requires answered (not stalled) before
// it will touch the bulk command channel at all. This is the SAME gate `switch_pro2.c`'s
// `ns2_vendor_control_xfer()` already solved for Pro Controller 2; found 2026-07-13 to be
// exactly why GC mode enumerated and streamed fine on Windows/Steam but was not recognized on a
// real console. See the .c file's own comment for the byte-exact identity/info block layout
// (Confirmed from this project's own captured GC bRequest=3/2 responses, serial fictionalized
// per NSO-GC.md's exclusion rule). Anything else stalls.
bool switch_gc_vendor_control_xfer(uint8_t rhport, uint8_t stage, const void *request);

// TinyUSB SET_CONFIGURATION-complete hook, called from the centralized
// tud_mount_cb dispatcher. Stage B stub: resets the (currently trivial)
// runtime state that switch_gc_init() also resets, mirroring ns2_mount()'s
// role of invalidating any host-session-scoped state on a fresh connection.
void switch_gc_mount(void);

#endif  // SWITCH_GC_H
