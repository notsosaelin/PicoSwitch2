// sw2_capture.h — non-invasive, timestamped raw-BLE-traffic capture for a genuine Switch 2
// controller (Pro Controller 2 / Joy-Con 2 / GameCube Controller 2), added 2026-07-10.
//
// Purpose: switch2_ble.c's process_report() only reads report bytes 0-15 (buttons/sticks) and
// 60-61 (GC triggers) out of a 63-64 byte packet — bytes 16-59 (which the sibling native-BLE
// motion format, docs/experiments/switch2_native_motion_map_DyCOOL.md, places motion inside)
// were originally received, decrypted by the BT stack, and then silently discarded. The
// production Pro2 path now preserves native 0x000E motion separately; this module still captures
// the COMPLETE raw bytes of every notification/command/state-transition this repo's BLE host
// code already sees, unmodified, with a timestamp — so a captured session can be inspected
// offline instead of guessing at what an undecoded byte range might contain.
//
// Design constraints (deliberate):
//   - Capture NEVER blocks or affects BT stack behavior: the producer (core1, BT stack
//     callbacks) overwrites the oldest entry and counts it if the ring is full, rather than waiting.
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
    SW2_CAP_VARIANT      = 9,  // v2 experiment variant starting. data[0] = variant id (1-9, see
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
    SW2_CAP_LINK_PARAMS  = 12, // BLE connection parameters. handle=HCI connection handle; data =
                                // [phase][status][interval:u16-LE][latency:u16-LE]
                                // [supervision_timeout:u16-LE]. phase: 1=snapshot before an
                                // experimental update, 2=update request submitted,
                                // 3=HCI LE Connection Update Complete. Interval is in 1.25ms
                                // units and supervision timeout in 10ms units.
    SW2_CAP_MOTION_PAIR  = 13, // Internal time-aligned Pro2-PDU + DS5-IMU diagnostic record.
    SW2_CAP_NFC_NOTIFY   = 14, // Extended NFC response notification from value handle 0x001E.
    SW2_CAP_NFC_STATE    = 15, // Genuine report-0x000E NFC-state transition; data[0]=state.
    SW2_CAP_MOTION_HYBRID = 16, // Genuine base + emitted XOR + live donor diagnostics.
} sw2_capture_kind_t;

typedef enum {
    SW2_CAPTURE_FILTER_ALL = 0,
    // Retain both command paths used during NFC diagnosis:
    // primary 0x0014/0x001A/0x001B, extended
    // 0x0016/0x001E/0x001F, state transitions, and explicit markers.
    SW2_CAPTURE_FILTER_NFC = 1,
} sw2_capture_filter_t;

#define SW2_CAP_MAX_DATA 128 // Includes the 112-byte Pro2 headset/audio input report (0x002E).

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
void sw2_capture_set_filter(sw2_capture_filter_t filter);
sw2_capture_filter_t sw2_capture_get_filter(void);
bool sw2_capture_get_enabled(void);
uint32_t sw2_capture_dropped_count(void);  // oldest entries overwritten since last enable
uint16_t sw2_capture_buffered_count(void); // entries currently waiting to be drained

// Mutually-exclusive reuse of the same retained ring for paired motion data.
// This avoids allocating a second large diagnostic buffer in the firmware's
// tight CYW43 runtime-heap margin. Ordinary BLE capture is forced off when
// paired mode starts, and vice versa.
void sw2_capture_pair_set_enabled(bool on);
bool sw2_capture_pair_get_enabled(void);
void sw2_capture_pair_record(const uint8_t *data, uint16_t len);

// Mutually-exclusive live-hybrid use of the same retained ring. Keeping this
// separate from `motionpair` makes the exported schema unambiguous while
// retaining the existing diagnostic RAM ceiling.
void sw2_capture_hybrid_set_enabled(bool on);
bool sw2_capture_hybrid_get_enabled(void);
void sw2_capture_hybrid_record(const uint8_t *data, uint16_t len);

// Diagnostic chart-transition capture support. Retain only the newest
// pre-trigger records and make the remainder of the ring available for a
// lossless post-trigger tail. Intentional pre-trigger sliding-window
// overwrites are cleared from the session drop counter at this boundary.
uint16_t sw2_capture_pair_prepare_post_trigger(uint16_t retain_newest);

// ---- One-shot GATT discovery: ground truth for raw ATT handle numbering (2026-07-10) ----
// OFF by default. The v1 motion-enable experiment (superseded by the v2 matrix below) worked --
// 0x000E is real and responds to a 0x0C configure/enable pair -- but designing a v2 experiment
// exposed that this repo had no independently-confirmed mapping for handles beyond the ones its
// own fixed #defines already use (0x000A/B, 0x0012, 0x0014, 0x001A/B) or the reference tool's
// documented value handles (0x000E, 0x0016, 0x001E). In particular, a "write report rate" step in
// that reference tool initially had ambiguous Bleak-to-ATT arithmetic. The completed discovery
// identifies descriptor 0x0010 as the actual target. This facility walks the actual GATT table
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
//   7. reset_then_imu             -- disable all features, then configure/enable 0x07
//   8. console_motion_core        -- console-captured configure=0x27, exact console calibration
//                                  reads, enable=0x27, report-rate descriptor 0x0085, then deferred
//                                  0x000E subscription; this is the first console-grounded variant
//   9. console_motion_fast_link   -- exactly variant 8, followed by a standard-compliant 7.5ms
//                                  central-side BLE connection-interval update after subscription;
//                                  isolates the observed 30ms link bottleneck without changing v8
// Selecting a variant arms it for the next connection's post-SW2_INIT_DONE one-shot firing (same
// trigger point v1 used). Exactly one variant is armed at a time; each variant fires once per
// connection (the guard resets on disconnect), so testing a different variant -- or re-testing
// the same one -- requires a fresh reconnect/power-cycle between attempts, by construction. Every
// command, CCC write, and ACK this produces is captured via sw2_capture_record() exactly like
// v1 was, plus one SW2_CAP_VARIANT marker at the start identifying which variant is running.
// The UART selector remains an opt-in RE control surface. Production native Pro2 motion uses a
// separately named profile with the same console-confirmed sequence; its 0x000E notification is
// normalized for buttons/sticks and transported opaquely into report 0x09.
// See docs/switch2/ble-controller-protocol-inventory.md §3.7 for the full design, the
// handle-numbering analysis, and what each variant is meant to isolate.
void sw2_set_v2_variant(uint8_t variant);  // 0 = off/disarmed, 1-9 = variant id per the list above
uint8_t sw2_get_v2_variant(void);

typedef struct {
    uint32_t checks;
    uint32_t starts;
    uint32_t wait_elapsed_ms;
    uint16_t source_pid;
    uint8_t personality;
    uint8_t init_state;
    uint8_t v2_state;
    uint8_t auto_fired;
    uint8_t armed_variant;
    uint8_t gatt_discovery;
    uint8_t block_mask; // bit0 wait, bit1 armed, bit2 discovery, bit3 personality,
                        // bit4 no source, bit5 PID mismatch, bit6 v2 busy
} sw2_native_auto_diag_t;

// Read-only automatic native-motion gate state, exposed over UART as `motionauto` so a failed
// cold boot identifies the exact gate instead of requiring another speculative firmware change.
void sw2_native_auto_diag_snapshot(sw2_native_auto_diag_t *out);

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
