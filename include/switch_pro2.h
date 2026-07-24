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

//--------------------------------------------------------------------+
// Report-0x09 motion debug/instrumentation (config.c's "imu" CDC command).
//--------------------------------------------------------------------+

// Bisects the gyro pipeline: report-0x09 USB state (active report id, streaming, motion
// length last emitted).
void ns2_dbg_report_state(uint8_t *report_id, uint8_t *streaming, uint8_t *motion_len);

// Bias-tracker state (raw LSB units) + whether the stillness gate is open right now.
void ns2_dbg_motion_bias(int32_t bias_out[3], uint8_t *still_out);

// The live report-0x09 phase[] accumulator (raw int32, binary-angle units, 2^32 == 360 deg).
void ns2_dbg_motion_phase(int32_t phase_out[3]);

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
} ns2_ds5_motion_diag_t;

// Native-quaternion translator state for UART diagnosis. Quaternion values are
// fixed-point x1,000,000 so diagnostics do not depend on float printf support.
void ns2_dbg_ds5_motion(ns2_ds5_motion_diag_t *out);
bool ns2_dbg_ds5_motion_enabled(void);
void ns2_dbg_ds5_motion_set_enabled(bool enabled);
bool ns2_dbg_ds5_motion_probe_rate(uint8_t axis, int16_t rate);
void ns2_dbg_ds5_motion_probe_off(void);
void ns2_dbg_ds5_motion_set_body_frame(bool body_frame);
bool ns2_dbg_ds5_motion_set_map(const int8_t map[3]);
void ns2_dbg_ds5_motion_set_carrier(uint8_t carrier);

// Anomaly-instrumentation types (2026-07-10). See ns2_motion_tick()'s NS2_MAX_PHASE_DELTA
// derivation in switch_pro2.c for the (mathematically bounded, not heuristic) detection
// criterion: a phase increment that exceeds what the code's own clamps allow is proof of a
// computation defect, not merely "fast motion." Pure observability — nothing here alters
// ns2_phase[], the bias tracker, or the transmitted report.
typedef struct {
    int16_t  gyro[3];   // raw sensor input this tick
    int32_t  delta[3];  // phase increment this tick would have added, per axis
    uint8_t  still;      // stillness-gate state this tick
    uint32_t dt_us;      // elapsed time used for integration this tick
} ns2_anom_trail_t;

#define NS2_ANOM_TRAIL 4  // ticks of preceding context kept alongside a capture

typedef struct {
    uint8_t  valid;     // 0 until the first anomaly is captured
    uint32_t seq;       // total anomalies seen since boot
    ns2_anom_trail_t trail[NS2_ANOM_TRAIL];  // preceding ticks, oldest first
    int16_t  gyro[3];
    int16_t  accel[3];
    int32_t  g[3];             // bias-corrected, low-passed gyro (raw LSB units) used this tick
    int32_t  bias[3];          // bias estimate at capture time (raw LSB units)
    uint8_t  still;
    uint32_t dt_us;
    int32_t  phase_before[3];
    int32_t  phase_after[3];
    int32_t  delta[3];         // phase_after - phase_before, i.e. the anomalous increment(s)
    uint16_t imu_tick;
    uint8_t  tick_count;       // high nibble of the timing word (ticks elapsed this report)
    uint8_t  imu_enabled;      // was the IMU feature actually negotiated at capture time
    uint8_t  motion_len;       // 30 if imu_enabled (this tick's bytes were really transmitted), else 0
    uint8_t  motion_bytes[30]; // what this tick's phase/accel WOULD encode to, regardless of motion_len
} ns2_anom_capture_t;

// Returns a pointer to the single, static "most recent anomaly" capture. valid==0 means no
// anomaly has been captured since boot yet (the rest of the struct's fields are meaningless
// in that case, not "no motion").
const ns2_anom_capture_t *ns2_dbg_motion_anomaly(void);

#endif  // SWITCH_PRO2_H
