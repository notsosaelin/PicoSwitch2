#ifndef NS2_MOTION_PROBE_H
#define NS2_MOTION_PROBE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool latched;
    bool enabled;
    uint32_t orientation[3];
    uint32_t baseline[3];
    int32_t rate[3];
    uint32_t updates;
} ns2_motion_probe_status_t;

// Latch the latest fresh genuine 0x1E PDU. The probe is deliberately not
// enabled by latching; UART must arm it explicitly after inspecting status.
bool ns2_motion_probe_latch(void);
// Create a self-contained stationary 0x1E PDU without requiring a genuine
// Pro Controller 2 to be connected. The requested state occupies G2[25:24].
bool ns2_motion_probe_seed(uint8_t swap_state);
bool ns2_motion_probe_set_enabled(bool enabled);
void ns2_motion_probe_reset(void);
// Atomically prepare all three packed carriers while the probe is disabled.
// This keeps the console from observing the invalid intermediate states that
// would result from changing the carriers one at a time.
bool ns2_motion_probe_set_orientation(const uint32_t values[3]);
// Set raw signed accelerometer carrier values while the probe is disabled.
bool ns2_motion_probe_set_accel(const int32_t values[3]);
bool ns2_motion_probe_set_rate(uint8_t axis, int32_t units_per_7500us);
void ns2_motion_probe_get_status(ns2_motion_probe_status_t *out);

// Build a diagnostic 0x1E PDU when explicitly enabled. Returns false during
// all ordinary operation, leaving the proven native/fallback paths untouched.
bool ns2_motion_probe_build(uint8_t out[30]);

#endif  // NS2_MOTION_PROBE_H
