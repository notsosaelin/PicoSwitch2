#include "ns2_motion_hybrid_live.h"

#include <string.h>

#include "ns2_motion_hybrid.h"
#include "ns2_motion_seam.h"
#include "sw2_capture.h"
#include "switch_pro.h"

static ns2_motion_hybrid_projector_t s_projector;
static ns2_motion_hybrid_live_diag_t s_diag;
static volatile uint32_t s_diag_sequence;
static volatile uint32_t s_control_generation;
static volatile uint8_t s_requested_mode;
static uint32_t s_active_generation;
static uint8_t s_active_mode;
static uint32_t s_ds5_sequence;
static volatile uint32_t s_source_last_us;
static volatile uint32_t s_source_sequence;
static volatile uint8_t s_source_calibration_state;

static void diag_begin(void)
{
    (void)__atomic_add_fetch(&s_diag_sequence, 1u, __ATOMIC_ACQ_REL);
}

static void diag_end(void)
{
    (void)__atomic_add_fetch(&s_diag_sequence, 1u, __ATOMIC_RELEASE);
}

static uint32_t groups_for_mode(uint8_t mode)
{
    switch (mode) {
        case NS2_MOTION_HYBRID_MODE_ACCEL:
            return NS2_MOTION_HYBRID_ACCEL;
        case NS2_MOTION_HYBRID_MODE_GYRO:
            return NS2_MOTION_HYBRID_GYRO;
        case NS2_MOTION_HYBRID_MODE_PREFIX:
            return NS2_MOTION_HYBRID_PREFIX;
        case NS2_MOTION_HYBRID_MODE_IMU:
            return NS2_MOTION_HYBRID_ACCEL | NS2_MOTION_HYBRID_GYRO;
        case NS2_MOTION_HYBRID_MODE_ALL:
            return NS2_MOTION_HYBRID_PREFIX | NS2_MOTION_HYBRID_ACCEL |
                   NS2_MOTION_HYBRID_GYRO;
        default:
            return 0u;
    }
}

static void sync_control(void)
{
    const uint32_t generation =
        __atomic_load_n(&s_control_generation, __ATOMIC_ACQUIRE);
    if (generation == s_active_generation) return;
    s_active_generation = generation;
    s_active_mode = __atomic_load_n(&s_requested_mode, __ATOMIC_ACQUIRE);
    ns2_motion_hybrid_projector_reset(&s_projector);
    s_ds5_sequence = 0u;
    diag_begin();
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.requested_mode = s_active_mode;
    s_diag.active_mode = s_active_mode;
    diag_end();
}

void ns2_motion_hybrid_live_update_ds5(
    uint32_t captured_us, uint32_t sensor_timestamp,
    const int16_t calibrated_gyro[3], const int16_t calibrated_accel[3],
    uint8_t calibration_state)
{
    if (!calibrated_gyro || !calibrated_accel) return;
    (void)__atomic_add_fetch(&s_source_sequence, 1u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_source_calibration_state, calibration_state,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_source_last_us, captured_us, __ATOMIC_RELEASE);
    sync_control();
    if (s_active_mode == NS2_MOTION_HYBRID_MODE_OFF ||
        s_active_mode == NS2_MOTION_HYBRID_MODE_GENUINE)
        return;
    int16_t accel[3];
    int16_t gyro[3];
    ns2_motion_seam_apply(SWITCH_MOTION_SOURCE_DUALSENSE,
                          calibrated_accel, calibrated_gyro, accel, gyro);
    s_ds5_sequence++;
    ns2_motion_hybrid_projector_push(
        &s_projector, captured_us, sensor_timestamp, s_ds5_sequence,
        accel, gyro, calibration_state);
}

static void capture_result(uint32_t captured_us, const uint8_t *base,
                           const uint8_t *output, uint8_t length,
                           uint8_t mode,
                           const ns2_motion_hybrid_project_result_t *result)
{
    if (!ns2_motion_hybrid_live_capture_get_enabled()) return;
    ns2_motion_hybrid_capture_record_t record;
    memset(&record, 0, sizeof(record));
    record.native_us = captured_us;
    record.ds5_age_us = result->ds5_age_us;
    record.ds5_sequence = result->ds5_sequence;
    record.requested_groups = result->requested_groups;
    record.changed_bits = result->changed_bits;
    record.length = length;
    record.mode = mode;
    record.reason = (uint8_t)result->reason;
    record.calibration_state = result->calibration_state;
    record.pose_aligned = result->pose_aligned;
    memcpy(record.base, base, length);
    for (uint8_t i = 0; i < length; ++i)
        record.output_xor[i] = base[i] ^ output[i];
    _Static_assert(sizeof(record) <= SW2_CAP_MAX_DATA,
                   "hybrid capture record exceeds retained entry");
    sw2_capture_hybrid_record((const uint8_t *)&record, sizeof(record));
}

bool ns2_motion_hybrid_live_project_native(
    uint32_t captured_us, const uint8_t *base, uint8_t length,
    uint8_t out[40], uint8_t *mode_out, uint8_t *reason_out,
    uint32_t *groups_out)
{
    if (!base || !out || (length != 0x1Eu && length != 0x28u)) return false;
    sync_control();
    memcpy(out, base, length);
    const uint8_t mode = s_active_mode;
    if (mode_out) *mode_out = mode;
    if (groups_out) *groups_out = groups_for_mode(mode);
    if (mode == NS2_MOTION_HYBRID_MODE_OFF) {
        if (reason_out) *reason_out = NS2_MOTION_HYBRID_LIVE_PASSTHROUGH_30;
        return false;
    }

    ns2_motion_hybrid_project_result_t result;
    memset(&result, 0, sizeof(result));
    if (mode == NS2_MOTION_HYBRID_MODE_GENUINE) {
        result.reason = NS2_MOTION_HYBRID_LIVE_GENUINE_CONTROL;
        result.pose_aligned = s_projector.pose_aligned;
        result.calibration_state = s_projector.calibration_state;
    } else {
        (void)ns2_motion_hybrid_projector_project(
            &s_projector, base, length, captured_us,
            groups_for_mode(mode), out, &result);
    }
    if (reason_out) *reason_out = (uint8_t)result.reason;
    if (groups_out) *groups_out = result.applied_groups;

    diag_begin();
    s_diag.requested_mode =
        __atomic_load_n(&s_requested_mode, __ATOMIC_ACQUIRE);
    s_diag.active_mode = mode;
    s_diag.pose_aligned = result.pose_aligned;
    s_diag.calibration_state = result.calibration_state;
    s_diag.last_reason = (uint8_t)result.reason;
    s_diag.last_length = length;
    s_diag.last_changed_bits = result.changed_bits;
    s_diag.last_groups = result.applied_groups;
    s_diag.last_ds5_age_us = result.ds5_age_us;
    s_diag.last_ds5_sequence = result.ds5_sequence;
    s_diag.native_packets++;
    if ((unsigned)result.reason < NS2_MOTION_HYBRID_REASON_COUNT)
        s_diag.reasons[result.reason]++;
    if (result.reason == NS2_MOTION_HYBRID_LIVE_APPLIED)
        s_diag.hybrid_packets++;
    else if (result.reason == NS2_MOTION_HYBRID_LIVE_GENUINE_CONTROL)
        s_diag.genuine_controls++;
    else if (result.reason != NS2_MOTION_HYBRID_LIVE_PASSTHROUGH_30)
        s_diag.fallback_packets++;
    s_diag.saturated_accel += result.saturated_accel;
    s_diag.saturated_gyro += result.saturated_gyro;
    diag_end();

    capture_result(captured_us, base, out, length, mode, &result);
    // Even a rejected replacement is a valid instrumented snapshot: `out` is
    // byte-identical genuine data and the reason makes the fallback explicit.
    return true;
}

bool ns2_motion_hybrid_live_set_mode(uint8_t mode)
{
    if (mode >= NS2_MOTION_HYBRID_MODE_COUNT) return false;
    __atomic_store_n(&s_requested_mode, mode, __ATOMIC_RELEASE);
    (void)__atomic_add_fetch(&s_control_generation, 1u, __ATOMIC_ACQ_REL);
    if (mode == NS2_MOTION_HYBRID_MODE_OFF)
        ns2_motion_hybrid_live_capture_set_enabled(false);
    return true;
}

uint8_t ns2_motion_hybrid_live_get_mode(void)
{
    return __atomic_load_n(&s_requested_mode, __ATOMIC_ACQUIRE);
}

const char *ns2_motion_hybrid_mode_name(uint8_t mode)
{
    static const char *const names[] = {
        "off", "genuine", "accel", "gyro", "prefix", "imu", "all",
    };
    return mode < NS2_MOTION_HYBRID_MODE_COUNT ? names[mode] : "unknown";
}

void ns2_motion_hybrid_live_source_disconnected(void)
{
    (void)__atomic_add_fetch(&s_control_generation, 1u, __ATOMIC_ACQ_REL);
}

void ns2_motion_hybrid_live_get_diag(ns2_motion_hybrid_live_diag_t *out)
{
    if (!out) return;
    for (;;) {
        const uint32_t before =
            __atomic_load_n(&s_diag_sequence, __ATOMIC_ACQUIRE);
        if (before & 1u) continue;
        *out = s_diag;
        const uint32_t after =
            __atomic_load_n(&s_diag_sequence, __ATOMIC_ACQUIRE);
        if (before == after && !(after & 1u)) break;
    }
    out->requested_mode = ns2_motion_hybrid_live_get_mode();
    out->source_last_us =
        __atomic_load_n(&s_source_last_us, __ATOMIC_ACQUIRE);
    out->source_sequence =
        __atomic_load_n(&s_source_sequence, __ATOMIC_ACQUIRE);
    out->source_calibration_state =
        __atomic_load_n(&s_source_calibration_state, __ATOMIC_ACQUIRE);
}

void ns2_motion_hybrid_live_capture_set_enabled(bool enabled)
{
    sw2_capture_hybrid_set_enabled(enabled);
}

bool ns2_motion_hybrid_live_capture_get_enabled(void)
{
    return sw2_capture_hybrid_get_enabled();
}

uint16_t ns2_motion_hybrid_live_capture_count(void)
{
    return sw2_capture_buffered_count();
}

uint32_t ns2_motion_hybrid_live_capture_dropped(void)
{
    return sw2_capture_dropped_count();
}

bool ns2_motion_hybrid_live_capture_drain(
    ns2_motion_hybrid_capture_record_t *out)
{
    if (!out) return false;
    sw2_cap_entry_t entry;
    if (!sw2_capture_drain_one(&entry) ||
        entry.kind != SW2_CAP_MOTION_HYBRID ||
        entry.len != sizeof(*out) || entry.orig_len != sizeof(*out))
        return false;
    memcpy(out, entry.data, sizeof(*out));
    return true;
}
