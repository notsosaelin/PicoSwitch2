// sw2_capture.h — non-invasive, timestamped raw-BLE-traffic capture for a genuine Switch 2
// controller (Pro Controller 2 / Joy-Con 2 / GameCube Controller 2), added 2026-07-10.
//
// Purpose: switch2_ble.c's process_report() only reads report bytes 0-15 (buttons/sticks) and
// 60-61 (GC triggers) out of a 63-64 byte packet — bytes 16-59 (which the sibling native-BLE
// motion format, docs/experiments/switch2_native_motion_map_DyCOOL.md, places motion inside)
// are received, decrypted by the BT stack, and then silently discarded. This module captures
// the COMPLETE raw bytes of every notification/command/state-transition this repo's BLE host
// code already sees, unmodified, with a timestamp — so a captured session can be inspected
// offline instead of guessing at what an undecoded byte range might contain.
//
// Design constraints (deliberate):
//   - Capture NEVER blocks or affects BT stack behavior: the producer (core1, BT stack
//     callbacks) drops an entry and counts it if the ring buffer is full, rather than waiting.
//   - Capture does not decode, interpret, or alter anything — it stores exact bytes as received/
//     sent, plus a kind tag and the GATT handle involved. Any semantic decoding happens later,
//     offline, from the exported log — never inline in the capture path.
//   - Off by default. Enabling it (via the config-mode `sw2cap on` command) is the only behavior
//     change from normal operation; disabled, this module does nothing.
//   - Pull-based drain (2026-07-10 revision). The first version auto-streamed NDJSON lines to
//     CDC unprompted from config_cdc_task(); that's incompatible with the web UI's
//     one-line-per-command request/response protocol (sendCmd() matches replies to requests by
//     strict arrival order — an unsolicited capture line arriving between a command and its
//     reply is indistinguishable from that reply, corrupting the queue). Draining is now
//     explicitly pulled one entry at a time by the caller (config.c, in response to a
//     `sw2cap drain` command), which builds a normal bounded JSON reply — fitting the existing
//     protocol instead of fighting it.
#ifndef SW2_CAPTURE_H
#define SW2_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SW2_CAP_INPUT_NOTIFY = 1,  // raw bytes from a Switch 2 input-report GATT notification
    SW2_CAP_ACK_NOTIFY   = 2,  // raw bytes from the Switch 2 command-ACK GATT notification
    SW2_CAP_CMD_OUT      = 3,  // raw bytes this host wrote to the Switch 2 command characteristic
    SW2_CAP_CCC_WRITE    = 4,  // a CCC (notify-enable) write this host performed
    SW2_CAP_STATE        = 5,  // sw2_init_state_t transition; data[0] = new state
    // Added 2026-07-10 for the GATT-discovery tool (ground truth for raw ATT handle numbering,
    // see sw2_set_gatt_discovery_enabled() below) and the v2 feature-enable experiment matrix
    // (see sw2_set_v2_variant() below).
    SW2_CAP_GATT_SVC     = 6,  // one discovered primary service. handle=start_group_handle;
                                // data = [end_group_handle:u16-LE][uuid16:u16-LE][uuid128:16B]
    SW2_CAP_GATT_CHAR    = 7,  // one discovered characteristic. handle=value_handle (the raw ATT
                                // handle this repo's own GATT calls would use); data =
                                // [decl_handle:u16-LE][end_handle:u16-LE][properties:u16-LE]
                                // [uuid16:u16-LE][uuid128:16B] -- decl_handle is the declaration
                                // handle (always == value_handle-1 per the GATT spec; included so
                                // the relationship is visible directly in the export, not assumed)
    SW2_CAP_GATT_DESC    = 8,  // one discovered descriptor of the characteristic most recently
                                // seen via SW2_CAP_GATT_CHAR. handle=descriptor handle (raw ATT);
                                // data = [uuid16:u16-LE][uuid128:16B]
    SW2_CAP_VARIANT      = 9,  // v2 experiment variant starting. data[0] = variant id (1-6, see
                                // sw2_set_v2_variant()); logged once, before that variant's first
                                // protocol action, so it's unambiguous which variant produced the
                                // entries that follow it in an exported NDJSON file
    // Added 2026-07-10 for controlled-motion capture sessions (see sw2_capture_mark() below) and
    // to close a gap found analyzing the v2 hardware run: the CCC-write/handle-write completion
    // callbacks in btstack_host.c only printf() their ATT status, never captured it.
    SW2_CAP_WRITE_STATUS = 10, // ATT status of a GATT write's completion (CCC write or the
                                // report-rate descriptor write). handle=the handle that was
                                // written; data[0]=ATT status byte (0x00=success, BTstack
                                // ATT_ERROR_* otherwise). Logged from the SAME completion
                                // callback that was already firing -- adds a capture record, does
                                // not change when the callback fires or what it does afterward.
    SW2_CAP_MARKER       = 11, // a user-supplied text label, inserted on demand via the config
                                // command `sw2cap mark <text>` / the web panel's marker button.
                                // handle=0; data=raw ASCII bytes of the label (not
                                // NUL-terminated in-frame; length is `len`). Purely additive
                                // logging -- see sw2_capture_mark()'s own comment for the exact
                                // non-invasiveness guarantee.
} sw2_capture_kind_t;

#define SW2_CAP_MAX_DATA 64  // largest single Switch 2 BLE payload observed anywhere so far

typedef struct {
    uint64_t us;      // capture timestamp, time_us_64() (monotonic since boot, not wall-clock)
    uint16_t handle;  // GATT handle involved
    uint8_t  kind;    // sw2_capture_kind_t
    uint8_t  len;     // bytes actually stored in data[] (may be < orig_len if ever truncated)
    uint16_t orig_len;
    uint8_t  data[SW2_CAP_MAX_DATA];
} sw2_cap_entry_t;

// Record one event. Safe to call from core1's BT-stack context; never blocks. No-op if capture
// is currently disabled.
void sw2_capture_record(sw2_capture_kind_t kind, uint16_t handle, const uint8_t *data, uint16_t len);

// Pop the oldest buffered entry into *out. Returns true if one was available (and popped), false
// if the ring is currently empty (nothing more to drain right now). Call in a bounded loop from
// a config command handler to build a batch reply — see config.c's cmd_sw2cap_drain().
bool sw2_capture_drain_one(sw2_cap_entry_t *out);

// Human-readable name for a sw2_capture_kind_t value (e.g. "input", "ack") — shared so config.c
// doesn't need its own copy of the kind->name mapping.
const char *sw2_capture_kind_name(uint8_t kind);

void sw2_capture_set_enabled(bool on);
bool sw2_capture_get_enabled(void);
uint32_t sw2_capture_dropped_count(void);  // entries lost to a full ring buffer since last enable

// ---- One-shot GATT discovery: ground truth for raw ATT handle numbering (2026-07-10) ----
// OFF by default. The v1 motion-enable experiment (superseded by the v2 matrix below) worked --
// 0x000E is real and responds to a 0x0C configure/enable pair -- but designing a v2 experiment
// exposed that this repo had no independently-confirmed mapping for handles beyond the ones its
// own fixed #defines already use (0x000A/B, 0x0012, 0x0014, 0x001A/B) or the reference tool's
// documented value handles (0x000E, 0x0016, 0x001E). In particular, a "write report rate" step in
// that reference tool targets a handle derived by bleak-offset arithmetic that turned out
// genuinely ambiguous on paper (see ble-controller-protocol-inventory.md §3.7.1 for the two
// competing derivations, 0x000C vs 0x000D). Rather than guess, this walks the actual GATT table
// BTstack itself discovers on the live connection -- every primary service, every characteristic
// (declaration handle + value handle + properties + UUID), and every descriptor (handle + UUID)
// -- and captures each one via the existing sw2_capture_record() pipeline (SW2_CAP_GATT_SVC/
// CHAR/DESC), so it exports through the same NDJSON path as everything else. This is the
// authoritative source for handle numbering going forward; arithmetic derivations from the
// reference tool's bleak-indexed numbers should be treated as superseded once a discovery capture
// exists. Fires once per connection, after SW2_INIT_DONE, same trigger point as the v2 experiment
// below (do not arm both in the same session -- BTstack allows only one outstanding GATT query
// per connection, so overlapping the two is unsupported, not merely untidy).
void sw2_set_gatt_discovery_enabled(bool on);
bool sw2_get_gatt_discovery_enabled(void);

// ---- v2 feature-enable experiment matrix (2026-07-10) ----
// OFF by default (variant 0). Supersedes the single-shot v1 experiment (which is now variant 1,
// "control" -- reproduced exactly, not reimplemented). Motivated by the v1 result: 0x000E is
// reachable and its 0x0C configure(0x07)/enable(0x07) pair is accepted, but the resulting report
// is a byte-shifted duplicate of 0x000A's buttons+sticks payload -- no orientation-responsive
// data. Diffing v1's command sequence against the reference tool's actual working init flow
// (tools/switch2_input_viewer.py) found three untested differences: a 0xFF (not 0x07) configure
// mask, six intervening SPI calibration reads, and a write to a not-yet-independently-confirmed
// descriptor handle. Rather than replicate all three as one opaque combined sequence, each is
// isolated as its own one-shot variant so a positive result is attributable to a specific cause:
//   1. control               -- exactly reproduces v1 (configure=0x07, enable=0x07, no extras)
//   2. mask_ff                  -- only the configure/enable mask changes, 0x07 -> 0xFF
//   3. handle_write_only        -- only the reference tool's descriptor write is added, mask stays 0x07
//   4. mask_ff_handle_write     -- both of the above together, still no calibration reads
//   5. calibration_seq          -- the six SPI calibration reads inserted (in the reference tool's
//                                  exact order) between configure and enable, mask stays 0x07
//   6. full_sequence             -- configure=0xFF, calibration reads, enable=0x07, then the
//                                  descriptor write, AND (unique to this variant) the 0x000E CCC
//                                  subscribe deferred to the very end -- mirroring the reference
//                                  tool's actual operation order, not just its command bytes
// Selecting a variant arms it for the next connection's post-SW2_INIT_DONE one-shot firing (same
// trigger point v1 used). Exactly one variant is armed at a time; each variant fires once per
// connection (the guard resets on disconnect), so testing a different variant -- or re-testing
// the same one -- requires a fresh reconnect/power-cycle between attempts, by construction. Every
// command, CCC write, and ACK this produces is captured via sw2_capture_record() exactly like
// v1 was, plus one SW2_CAP_VARIANT marker at the start identifying which variant is running.
// Nothing here is decoded, and nothing feeds normal input routing or report 0x09.
// See docs/switch2/ble-controller-protocol-inventory.md §3.7 for the full design, the
// handle-numbering analysis, and what each variant is meant to isolate.
void sw2_set_v2_variant(uint8_t variant);  // 0 = off/disarmed, 1-6 = variant id per the list above
uint8_t sw2_get_v2_variant(void);

// ---- Capture annotation marker (2026-07-10) ----
// Inserts a single SW2_CAP_MARKER entry, timestamped like everything else, carrying an arbitrary
// short text label. Exists so a controlled-motion capture session (hold still / rotate / hold
// tilted / return to baseline, per docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md
// §8's protocol) can mark exactly when each phase began and ended in the exported NDJSON, without
// touching the BLE connection, the v2 experiment, or report parsing in any way. Callable directly
// from config.c (core0) -- sw2_capture_record() is already cross-core-safe internally, and a
// marker is pure logging, not a BLE operation, so no core1 marshaling is needed here (contrast
// with e.g. btstack_host_mouthpad_clear_bond(), which DOES need to marshal because it touches
// live GATT state). No-op if capture is currently disabled, same as every other capture call.
void sw2_capture_mark(const uint8_t *label, uint16_t len);

#endif  // SW2_CAPTURE_H
