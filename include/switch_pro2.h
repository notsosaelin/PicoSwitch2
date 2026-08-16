/*
 * Switch 2 Pro Controller (VID 057E / PID 2069) USB emulation.
 * Compiled/active only when the firmware is built with -DNS2_PRO.
 *
 * Protocol + byte-exact details: docs/switch2/usb-spec.md (verified against
 * ndeadly's USB capture). USB layout (Option B, no audio):
 *   IF0 HID    - input report 0x09 on EP 0x81 IN, rumble report 0x02 on EP 0x01 OUT
 *   IF1 Vendor - 8-byte command protocol on EP 0x02 bulk OUT / 0x82 bulk IN
 */
#ifndef SWITCH_PRO2_H
#define SWITCH_PRO2_H

#include <stddef.h>
#include <stdint.h>

// Descriptor accessors used by usb_descriptors.c when NS2_PRO is defined.
const uint8_t *ns2_device_descriptor(void);
const uint8_t *ns2_config_descriptor(void);
const uint8_t *ns2_hid_report_descriptor(void);  // 97 bytes (length declared in config desc)
const char   **ns2_string_table(size_t *count);
const uint16_t *ns2_ms_os_string_descriptor(void);  // MS OS 1.0 OS string (index 0xEE)
extern volatile uint8_t g_ns2_stage;  // NS2_DIAG: enumeration + handshake progress (0-7)

// core0 lifecycle: init once, then service every loop iteration in normal mode.
void ns2_init(void);
void ns2_task(void);

// Diagnostic (UART `pipe`): number of Pro2 input reports built for the console,
// and ms since the last one (UINT32_MAX if none). With report_input_age_ms(),
// distinguishes a stalled core0 report loop from BT input that stopped feeding.
uint32_t ns2_pro2_report_count(void);
uint32_t ns2_pro2_last_report_age_ms(void);

// Config-mode-only lifecycle: ns2_task() (and therefore the report-0x09 motion tracker inside
// ns2_build_report()) never runs while the dongle is in config mode, so this runs an equivalent,
// rate-limited tick directly off the live cross-core motion state — otherwise the config-mode
// `imu` debug command's bias/still fields are dead, always-zero static memory (found 2026-07-10
// from hardware output showing bias=[0,0,0] on a moving, motion-feeding DualSense).
void ns2_motion_debug_tick(void);

// Rumble output report 0x02 delivered on the HID OUT endpoint. `report_id`/`data`/`len` are
// pre-normalized by usb_descriptors.c's tud_hid_set_report_cb() dispatcher (Phase 4,
// 2026-07-13) -- `report_id` is always the real report ID regardless of transport
// (interrupt OUT vs control SET_REPORT), and `data`/`len` never include it.
void ns2_hid_out_report(uint8_t report_id, const uint8_t *data, uint16_t len);

// This personality's own TinyUSB EP0-vendor-control and mount hooks, called
// from usb_descriptors.c's centralized tud_vendor_control_xfer_cb/tud_mount_cb
// dispatchers (renamed 2026-07-13 so a second personality, GameCube, can also
// have a hook without two competing global TinyUSB callback definitions --
// see docs/switch2-gc/usb-personality.md "TinyUSB dispatch..."). Declared with
// an opaque `const void *` (matching switch_gc.h's analogous declaration) so
// this header does not need to include tusb.h; usb_descriptors.c (which does
// include it) passes the real tusb_control_request_t* straight through.
#include <stdbool.h>
bool ns2_vendor_control_xfer(uint8_t rhport, uint8_t stage, const void *request);
void ns2_mount(void);

// NTAG I2C 2K ("figure v3", e.g. Kirby Air Riders) advertised McuTagType.
//
// The console chooses which tag pages to request (0x06 descriptor: D0 | uid_len |
// uid | McuTagType | block_count | page ranges) from the tag type the controller
// reports in the 0x05 status. `McuTagType 1` = NTAG 215, which yields page ranges
// 0x00-0x86 = exactly 540 bytes. The value for a 2 KB NTAG I2C tag is not
// documented anywhere, so it is sweepable at runtime over UART (`v3mode N`) to
// allow a hardware experiment matrix without reflashing. Production default 1
// uses the compatibility view for ordinary reads and raw v3 for extended ones.
// See docs/switch2/kirby-air-riders-extended-amiibo.md.
void ns2_v3_set_serve_mode(uint8_t mode);
uint8_t ns2_v3_get_serve_mode(void);

// NFC status-payload probe (RE tooling, UART `v3probe`).
//
// The 0x05 status reply carries 61 payload bytes; only ~16 are understood
// (state, UID length, UID and the `01 02 00 07` protocol/type quad). A genuine
// controller reports the detected tag's identity somewhere in the remainder,
// which is how the console decides the page ranges it requests in the 0x06 read
// descriptor -- it always asks us for the NTAG215 set (0x00-0x86 = 540 bytes)
// because nothing we send says otherwise.
//
// This overlays arbitrary bytes onto that payload at runtime so the field can be
// swept on hardware without reflashing: set candidate bytes, scan, and watch
// whether the console's next 0x06 descriptor changes its McuTagType or ranges.
// Returns false if the range does not fit the payload.
bool ns2_v3_status_probe_set(uint8_t index, const uint8_t *bytes, uint8_t len);
void ns2_v3_status_probe_clear(void);
uint8_t ns2_v3_status_probe_count(void);

// Reply override (RE tooling, UART `v3reply`). The console may take the tag's
// identity from a reply we currently answer with a bare ACK and no payload --
// notably 0x03 (start polling) and 0x0C (undocumented in every published
// decoder). This answers a chosen subcommand with candidate bytes instead.
bool ns2_v3_set_reply(uint8_t sub, const uint8_t *data, uint8_t len);
void ns2_v3_clear_reply(void);
uint8_t ns2_v3_get_reply_sub(void);

// Read-buffer prefix probe (RE tooling, UART `v3hdr`).
//
// Byte 18 of the 60-byte read-buffer prefix is the NTAG model. The console reads
// it from the buffer WE generate and uses it to choose the page ranges it then
// requests: 0 = NTAG215 (135 pages -> 00-3b,3c-77,78-86 = 540 B), 3 = NTAG213
// (45), 4 = NTAG216 (231). Verified against CTCaer/jc_toolkit and the yuzu
// Joy-Con driver, which independently agree the model is at buf2[74] with the MCU
// payload starting at buf2[56], i.e. header offset 18. We emit 0x00, which is why
// the console has only ever asked us for the 540-byte NTAG215 page set.
//
// NTAG I2C Plus 2K must be a different (newer) enum value; this sweeps it live.
bool ns2_v3_hdr_probe_set(uint8_t index, const uint8_t *bytes, uint8_t len);
void ns2_v3_hdr_probe_clear(void);
uint8_t ns2_v3_hdr_probe_count(void);

// The v3 tag's 32-byte originality signature, served in read-buffer prefix
// [19..50]. A 2048-byte dump does not carry it; capture it from a physical tag
// with tools/nfc_probe.ps1. RAM-only, so it can be set between console tests
// without a reflash.
// How many times the 0x14 staging gate passed, and how many 0x21 device results
// were built. A 0x21 reply is a bare ACK either way, so the wire cannot show it.
void ns2_v3_device_cmd_counts(uint32_t *staged, uint32_t *results);
// Capture-derived v3 console-write counters. Chunks count accepted 0x14 data
// fragments; commits count generation-checked 0x08 updates; errors count
// rejected write transactions.
void ns2_v3_write_counts(uint32_t *chunks, uint32_t *commits,
                         uint32_t *errors);
// Sector-aware 355-byte clear and 167-byte update operations use 0x14 staging
// and 0x20 completion. These counters keep them distinct from the ordinary
// 454-byte/0x08 encrypted-body write that follows each one.
void ns2_v3_extended_counts(uint32_t *chunks, uint32_t *completions);

// Why the last v3 operation failed. Every failure reaches the console as the
// same status 0x07 / detail 0x41, which it renders as 2115-0096; during the v3
// investigation that one value was produced first by a tag-removal timing bug
// and later by a fail-closed record-layout rejection, and the second was
// misdiagnosed as the first. `result` is the ns2_virtual_nfc_result_t from a
// staging or commit rejection, `offset` the 0x14 stage offset when relevant.
// Any pointer may be NULL. See ns2_amiibo_v3_error_t.
void ns2_v3_last_error(uint8_t *error, const char **name, uint8_t *sub,
                       uint8_t *result, uint16_t *offset, uint32_t *count);

bool ns2_v3_set_signature(const uint8_t *bytes, size_t len);
void ns2_v3_clear_signature(void);
bool ns2_v3_has_signature(void);

//--------------------------------------------------------------------+
// Report-0x09 motion debug/instrumentation (config.c's "imu" CDC command).
//--------------------------------------------------------------------+

// Bisects the gyro pipeline: report-0x09 USB state (active report id, streaming, motion
// length last emitted).
void ns2_dbg_report_state(uint8_t *report_id, uint8_t *streaming, uint8_t *motion_len);

typedef struct {
    uint8_t enabled;
    uint8_t source_active;
    uint8_t initialized;
    uint8_t has_sample;
    uint8_t probe_active;
    int16_t probe_gyro[3];
    int16_t input_gyro[3];
    int32_t bias_gyro[3];
    int32_t corrected_gyro[3];
    int32_t jitter[3];
    int8_t gyro_map[3];
    uint8_t carrier;
    uint8_t body_frame;
    int32_t quaternion_million[4];
    uint32_t updates;
    uint32_t representation_rejects;
    uint32_t host_dt_us;
    uint32_t sensor_dt_us;
    uint32_t sensor_dt_max_us;
    uint32_t timestamp_fallbacks;
    uint32_t timestamp_invalid;
    uint32_t sequence_gaps;
    uint32_t integration_substeps;
} ns2_ds5_motion_diag_t;

// Native-quaternion translator state for UART diagnosis. Quaternion values are
// fixed-point x1,000,000 so diagnostics do not depend on float printf support.
void ns2_dbg_ds5_motion(ns2_ds5_motion_diag_t *out);
bool ns2_dbg_ds5_motion_enabled(void);

// True while the console has actually negotiated the IMU feature, i.e. motion
// bytes are really being transmitted. Read by the seam so a motion-capable
// source (currently the Android companion bridge) can idle its sensors when no
// game is consuming motion, instead of streaming gyro into a report that
// carries none. Not a debug hook -- this is the console's real IMU state.
bool ns2_motion_negotiated(void);
void ns2_dbg_ds5_motion_set_enabled(bool enabled);
bool ns2_dbg_ds5_motion_probe_rate(uint8_t axis, int16_t rate);
void ns2_dbg_ds5_motion_probe_off(void);
void ns2_dbg_ds5_motion_set_body_frame(bool body_frame);
bool ns2_dbg_ds5_motion_set_map(const int8_t map[3]);
void ns2_dbg_ds5_motion_set_carrier(uint8_t carrier);

// Length-0x28 catch-up translation gate. DEFAULT OFF.
//
// Enabling REPLACES the motion block rather than supplementing it: genuine
// controllers run 0x28-only or interleaved, and only the 0x28-only mode has a
// resolved elapsed relation, so the 0x1E carrier is not sent alongside.
// Toggling either way clears buffered samples and the cadence epoch.
//
// Intended for a UART-gated hardware A/B against the proven 0x1E path, not for
// unattended use. See docs/experiments/pro2-carrier-unknown-fields-2026-07-31.md.
bool ns2_ds5_motion40_get_enabled(void);
void ns2_ds5_motion40_set_enabled(bool enabled);

// What occupies the motion block on the ~19 USB polls between catch-up
// packets. A 1 kHz poll rate against a ~50 Hz packet cadence means this choice
// decides how many times the console sees each 0x28.
//
//   EMPTY   no motion block between packets, so each 0x28 is delivered exactly
//           once -- the only fill matching genuine hardware, where a 0x28 is
//           never sent twice. Default.
//   REPEAT  resend the latest 0x28 every poll. Tested on hardware 2026-07-31:
//           the console accepted the packets and produced violent, erratic
//           motion. A 0x1E tolerates this because an absolute quaternion is
//           idempotent; a 0x28 carries integrable samples and a modular
//           orientation slice, so repeats can integrate as many times the real
//           rotation.
//   CARRIER coherent mixed scheduler: select one new 0x1E/0x28 PDU on the
//           shared native-rate tick timeline and hold it across intervening
//           USB polls, matching genuine bridge ownership.
//
// Changing the fill clears buffered samples and the cadence epoch.
#define NS2_PDU40_FILL_EMPTY   0u
#define NS2_PDU40_FILL_REPEAT  1u
#define NS2_PDU40_FILL_CARRIER 2u
uint8_t ns2_ds5_motion40_get_fill(void);
void ns2_ds5_motion40_set_fill(uint8_t fill);

// UART-only acceleration-scale discriminator for the generated 0x28 path.
// LIVE applies the validated 0x1E carrier's output calibration to the post-
// seam 4096-count/g source. HALF recreates the rejected double-normalization;
// ZERO is diagnostic-only. Changing it resets the buffered 0x28 timeline but
// never touches the production 0x1E translator.
uint8_t ns2_ds5_motion40_get_accel_mode(void);
bool ns2_ds5_motion40_set_accel_mode(uint8_t mode);

// Emitted packet count, starved intervals, and saturated axes -- the three
// numbers that distinguish "working" from "well-formed but wrong".
// `overlong` counts windows that outran the high-rate elapsed band and were
// dropped rather than clamped: elapsed selects the layout, so a longer span
// would be decoded with the wrong field map.
void ns2_ds5_motion40_get_counters(uint32_t *emitted, uint32_t *starved,
                                   uint32_t *overlong,
                                   uint32_t *saturated_accel,
                                   uint32_t *saturated_gyro);
void ns2_ds5_motion40_get_schedule(uint32_t *carrier_frames,
                                   uint32_t *held_polls,
                                   uint32_t *fallback_carriers,
                                   uint8_t *output_length,
                                   uint16_t *last_tick);

#endif  // SWITCH_PRO2_H
