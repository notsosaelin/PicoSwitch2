#include "ns2_motion_probe.h"

#include <string.h>

#include "ns2_motion_pdu.h"
#include "ns2_native_motion.h"
#include "pico/time.h"

#define NS2_MOTION_PROBE_PERIOD_US 7500u
#define NS2_MOTION_PROBE_MAX_RATE  262144

typedef struct {
    bool latched;
    bool enabled;
    uint8_t pdu[NS2_MOTION_PDU30_LENGTH];
    uint8_t baseline_pdu[NS2_MOTION_PDU30_LENGTH];
    uint32_t orientation[3];
    uint32_t baseline[3];
    int32_t rate[3];
    uint32_t latch_us;
    uint32_t last_update_us;
    uint16_t base_tick;
    uint16_t previous_tick;
    uint32_t updates;
} ns2_motion_probe_state_t;

static ns2_motion_probe_state_t s_probe;

static void write_le32(uint8_t *out, int32_t value)
{
    const uint32_t u = (uint32_t)value;
    out[0] = (uint8_t)u;
    out[1] = (uint8_t)(u >> 8);
    out[2] = (uint8_t)(u >> 16);
    out[3] = (uint8_t)(u >> 24);
}

static void finish_latch(uint32_t now)
{
    memcpy(s_probe.pdu, s_probe.baseline_pdu, sizeof(s_probe.pdu));
    memcpy(s_probe.orientation, s_probe.baseline, sizeof(s_probe.orientation));
    memset(s_probe.rate, 0, sizeof(s_probe.rate));
    s_probe.base_tick = ((uint16_t)s_probe.pdu[0] |
                         ((uint16_t)s_probe.pdu[1] << 8)) & 0x0FFFu;
    s_probe.previous_tick = s_probe.base_tick;
    s_probe.latch_us = now;
    s_probe.last_update_us = now;
    s_probe.updates = 0;
    s_probe.latched = true;
    s_probe.enabled = false;
}

bool ns2_motion_probe_latch(void)
{
    ns2_native_motion_snapshot_t native;
    const uint32_t now = time_us_32();
    if (!ns2_native_motion_snapshot_30(&native, now, 100000u) ||
        !ns2_motion_pdu30_get_orientation(native.data, s_probe.baseline)) {
        return false;
    }

    memcpy(s_probe.baseline_pdu, native.data, sizeof(s_probe.baseline_pdu));
    finish_latch(now);
    return true;
}

bool ns2_motion_probe_seed(uint8_t swap_state)
{
    if (swap_state > 3u) return false;

    memset(s_probe.baseline_pdu, 0, sizeof(s_probe.baseline_pdu));
    s_probe.baseline_pdu[2] = 0x00;
    s_probe.baseline_pdu[3] = 0x0C;
    const uint32_t midpoint[3] = {
        0x02000000u,
        0x02000000u,
        ((uint32_t)swap_state << 24) | 0x00800000u,
    };
    if (!ns2_motion_pdu30_set_orientation(s_probe.baseline_pdu, midpoint))
        return false;
    // Stationary, face-up controller: +1 g on the face-normal carrier.
    write_le32(&s_probe.baseline_pdu[24], 282472448);
    s_probe.baseline_pdu[29] = 0x02;
    memcpy(s_probe.baseline, midpoint, sizeof(s_probe.baseline));
    finish_latch(time_us_32());
    return true;
}

bool ns2_motion_probe_set_enabled(bool enabled)
{
    if (enabled && !s_probe.latched) return false;
    s_probe.enabled = enabled;
    if (enabled) {
        const uint32_t now = time_us_32();
        s_probe.latch_us = now;
        s_probe.last_update_us = now;
        s_probe.previous_tick = s_probe.base_tick;
    }
    return true;
}

void ns2_motion_probe_reset(void)
{
    if (!s_probe.latched) return;
    memcpy(s_probe.pdu, s_probe.baseline_pdu, sizeof(s_probe.pdu));
    memcpy(s_probe.orientation, s_probe.baseline, sizeof(s_probe.orientation));
    memset(s_probe.rate, 0, sizeof(s_probe.rate));
    s_probe.updates = 0;
    const uint32_t now = time_us_32();
    s_probe.latch_us = now;
    s_probe.last_update_us = now;
    s_probe.previous_tick = s_probe.base_tick;
}

bool ns2_motion_probe_set_orientation(const uint32_t values[3])
{
    if (!values || !s_probe.latched || s_probe.enabled)
        return false;
    for (uint8_t axis = 0; axis < 3u; ++axis) {
        if (values[axis] > NS2_MOTION_ORIENTATION_MASK)
            return false;
    }

    memcpy(s_probe.orientation, values, sizeof(s_probe.orientation));
    if (!ns2_motion_pdu30_set_orientation(s_probe.pdu, s_probe.orientation))
        return false;
    memset(s_probe.rate, 0, sizeof(s_probe.rate));
    s_probe.updates = 0;
    const uint32_t now = time_us_32();
    s_probe.latch_us = now;
    s_probe.last_update_us = now;
    s_probe.previous_tick = s_probe.base_tick;
    return true;
}

bool ns2_motion_probe_set_accel(const int32_t values[3])
{
    if (!values || !s_probe.latched || s_probe.enabled)
        return false;
    for (uint8_t axis = 0; axis < 3u; ++axis) {
        write_le32(&s_probe.pdu[16u + axis * 4u], values[axis]);
        write_le32(&s_probe.baseline_pdu[16u + axis * 4u], values[axis]);
    }
    return true;
}

bool ns2_motion_probe_set_rate(uint8_t axis, int32_t units_per_7500us)
{
    if (axis >= 3u || units_per_7500us < -NS2_MOTION_PROBE_MAX_RATE ||
        units_per_7500us > NS2_MOTION_PROBE_MAX_RATE) {
        return false;
    }
    s_probe.rate[axis] = units_per_7500us;
    return true;
}

void ns2_motion_probe_get_status(ns2_motion_probe_status_t *out)
{
    if (!out) return;
    out->latched = s_probe.latched;
    out->enabled = s_probe.enabled;
    memcpy(out->orientation, s_probe.orientation, sizeof(out->orientation));
    memcpy(out->baseline, s_probe.baseline, sizeof(out->baseline));
    memcpy(out->rate, s_probe.rate, sizeof(out->rate));
    out->updates = s_probe.updates;
}

bool ns2_motion_probe_build(uint8_t out[30])
{
    if (!out || !s_probe.latched || !s_probe.enabled) return false;

    const uint32_t now = time_us_32();
    uint32_t steps = (now - s_probe.last_update_us) / NS2_MOTION_PROBE_PERIOD_US;
    if (steps) {
        // Avoid a debugger/UART/USB pause turning into an enormous discontinuity,
        // and discard the excess backlog rather than applying eight old steps on
        // every subsequent 1 ms USB poll until the probe eventually catches up.
        if (steps > 8u) {
            steps = 8u;
            s_probe.last_update_us = now - steps * NS2_MOTION_PROBE_PERIOD_US;
        }
        for (uint8_t axis = 0; axis < 3u; ++axis) {
            const int64_t delta = (int64_t)s_probe.rate[axis] * steps;
            s_probe.orientation[axis] =
                (uint32_t)((s_probe.orientation[axis] + delta) &
                           NS2_MOTION_ORIENTATION_MASK);
        }
        ns2_motion_pdu30_set_orientation(s_probe.pdu, s_probe.orientation);

        const uint16_t tick = (uint16_t)((s_probe.base_tick +
            (now - s_probe.latch_us) / 1250u) & 0x0FFFu);
        uint16_t count = (uint16_t)((tick - s_probe.previous_tick) & 0x0FFFu);
        if (count > 15u) count = 15u;
        const uint16_t timing = (uint16_t)((count << 12) | tick);
        s_probe.pdu[0] = (uint8_t)timing;
        s_probe.pdu[1] = (uint8_t)(timing >> 8);
        s_probe.previous_tick = tick;
        s_probe.last_update_us += steps * NS2_MOTION_PROBE_PERIOD_US;
        s_probe.updates += steps;
    }

    memcpy(out, s_probe.pdu, NS2_MOTION_PDU30_LENGTH);
    return true;
}
