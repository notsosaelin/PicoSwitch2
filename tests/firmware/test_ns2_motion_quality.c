// Motion QUALITY harness for the one Switch 2 motion encoder.
//
// The existing motion tests answer "is the wire format right". This one answers
// "is the motion GOOD": does a pure-axis input stay on its axis, does constant
// rotation stay smooth, and — the reason this file exists — how much integrated
// angle is lost when sample dt comes from packet ARRIVAL time instead of the
// source's own sensor clock.
//
// That distinction is the structural difference between the DualSense path (which
// forwards a sensor timestamp) and the Android companion path (which did not, and
// therefore fell back to the host clock, the 3800 us minimum-period gate, and the
// 16 ms anti-lurch clamp). Hardware reported Android yaw smooth but pitch choppy
// with brief excursions; anything that perturbs integrated attitude shows up in
// pitch and never in yaw, because the console cross-checks pitch against gravity
// and gravity says nothing about yaw. So the yaw/pitch asymmetry does NOT localize
// the cause on its own — this harness measures the candidate directly instead.
//
// Everything here drives the production C translator. No behavior is reimplemented.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_ds5_motion.h"
#include "ns2_motion_pdu.h"
#include "switch_pro.h"

static int failures;

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    } else {
        printf("OK:   %s\n", message);
    }
}

// ---------------------------------------------------------------- decode side

// Recover the encoder's own quaternion from the wire block, in canonical
// [x,y,z,w]. This mirrors ns2_ds5_motion.c's packing (wire order w/x/y/z, the
// omitted component restored with the positive square root) rather than reusing
// the diagnostic helper, so a packing change cannot silently pass by changing
// both sides at once.
static bool decode_quaternion(const uint8_t pdu[30], float out_xyzw[4])
{
    uint32_t orientation[3];
    if (!ns2_motion_pdu30_get_orientation(pdu, orientation)) return false;

    const unsigned omitted = (orientation[2] >> 24) & 3u;
    const float sqrt2 = 1.41421356237309504880f;
    const float c0 = ((float)orientation[0] / 67108864.0f - 0.5f) * sqrt2;
    const float c1 = ((float)orientation[1] / 33554432.0f - 0.5f) * sqrt2;
    const float c2 =
        ((float)(orientation[2] & 0x00FFFFFFu) / 16777216.0f - 0.5f) * sqrt2;

    float wire[4];
    const float retained = c0 * c0 + c1 * c1 + c2 * c2;
    if (retained > 1.0f) return false;
    wire[omitted] = sqrtf(1.0f - retained);
    wire[(omitted + 1u) & 3u] = c0;
    wire[(omitted + 2u) & 3u] = c1;
    wire[(omitted + 3u) & 3u] = c2;

    // wire is w/x/y/z; canonical is x/y/z/w.
    out_xyzw[0] = wire[1];
    out_xyzw[1] = wire[2];
    out_xyzw[2] = wire[3];
    out_xyzw[3] = wire[0];
    return true;
}

// Total rotation angle of a quaternion, in degrees, sign-independent.
static float quaternion_angle_deg(const float q[4])
{
    float w = q[3];
    if (w < 0.0f) w = -w;
    if (w > 1.0f) w = 1.0f;
    return 2.0f * acosf(w) * 57.2957795f;
}

// Unit rotation axis of a quaternion, canonicalized so w >= 0.
static void quaternion_axis(const float q[4], float axis[3])
{
    const float sign = q[3] < 0.0f ? -1.0f : 1.0f;
    float v[3] = { q[0] * sign, q[1] * sign, q[2] * sign };
    const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n < 1e-9f) {
        axis[0] = axis[1] = axis[2] = 0.0f;
        return;
    }
    for (unsigned i = 0; i < 3; ++i) axis[i] = v[i] / n;
}

// ---------------------------------------------------------------- drive side

#define COUNTS_PER_DPS 16.384f
#define STREAM_SAMPLES 40u
#define SENSOR_PERIOD_US 8000u   // the Android companion's 125 Hz report cadence
#define WARMUP_SAMPLES 96u

typedef struct {
    ns2_ds5_motion_state_t state;
    uint32_t now_us;
    uint32_t sequence;
} rig_t;

static void rig_init(rig_t *rig)
{
    memset(rig, 0, sizeof(*rig));
    ns2_ds5_motion_reset(&rig->state);
    rig->now_us = 1000000u;
}

static void make_sample(switch_pro_input_t *in, const float rate_dps[3],
                        uint32_t sequence)
{
    memset(in, 0, sizeof(*in));
    in->has_motion = 1;
    in->motion_sequence = sequence;
    for (unsigned i = 0; i < 3; ++i) {
        float counts = rate_dps[i] * COUNTS_PER_DPS;
        if (counts > 32767.0f) counts = 32767.0f;
        if (counts < -32768.0f) counts = -32768.0f;
        in->gyro[i] = (int16_t)(counts < 0.0f ? counts - 0.5f : counts + 0.5f);
    }
    // Resting gravity on the carrier's face-normal slot, at the genuine scale.
    in->accel[0] = 0;
    in->accel[1] = 0;
    in->accel[2] = 4096;
}

// Feed the translator a run of samples produced at a fixed SENSOR cadence but
// DELIVERED at the arrival times the caller supplies. Returns the integrated
// angle the encoder ended up reporting.
//
// arrival_us[] is the host time each sample was consumed at. When
// sensor_clock is true the sample also carries an authored sensor timestamp
// advancing at the true cadence, which is the DualSense's situation.
static float run_stream(const float rate_dps[3], unsigned count,
                        uint32_t sensor_period_us,
                        const uint32_t *arrival_offsets_us,
                        bool sensor_clock, float axis_out[3])
{
    rig_t rig;
    rig_init(&rig);

    // Warm the bias estimator up on genuinely still samples first: the encoder
    // integrates nothing until it trusts its zero-rate reference, which is
    // correct behavior and must not be mistaken for lost motion.
    // Long enough that even a scenario where the minimum-period gate rejects
    // every other sample still reaches the 32 accepted still samples the bias
    // estimator requires. A short warmup silently produces zero motion, which is
    // a real failure mode but a different one -- see test_warmup_starvation().
    const float still[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t t = rig.now_us;
    for (unsigned i = 0; i < WARMUP_SAMPLES; ++i) {
        switch_pro_input_t in;
        make_sample(&in, still, ++rig.sequence);
        if (sensor_clock) {
            // (i + 1) so the first motion sample below is exactly one period
            // after the last warmup sample. Numbering warmup from 0 leaves a
            // double-length first interval, which shows up as a real-looking
            // over-integration that is entirely the harness's fault.
            in.motion_timestamp = (i + 1u) * sensor_period_us * 3u;  // 0.33 us ticks
            in.motion_timestamp_valid = 1;
        }
        t += sensor_period_us;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }

    const uint32_t base_sensor = WARMUP_SAMPLES * sensor_period_us;
    const uint32_t base_host = t;
    for (unsigned i = 0; i < count; ++i) {
        switch_pro_input_t in;
        make_sample(&in, rate_dps, ++rig.sequence);
        if (sensor_clock) {
            in.motion_timestamp = (base_sensor + (i + 1u) * sensor_period_us) * 3u;
            in.motion_timestamp_valid = 1;
        }
        ns2_ds5_motion_update(&rig.state, &in,
                              base_host + arrival_offsets_us[i]);
    }

    uint8_t pdu[30];
    if (!ns2_ds5_motion_build(&rig.state, pdu)) return -1.0f;
    float q[4];
    if (!decode_quaternion(pdu, q)) return -2.0f;
    if (axis_out) quaternion_axis(q, axis_out);
    return quaternion_angle_deg(q);
}

// ------------------------------------------------------------------ scenarios


static void fill_regular(uint32_t *out, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        out[i] = (i + 1u) * SENSOR_PERIOD_US;
}

// Bluetooth Classic HID does not deliver a 125 Hz stream at an even 8 ms. It
// bunches: a report is late, then the next arrives almost immediately behind it.
// This models that without inventing a distribution -- it is a strict
// alternation of "late" and "immediately after", preserving the same MEAN rate,
// so any error it produces is attributable to pairing, not to a rate change.
static void fill_bursty(uint32_t *out, unsigned count)
{
    uint32_t t = 0;
    for (unsigned i = 0; i < count; ++i) {
        t += (i & 1u) ? 2000u : 14000u;  // mean still 8000 us
        out[i] = t;
    }
}

static void test_pure_axis_isolation(void)
{
    static const struct {
        const char *name;
        float rate[3];
        unsigned axis;
    } cases[] = {
        { "pure pitch (carrier X)", { 60.0f, 0.0f, 0.0f }, 0 },
        { "pure roll  (carrier Y)", { 0.0f, 60.0f, 0.0f }, 1 },
        { "pure yaw   (carrier Z)", { 0.0f, 0.0f, 60.0f }, 2 },
    };

    uint32_t arrivals[STREAM_SAMPLES];
    fill_regular(arrivals, STREAM_SAMPLES);

    for (unsigned c = 0; c < 3; ++c) {
        float axis[3];
        const float angle = run_stream(cases[c].rate, STREAM_SAMPLES,
                                       SENSOR_PERIOD_US, arrivals, false, axis);
        char msg[160];

        snprintf(msg, sizeof(msg), "%s: encoder produced a decodable rotation",
                 cases[c].name);
        check(angle >= 0.0f, msg);
        if (angle < 0.0f) continue;

        // 40 samples x 8 ms x 60 dps = 19.2 degrees.
        const float expected = 60.0f * (float)STREAM_SAMPLES *
                               (float)SENSOR_PERIOD_US / 1000000.0f;
        snprintf(msg, sizeof(msg),
                 "%s: angle %.3f deg within 2%% of expected %.3f",
                 cases[c].name, (double)angle, (double)expected);
        check(fabsf(angle - expected) <= expected * 0.02f, msg);

        // Leakage: the rotation axis must be the driven axis. Any component on
        // the other two is an axis error in the transform chain, which is
        // exactly what interactive testing cannot measure reliably.
        for (unsigned a = 0; a < 3; ++a) {
            if (a == cases[c].axis) continue;
            snprintf(msg, sizeof(msg),
                     "%s: leakage into axis %u is %.5f (limit 0.01)",
                     cases[c].name, a, (double)fabsf(axis[a]));
            check(fabsf(axis[a]) <= 0.01f, msg);
        }
        snprintf(msg, sizeof(msg), "%s: driven axis magnitude %.5f >= 0.999",
                 cases[c].name, (double)fabsf(axis[cases[c].axis]));
        check(fabsf(axis[cases[c].axis]) >= 0.999f, msg);
    }
}

// THE CORE MEASUREMENT.
//
// A CONSTANT rate cannot expose this defect: the accepted intervals still sum to
// the true elapsed time, so the endpoint comes out right whichever clock is
// used. (Measured: constant-rate bursty error is -0.17 deg over 28.8 deg.) Real
// aiming is not constant rate. When the rate VARIES, pairing each sample with an
// arrival interval it did not occur over produces trajectory error that a player
// perceives as choppiness -- and the error is largest exactly when the motion is
// changing fastest, which is when it is most visible.
//
// Ground truth is integrated at the sensor cadence, which is by definition the
// correct pairing.
static float varying_rate_dps(unsigned index)
{
    // ~2 Hz, the fast end of deliberate hand aiming, peak 120 dps.
    const float t = (float)index * (float)SENSOR_PERIOD_US / 1000000.0f;
    return 120.0f * sinf(6.2831853f * 2.0f * t);
}

static float run_varying(const uint32_t *arrival_offsets_us, bool sensor_clock,
                         float *worst_error_deg)
{
    rig_t rig;
    rig_init(&rig);
    const float still[3] = { 0.0f, 0.0f, 0.0f };

    uint32_t t = rig.now_us;
    for (unsigned i = 0; i < WARMUP_SAMPLES; ++i) {
        switch_pro_input_t in;
        make_sample(&in, still, ++rig.sequence);
        if (sensor_clock) {
            in.motion_timestamp = (i + 1u) * SENSOR_PERIOD_US * 3u;
            in.motion_timestamp_valid = 1;
        }
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }

    const uint32_t base_host = t;
    float truth_deg = 0.0f;
    float worst = 0.0f;
    for (unsigned i = 0; i < STREAM_SAMPLES; ++i) {
        const float rate = varying_rate_dps(i);
        const float rate3[3] = { 0.0f, 0.0f, rate };
        switch_pro_input_t in;
        make_sample(&in, rate3, ++rig.sequence);
        if (sensor_clock) {
            in.motion_timestamp =
                (WARMUP_SAMPLES + i + 1u) * SENSOR_PERIOD_US * 3u;
            in.motion_timestamp_valid = 1;
        }
        ns2_ds5_motion_update(&rig.state, &in,
                              base_host + arrival_offsets_us[i]);

        // Correct pairing: this sample's rate over one sensor period.
        truth_deg += rate * (float)SENSOR_PERIOD_US / 1000000.0f;

        uint8_t pdu[30];
        if (!ns2_ds5_motion_build(&rig.state, pdu)) continue;
        float q[4];
        if (!decode_quaternion(pdu, q)) continue;
        float reported = quaternion_angle_deg(q);
        // The trajectory crosses zero; compare signed magnitude against |truth|.
        const float err = fabsf(reported - fabsf(truth_deg));
        if (err > worst) worst = err;
    }
    if (worst_error_deg) *worst_error_deg = worst;
    return truth_deg;
}

static void test_varying_rate_jitter(void)
{
    uint32_t regular[STREAM_SAMPLES], bursty[STREAM_SAMPLES];
    fill_regular(regular, STREAM_SAMPLES);
    fill_bursty(bursty, STREAM_SAMPLES);

    float err_host_even = 0.0f, err_host_bursty = 0.0f, err_sensor_bursty = 0.0f;
    run_varying(regular, false, &err_host_even);
    run_varying(bursty, false, &err_host_bursty);
    run_varying(bursty, true, &err_sensor_bursty);

    printf("      worst trajectory error, host clock + even arrival    %.3f deg\n",
           (double)err_host_even);
    printf("      worst trajectory error, host clock + bursty arrival  %.3f deg\n",
           (double)err_host_bursty);
    printf("      worst trajectory error, SENSOR clock + bursty        %.3f deg\n",
           (double)err_sensor_bursty);

    check(err_host_bursty > err_host_even,
          "bursty arrival measurably degrades a varying-rate trajectory on the host clock");
    check(err_sensor_bursty < err_host_bursty,
          "forwarding the source IMU clock recovers accuracy under bursty arrival");
}

// A source on the host clock loses every sample that arrives inside the
// translator's minimum period, and the rotation that sample carried is not
// recovered. A source with its own clock is not subject to that gate at all.
static void test_min_period_gate_drops_samples(void)
{
    const float rate[3] = { 0.0f, 0.0f, 90.0f };
    uint32_t tight[STREAM_SAMPLES];
    for (unsigned i = 0; i < STREAM_SAMPLES; ++i)
        tight[i] = (i + 1u) * 3000u;  // 3 ms: inside the 3800 us gate

    const float host =
        run_stream(rate, STREAM_SAMPLES, 3000u, tight, false, NULL);
    const float sensor =
        run_stream(rate, STREAM_SAMPLES, 3000u, tight, true, NULL);
    const float expected =
        90.0f * (float)STREAM_SAMPLES * 3000.0f / 1000000.0f;

    printf("      3 ms cadence expected       %.3f deg\n", (double)expected);
    printf("      host clock                  %.3f deg\n", (double)host);
    printf("      sensor clock                %.3f deg\n", (double)sensor);

    check(sensor >= 0.0f && fabsf(sensor - expected) <= expected * 0.05f,
          "a source with its own IMU clock is unaffected by the minimum-period gate");
    check(host >= 0.0f, "host-clock stream still produces a decodable orientation");
}

// A delivery gap longer than the anti-lurch ceiling permanently discards the
// rotation beyond it. Documented as intended for a stalled source; measured here
// so the cost is a number rather than an assumption.
static void test_host_clock_clamp_discards_rotation(void)
{
    const float rate[3] = { 0.0f, 0.0f, 90.0f };
    uint32_t gapped[STREAM_SAMPLES];
    for (unsigned i = 0; i < STREAM_SAMPLES; ++i)
        gapped[i] = (i + 1u) * 40000u;  // 40 ms: beyond the 16 ms clamp

    const float host =
        run_stream(rate, STREAM_SAMPLES, 40000u, gapped, false, NULL);
    const float expected =
        90.0f * (float)STREAM_SAMPLES * 40000.0f / 1000000.0f;
    const float clamped =
        90.0f * (float)STREAM_SAMPLES * 16000.0f / 1000000.0f;

    printf("      40 ms gaps, true rotation   %.3f deg\n", (double)expected);
    printf("      host clock reports          %.3f deg (clamp predicts %.3f)\n",
           (double)host, (double)clamped);

    check(host >= 0.0f, "long-gap stream still produces a decodable orientation");
    check(host < expected * 0.75f,
          "host clock provably under-integrates when gaps exceed the 16 ms clamp");
}

// Stopping and restarting must not step the orientation. A discontinuity here
// would be visible in game as a jump.
static void test_stop_start_has_no_jump(void)
{
    rig_t rig;
    rig_init(&rig);
    const float moving[3] = { 45.0f, 0.0f, 0.0f };
    const float still[3] = { 0.0f, 0.0f, 0.0f };

    uint32_t t = rig.now_us;
    for (unsigned i = 0; i < 64; ++i) {
        switch_pro_input_t in;
        make_sample(&in, still, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }

    float previous = 0.0f;
    float worst_step = 0.0f;
    bool have_previous = false;
    for (unsigned phase = 0; phase < 4; ++phase) {
        const float *rate = (phase & 1u) ? still : moving;
        for (unsigned i = 0; i < 20; ++i) {
            switch_pro_input_t in;
            make_sample(&in, rate, ++rig.sequence);
            t += SENSOR_PERIOD_US;
            ns2_ds5_motion_update(&rig.state, &in, t);

            uint8_t pdu[30];
            if (!ns2_ds5_motion_build(&rig.state, pdu)) continue;
            float q[4];
            if (!decode_quaternion(pdu, q)) continue;
            const float angle = quaternion_angle_deg(q);
            if (have_previous) {
                const float step = fabsf(angle - previous);
                if (step > worst_step) worst_step = step;
            }
            previous = angle;
            have_previous = true;
        }
    }

    // One 8 ms step at 45 dps is 0.36 deg. Allow generous headroom; a real
    // discontinuity is orders of magnitude larger than this.
    printf("      worst single-sample step    %.4f deg\n", (double)worst_step);
    check(worst_step < 1.0f, "stop/start produces no orientation jump");
}

// A full revolution crosses every chart boundary. The encoder must never refuse
// to build, and the angle must stay bounded and continuous through each swap.
static void test_full_revolution_survives_chart_transitions(void)
{
    rig_t rig;
    rig_init(&rig);
    const float still[3] = { 0.0f, 0.0f, 0.0f };
    const float spin[3] = { 0.0f, 0.0f, 180.0f };

    uint32_t t = rig.now_us;
    for (unsigned i = 0; i < 64; ++i) {
        switch_pro_input_t in;
        make_sample(&in, still, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }

    unsigned states_seen = 0;
    unsigned state_mask = 0;
    unsigned rejects = 0;
    // 180 dps for 2 s is a full turn.
    for (unsigned i = 0; i < 250; ++i) {
        switch_pro_input_t in;
        make_sample(&in, spin, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);

        uint8_t pdu[30];
        if (!ns2_ds5_motion_build(&rig.state, pdu)) {
            rejects++;
            continue;
        }
        uint32_t orientation[3];
        if (!ns2_motion_pdu30_get_orientation(pdu, orientation)) continue;
        const unsigned st = (orientation[2] >> 24) & 3u;
        if (!(state_mask & (1u << st))) {
            state_mask |= 1u << st;
            states_seen++;
        }
    }

    printf("      chart states visited        %u (mask 0x%X), build rejects %u\n",
           states_seen, state_mask, rejects);
    check(rejects == 0, "a full revolution never fails to encode");
    check(states_seen >= 2,
          "a full revolution actually exercises more than one chart state");
    check(rig.state.representation_rejects == 0,
          "no orientation was rejected as unrepresentable");
}

// Saturation: the interchange scale tops out at +-2000 dps. Confirm the encoder
// stays well-defined rather than wrapping when a source clips.
static void test_saturated_rate_is_well_defined(void)
{
    const float rate[3] = { 0.0f, 0.0f, 1999.0f };
    uint32_t arrivals[STREAM_SAMPLES];
    fill_regular(arrivals, STREAM_SAMPLES);
    float axis[3];
    const float angle =
        run_stream(rate, STREAM_SAMPLES, SENSOR_PERIOD_US, arrivals, false, axis);
    check(angle >= 0.0f, "full-scale rate still encodes a valid orientation");
}

// The minimum-period gate rejects samples without advancing the accepted-sample
// clock, so a source delivering faster than the gate has its EFFECTIVE rate
// halved. The bias estimator needs 32 CONSECUTIVE accepted still samples, and any
// real movement resets that counter to zero. If a source starts moving before
// warmup completes, the encoder integrates nothing at all -- silently.
static void test_warmup_starvation(void)
{
    rig_t rig;
    rig_init(&rig);
    const float still[3] = { 0.0f, 0.0f, 0.0f };
    const float spin[3] = { 0.0f, 0.0f, 90.0f };

    // 40 still samples at a 3 ms cadence: plenty at face value, but the gate
    // accepts only about half of them.
    uint32_t t = rig.now_us;
    for (unsigned i = 0; i < 40; ++i) {
        switch_pro_input_t in;
        make_sample(&in, still, ++rig.sequence);
        t += 3000u;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }
    const bool ready_after_short_warmup = rig.state.bias_ready;

    for (unsigned i = 0; i < 60; ++i) {
        switch_pro_input_t in;
        make_sample(&in, spin, ++rig.sequence);
        t += 3000u;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }
    uint8_t pdu[30];
    float angle = -1.0f;
    if (ns2_ds5_motion_build(&rig.state, pdu)) {
        float q[4];
        if (decode_quaternion(pdu, q)) angle = quaternion_angle_deg(q);
    }

    printf("      bias ready after 40 gated still samples: %s\n",
           ready_after_short_warmup ? "yes" : "NO");
    printf("      rotation reported after 60 moving samples: %.3f deg\n",
           (double)angle);

    // This is a characterization, not a pass/fail on current behavior: it pins
    // the relationship so a future change to the gate or the warmup constant
    // cannot silently alter it.
    check(angle >= 0.0f, "warmup-starved stream still produces a decodable PDU");
    if (!ready_after_short_warmup) {
        check(angle < 1.0f,
              "when warmup never completes, NO motion is integrated (silent, by design)");
    }
}

// The Android bridge authors its clock in 100 us ticks in a 16-bit field. Prove
// the encoder handles that unit AND its wrap, since a mishandled wrap would
// present as a single enormous dt -- a visible lurch every 6.5 seconds.
static void test_android_timestamp_unit_and_wrap(void)
{
    rig_t rig;
    rig_init(&rig);
    const float still[3] = { 0.0f, 0.0f, 0.0f };
    const float spin[3] = { 0.0f, 0.0f, 90.0f };

    // Start close enough to the 16-bit ceiling that the run crosses it.
    uint32_t ticks = 0xFFC0u;
    uint32_t t = rig.now_us;
    for (unsigned i = 0; i < WARMUP_SAMPLES; ++i) {
        switch_pro_input_t in;
        make_sample(&in, still, ++rig.sequence);
        ticks = (ticks + 80u) & 0xFFFFu;          // 80 ticks = 8 ms
        in.motion_timestamp = ticks;
        in.motion_timestamp_unit = SWITCH_MOTION_TS_100US_16;
        in.motion_timestamp_valid = 1;
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }

    const uint32_t before = rig.state.sensor_timestamp_invalid;
    for (unsigned i = 0; i < STREAM_SAMPLES; ++i) {
        switch_pro_input_t in;
        make_sample(&in, spin, ++rig.sequence);
        ticks = (ticks + 80u) & 0xFFFFu;
        in.motion_timestamp = ticks;
        in.motion_timestamp_unit = SWITCH_MOTION_TS_100US_16;
        in.motion_timestamp_valid = 1;
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }

    uint8_t pdu[30];
    float angle = -1.0f;
    if (ns2_ds5_motion_build(&rig.state, pdu)) {
        float q[4];
        if (decode_quaternion(pdu, q)) angle = quaternion_angle_deg(q);
    }
    const float expected =
        90.0f * (float)STREAM_SAMPLES * (float)SENSOR_PERIOD_US / 1000000.0f;

    printf("      100 us unit, wrapping: %.3f deg (expected %.3f), "
           "rejected dt count %u\n",
           (double)angle, (double)expected,
           rig.state.sensor_timestamp_invalid - before);

    check(fabsf(angle - expected) <= expected * 0.02f,
          "100 us / 16-bit source clock integrates the correct angle across a wrap");
    check(rig.state.sensor_timestamp_invalid == before,
          "no interval was rejected as implausible while crossing the 16-bit wrap");
}


// ===========================================================================
// Chart / state transition continuity
// ===========================================================================
//
// The encoder transmits three of the four quaternion components and names the
// omitted one in a 2-bit chart state. Hardware reported a rare residual artifact
// that subjectively resembled "the representation struggling when it changes
// internal state", so this section measures exactly that.
//
// A state change is NOT a defect. A change in the represented PHYSICAL
// orientation across that boundary is.
//
// Two things make a naive test lie here:
//   * q and -q are the same rotation. The encoder canonicalizes the omitted
//     component positive, so every zero crossing of the omitted lane flips the
//     sign of all three transmitted lanes. Comparing raw components would report
//     a 180-degree "jump" that does not physically exist.
//   * the chart is RETAINED until a transmitted lane leaves +-1/sqrt(2), so
//     transitions are sparse and a short run can miss them entirely. Every
//     scenario below asserts it actually observed transitions.

// Sign-invariant angular distance between two orientations, in degrees.
static float orientation_delta_deg(const float a[4], const float b[4])
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f) dot = -dot;          // q and -q are the same rotation
    if (dot > 1.0f) dot = 1.0f;
    return 2.0f * acosf(dot) * 57.2957795f;
}

typedef struct {
    unsigned samples;
    unsigned transitions;
    unsigned build_failures;
    unsigned alternations;      // state returned to the one before it
                                // (NOT a defect on its own -- see the note in
                                //  test_chart_transition_continuity)
    float worst_step_at_transition;
    float worst_step_elsewhere;
    unsigned states_mask;
} chart_stats_t;

// Drive a constant-rate trajectory and measure orientation continuity, splitting
// the worst per-sample angular step into "across a chart change" and "not".
// If the encoder is continuous, those two numbers are the same.
static void run_chart_scan(const float rate_dps[3], unsigned samples,
                           chart_stats_t *out)
{
    rig_t rig;
    rig_init(&rig);
    memset(out, 0, sizeof(*out));

    const float still[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t t = rig.now_us;
    for (unsigned i = 0; i < WARMUP_SAMPLES; ++i) {
        switch_pro_input_t in;
        make_sample(&in, still, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }

    float previous_q[4];
    bool have_previous = false;
    unsigned previous_state = 0, before_previous_state = 0;
    bool have_previous_state = false;

    for (unsigned i = 0; i < samples; ++i) {
        switch_pro_input_t in;
        make_sample(&in, rate_dps, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);

        uint8_t pdu[30];
        if (!ns2_ds5_motion_build(&rig.state, pdu)) {
            out->build_failures++;
            continue;
        }
        uint32_t orientation[3];
        if (!ns2_motion_pdu30_get_orientation(pdu, orientation)) continue;
        const unsigned state = (orientation[2] >> 24) & 3u;
        out->states_mask |= 1u << state;

        float q[4];
        if (!decode_quaternion(pdu, q)) { out->build_failures++; continue; }
        out->samples++;

        if (have_previous) {
            const float step = orientation_delta_deg(previous_q, q);
            const bool changed = have_previous_state && state != previous_state;
            if (changed) {
                out->transitions++;
                if (step > out->worst_step_at_transition)
                    out->worst_step_at_transition = step;
                // A -> B -> A across three consecutive samples.
                if (out->transitions > 1u && state == before_previous_state)
                    out->alternations++;
            } else if (step > out->worst_step_elsewhere) {
                out->worst_step_elsewhere = step;
            }
            if (changed) {
                before_previous_state = previous_state;
            }
        }
        memcpy(previous_q, q, sizeof(previous_q));
        previous_state = state;
        have_previous_state = true;
        have_previous = true;
    }
}

static void report_scan(const char *name, const chart_stats_t *st, float rate_dps)
{
    const float expected_step =
        rate_dps * (float)SENSOR_PERIOD_US / 1000000.0f;
    printf("      %-26s states=0x%X transitions=%2u alt=%u fail=%u "
           "step@sw=%.4f elsewhere=%.4f (nominal %.4f)\n",
           name, st->states_mask, st->transitions, st->alternations,
           st->build_failures,
           (double)st->worst_step_at_transition,
           (double)st->worst_step_elsewhere,
           (double)expected_step);
}

static void test_chart_transition_continuity(void)
{
    static const struct { const char *name; float rate[3]; float mag; unsigned n; } cases[] = {
        { "slow pitch  30dps",   {  30.0f, 0.0f, 0.0f },  30.0f, 2000u },
        { "slow roll   30dps",   { 0.0f,  30.0f, 0.0f },  30.0f, 2000u },
        { "slow yaw    30dps",   { 0.0f, 0.0f,  30.0f },  30.0f, 2000u },
        { "moderate pitch 180",  { 180.0f, 0.0f, 0.0f }, 180.0f,  600u },
        { "moderate roll  180",  { 0.0f, 180.0f, 0.0f }, 180.0f,  600u },
        { "moderate yaw   180",  { 0.0f, 0.0f, 180.0f }, 180.0f,  600u },
        { "fast yaw       720",  { 0.0f, 0.0f, 720.0f }, 720.0f,  400u },
        { "combined axes",       { 120.0f, 90.0f, 60.0f }, 165.5f, 800u },
    };

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        chart_stats_t st;
        run_chart_scan(cases[c].rate, cases[c].n, &st);
        report_scan(cases[c].name, &st, cases[c].mag);

        char msg[192];
        snprintf(msg, sizeof(msg), "%s: encoder never failed to build",
                 cases[c].name);
        check(st.build_failures == 0u, msg);

        snprintf(msg, sizeof(msg), "%s: actually crossed a chart boundary",
                 cases[c].name);
        check(st.transitions > 0u, msg);

        // THE MEASUREMENT. A chart change must not move the physical
        // orientation any more than an ordinary sample does. Allow a small
        // absolute margin for float/quantization noise on top of the nominal
        // per-sample step.
        const float nominal = cases[c].mag * (float)SENSOR_PERIOD_US / 1000000.0f;
        const float limit = nominal * 1.5f + 0.01f;
        snprintf(msg, sizeof(msg),
                 "%s: step across a chart change (%.4f) is within %.4f",
                 cases[c].name, (double)st.worst_step_at_transition,
                 (double)limit);
        check(st.worst_step_at_transition <= limit, msg);
    }
}

// Park the orientation near a chart-selection threshold and oscillate across it.
// This is the shape that would produce state thrashing if the selection rule had
// no retention.
static void test_boundary_dither_does_not_thrash(void)
{
    rig_t rig;
    rig_init(&rig);
    const float still[3] = { 0.0f, 0.0f, 0.0f };

    uint32_t t = rig.now_us;
    for (unsigned i = 0; i < WARMUP_SAMPLES; ++i) {
        switch_pro_input_t in;
        make_sample(&in, still, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }

    // Drive to ~90 degrees about Z, which is exactly where a transmitted lane
    // reaches 1/sqrt(2) and the chart must change.
    const float spin[3] = { 0.0f, 0.0f, 180.0f };
    for (unsigned i = 0; i < 63; ++i) {
        switch_pro_input_t in;
        make_sample(&in, spin, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }

    // Now dither back and forth across that point.
    unsigned transitions = 0, failures = 0;
    float worst = 0.0f;
    float previous_q[4];
    bool have_previous = false;
    unsigned previous_state = 0;
    bool have_state = false;

    for (unsigned i = 0; i < 400; ++i) {
        const float dir = (i / 3u) % 2u ? -60.0f : 60.0f;
        const float rate[3] = { 0.0f, 0.0f, dir };
        switch_pro_input_t in;
        make_sample(&in, rate, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);

        uint8_t pdu[30];
        if (!ns2_ds5_motion_build(&rig.state, pdu)) { failures++; continue; }
        uint32_t orientation[3];
        if (!ns2_motion_pdu30_get_orientation(pdu, orientation)) continue;
        const unsigned state = (orientation[2] >> 24) & 3u;
        float q[4];
        if (!decode_quaternion(pdu, q)) { failures++; continue; }

        if (have_previous) {
            const float step = orientation_delta_deg(previous_q, q);
            if (step > worst) worst = step;
        }
        if (have_state && state != previous_state) transitions++;
        memcpy(previous_q, q, sizeof(previous_q));
        previous_state = state;
        have_state = true;
        have_previous = true;
    }

    printf("      boundary dither: %u chart changes over 400 samples, "
           "worst orientation step %.4f deg, build failures %u\n",
           transitions, (double)worst, failures);

    check(failures == 0u, "dithering on a chart boundary never fails to build");
    // 60 dps x 8 ms = 0.48 deg nominal. Anything near that is continuous
    // regardless of how many times the representation changed.
    check(worst < 1.0f,
          "dithering on a chart boundary causes no orientation discontinuity");
}

// Many revolutions, to visit every chart state and every sign branch.
// Rotate about a different body axis every phase. This is what actually walks
// the omitted component around all four charts; see the note at the call site.
static void run_chart_scan_phased(chart_stats_t *out)
{
    static const float axes[6][3] = {
        { 240.0f, 0.0f, 0.0f }, { 0.0f, 240.0f, 0.0f }, { 0.0f, 0.0f, 240.0f },
        { -240.0f, 0.0f, 0.0f }, { 0.0f, -240.0f, 0.0f }, { 170.0f, 170.0f, 0.0f },
    };
    rig_t rig;
    rig_init(&rig);
    memset(out, 0, sizeof(*out));

    const float still[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t t = rig.now_us;
    for (unsigned i = 0; i < WARMUP_SAMPLES; ++i) {
        switch_pro_input_t in;
        make_sample(&in, still, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }

    float previous_q[4];
    bool have_previous = false;
    unsigned previous_state = 0;
    bool have_state = false;

    for (unsigned phase = 0; phase < 24u; ++phase) {
        const float *rate = axes[phase % 6u];
        for (unsigned i = 0; i < 120u; ++i) {
            switch_pro_input_t in;
            make_sample(&in, rate, ++rig.sequence);
            t += SENSOR_PERIOD_US;
            ns2_ds5_motion_update(&rig.state, &in, t);

            uint8_t pdu[30];
            if (!ns2_ds5_motion_build(&rig.state, pdu)) { out->build_failures++; continue; }
            uint32_t orientation[3];
            if (!ns2_motion_pdu30_get_orientation(pdu, orientation)) continue;
            const unsigned state = (orientation[2] >> 24) & 3u;
            out->states_mask |= 1u << state;
            float q[4];
            if (!decode_quaternion(pdu, q)) { out->build_failures++; continue; }
            out->samples++;

            if (have_previous) {
                const float step = orientation_delta_deg(previous_q, q);
                if (have_state && state != previous_state) {
                    out->transitions++;
                    if (step > out->worst_step_at_transition)
                        out->worst_step_at_transition = step;
                } else if (step > out->worst_step_elsewhere) {
                    out->worst_step_elsewhere = step;
                }
            }
            memcpy(previous_q, q, sizeof(previous_q));
            previous_state = state;
            have_state = true;
            have_previous = true;
        }
    }
}

static unsigned popcount4(unsigned mask)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 4u; ++i) if (mask & (1u << i)) n++;
    return n;
}

static void test_multiple_revolutions_and_tumble(void)
{
    // A rotation about ONE axis only ever has two non-zero quaternion
    // components (w and that axis), so only TWO charts are reachable no matter
    // how long it runs. The state alternating between them is the correct
    // representation following the largest component, not thrashing -- the
    // continuity numbers above are what decide that.
    chart_stats_t st;
    const float yaw[3] = { 0.0f, 0.0f, 360.0f };
    run_chart_scan(yaw, 1500u, &st);
    report_scan("6 revolutions yaw", &st, 360.0f);
    check(st.build_failures == 0u, "multiple revolutions never fail to build");
    check(popcount4(st.states_mask) == 2u,
          "a single-axis rotation reaches exactly the two charts it can reach");
    const float nominal = 360.0f * (float)SENSOR_PERIOD_US / 1000000.0f;
    check(st.worst_step_at_transition <= nominal * 1.5f + 0.01f,
          "no orientation discontinuity across a single-axis chart change");

    // Reaching all four charts needs a CHANGING rotation axis. A constant
    // angular-velocity vector -- even with incommensurate per-axis rates -- is
    // still rotation about one fixed body axis, so the same component stays
    // largest and only two charts are ever reachable. Measured: {137,89,211}
    // dps over 3000 samples still visits only states 0 and 3.
    chart_stats_t phased;
    run_chart_scan_phased(&phased);
    report_scan("phased axis changes", &phased, 240.0f);
    check(phased.build_failures == 0u, "a phased trajectory never fails to build");
    check(popcount4(phased.states_mask) >= 3u,
          "changing the rotation axis exercises at least three of the four charts");
    const float tn = 240.0f * (float)SENSOR_PERIOD_US / 1000000.0f;
    check(phased.worst_step_at_transition <= tn * 1.5f + 0.01f,
          "no orientation discontinuity across any chart change while tumbling");
}

// Packed-layout round trip, independent of any trajectory: every state, and
// values at the extremes of each field, must survive pack -> unpack exactly and
// must not contaminate each other.
static void test_packed_layout_round_trip(void)
{
    static const uint32_t probes[] = {
        0u, 1u, 0x00FFFFFFu, 0x01000000u, 0x01FFFFFFu, 0x02000000u,
        0x03FFFFFFu, 0x01234567u, 0x02AAAAAAu, 0x00555555u,
    };
    const unsigned n = sizeof(probes) / sizeof(probes[0]);
    unsigned mismatches = 0;

    for (unsigned state = 0; state < 4u; ++state) {
        for (unsigned a = 0; a < n; ++a) {
            for (unsigned b = 0; b < n; ++b) {
                uint8_t pdu[30];
                // Fill with a non-zero pattern so a lost bit shows up as a
                // difference rather than coincidentally matching zero, and so
                // the preserved-bit check below is meaningful.
                memset(pdu, 0xA5, sizeof(pdu));
                uint8_t before[30];
                memcpy(before, pdu, sizeof(before));

                const uint32_t values[3] = {
                    probes[a],
                    probes[b],
                    (probes[(a + b) % n] & 0x00FFFFFFu) | (state << 24),
                };
                if (!ns2_motion_pdu30_set_orientation(pdu, values)) {
                    mismatches++;
                    continue;
                }
                uint32_t back[3];
                if (!ns2_motion_pdu30_get_orientation(pdu, back)) {
                    mismatches++;
                    continue;
                }
                for (unsigned i = 0; i < 3; ++i) {
                    if (back[i] != (values[i] & 0x03FFFFFFu)) mismatches++;
                }
                if (((back[2] >> 24) & 3u) != state) mismatches++;

                // Bytes outside the carrier must be untouched, and the six
                // unrelated high bits of 0x04 / 0x08 / 0x0C must survive.
                for (unsigned k = 0; k < 30u; ++k) {
                    const bool carrier =
                        (k >= 5u && k <= 15u) || k == 4u;
                    if (!carrier && pdu[k] != before[k]) mismatches++;
                }
                if ((pdu[4] & 0xFCu) != (before[4] & 0xFCu)) mismatches++;
                if ((pdu[8] & 0xFCu) != (before[8] & 0xFCu)) mismatches++;
                if ((pdu[12] & 0xFCu) != (before[12] & 0xFCu)) mismatches++;
            }
        }
    }

    printf("      packed round trip: %u state/value combinations, %u mismatches\n",
           4u * n * n, mismatches);
    check(mismatches == 0u,
          "orientation packing round-trips exactly and preserves unrelated bits");
}


// Chart transitions are continuous (measured above), so the next candidate for a
// rare perceptible artifact is the bias estimator ABSORBING real motion.
//
// The stillness gate calls a sample "steady" when the de-biased rate is under
// NS2_DS5_GYRO_STILL_LIMIT (40 counts = 2.44 dps) and the frame-to-frame
// derivative is small. A slow, smooth, deliberate rotation satisfies BOTH: it is
// under the magnitude limit and it has almost no derivative. The estimator then
// adapts its zero-rate reference toward the real rotation rate and subtracts it,
// so very slow aiming decays toward no movement and "recovers" as soon as the
// player speeds up past the limit.
//
// This measures how much of a genuinely constant slow rotation survives.
static float surviving_fraction_at(float rate_dps, unsigned samples)
{
    rig_t rig;
    rig_init(&rig);
    const float still[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t t = rig.now_us;
    for (unsigned i = 0; i < WARMUP_SAMPLES; ++i) {
        switch_pro_input_t in;
        make_sample(&in, still, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }
    const float rate[3] = { 0.0f, 0.0f, rate_dps };
    for (unsigned i = 0; i < samples; ++i) {
        switch_pro_input_t in;
        make_sample(&in, rate, ++rig.sequence);
        t += SENSOR_PERIOD_US;
        ns2_ds5_motion_update(&rig.state, &in, t);
    }
    uint8_t pdu[30];
    if (!ns2_ds5_motion_build(&rig.state, pdu)) return -1.0f;
    float q[4];
    if (!decode_quaternion(pdu, q)) return -1.0f;
    const float expected =
        rate_dps * (float)samples * (float)SENSOR_PERIOD_US / 1000000.0f;
    if (expected <= 0.0f) return -1.0f;
    return quaternion_angle_deg(q) / expected;
}

static void test_slow_rotation_is_not_absorbed_by_bias(void)
{
    static const float rates[] = { 0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f, 30.0f };
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        const float kept = surviving_fraction_at(rates[i], 400u);
        printf("      %5.1f dps constant: %5.1f%% of the rotation survives\n",
               (double)rates[i], (double)(kept * 100.0f));
    }
    // Characterization, deliberately not a hard gate on current behavior: the
    // point is to make the absorption threshold visible and to fail loudly if a
    // future change starts eating ordinary aiming speeds.
    const float fast = surviving_fraction_at(30.0f, 400u);
    check(fast > 0.95f, "ordinary aiming speed (30 dps) is not absorbed by the bias estimator");
}

int main(void)
{
    printf("-- pure-axis isolation --\n");
    test_pure_axis_isolation();
    printf("-- arrival jitter vs sensor clock --\n");
    test_varying_rate_jitter();
    printf("-- android 100us timestamp unit --\n");
    test_android_timestamp_unit_and_wrap();
    printf("-- warmup starvation --\n");
    test_warmup_starvation();
    printf("-- minimum-period gate --\n");
    test_min_period_gate_drops_samples();
    printf("-- host-clock anti-lurch clamp --\n");
    test_host_clock_clamp_discards_rotation();
    printf("-- stop/start continuity --\n");
    test_stop_start_has_no_jump();
    printf("-- chart transitions --\n");
    test_full_revolution_survives_chart_transitions();
    printf("-- chart transition continuity --\n");
    test_chart_transition_continuity();
    printf("-- boundary dither / thrash --\n");
    test_boundary_dither_does_not_thrash();
    printf("-- multiple revolutions --\n");
    test_multiple_revolutions_and_tumble();
    printf("-- slow-rotation bias absorption --\n");
    test_slow_rotation_is_not_absorbed_by_bias();
    printf("-- packed layout round trip --\n");
    test_packed_layout_round_trip();
    printf("-- saturation --\n");
    test_saturated_rate_is_well_defined();

    if (failures) {
        fprintf(stderr, "\nns2_motion_quality: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nns2_motion_quality: all checks passed\n");
    return 0;
}
