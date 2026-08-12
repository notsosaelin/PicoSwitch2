#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_motion_hybrid.h"
#include "ns2_motion_hybrid_projector.h"
#include "ns2_motion_pdu.h"

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void push_sample(ns2_motion_hybrid_projector_t *state,
                        uint32_t sequence, uint32_t us,
                        const int16_t accel[3], const int16_t gyro[3],
                        uint8_t calibration)
{
    ns2_motion_hybrid_projector_push(state, us, us * 3u, sequence,
                                    accel, gyro, calibration);
}

static void warm_and_align(ns2_motion_hybrid_projector_t *state,
                           uint32_t *sequence, uint32_t *us)
{
    const int16_t accel[3] = {0, 0, 4096};
    const int16_t still[3] = {0, 0, 0};
    ns2_motion_hybrid_projector_reset(state);
    for (unsigned i = 0; i < 36u; ++i) {
        *us += 4000u;
        push_sample(state, ++*sequence, *us, accel, still,
                    NS2_MOTION_HYBRID_CALIBRATION_READY);
    }
    check(state->translator.bias_ready, "stationary donor acquires bias");

    uint8_t carrier[NS2_MOTION_PDU30_LENGTH];
    check(ns2_ds5_motion_build(&state->translator, carrier),
          "aligned carrier fixture builds");
    ns2_motion_hybrid_project_result_t result;
    uint8_t out[NS2_MOTION_PDU40_LENGTH];
    check(!ns2_motion_hybrid_projector_project(
              state, carrier, sizeof(carrier), *us,
              NS2_MOTION_HYBRID_ACCEL, out, &result),
          "genuine 0x1e remains passthrough");
    check(result.reason == NS2_MOTION_HYBRID_LIVE_PASSTHROUGH_30,
          "carrier alignment reports passthrough");
    check(result.pose_aligned, "carrier alignment succeeds");
    check(memcmp(carrier, out, sizeof(carrier)) == 0,
          "carrier alignment never edits genuine bytes");
}

static void build_base(uint8_t out[NS2_MOTION_PDU40_LENGTH],
                       uint8_t status, uint16_t elapsed)
{
    ns2_motion40_high_rate_t fields;
    memset(&fields, 0, sizeof(fields));
    fields.tick = 0x345u;
    fields.elapsed_ticks = elapsed;
    fields.packing_mode = 3u;
    fields.status = status;
    fields.tail_value = 0x01C0u;
    fields.carrier[0] = 100;
    fields.carrier[1] = -200;
    fields.carrier[2] = 300;
    for (unsigned slot = 0; slot < 2u; ++slot)
        for (unsigned axis = 0; axis < 3u; ++axis)
            fields.accel[slot][axis] =
                1000 + (int32_t)(slot * 100u + axis * 10u);
    for (unsigned axis = 0; axis < 3u; ++axis)
        fields.gyro[0][axis] = 2000 + (int32_t)axis * 100;
    check(ns2_motion_pdu40_build_high_rate(out, &fields),
          "genuine-shaped high-rate base builds");
}

static unsigned changed_outside(uint32_t groups, const uint8_t *base,
                                const uint8_t *out, uint8_t length)
{
    uint8_t donor[NS2_MOTION_PDU40_LENGTH];
    uint8_t selected[NS2_MOTION_PDU40_LENGTH];
    memcpy(donor, out, length);
    check(ns2_motion_hybrid_splice(base, donor, length, groups,
                                   selected) == NS2_MOTION_HYBRID_OK,
          "selected-group verification splice succeeds");
    unsigned changed = 0u;
    for (unsigned i = 0; i < length; ++i)
        if (selected[i] != out[i]) changed++;
    return changed;
}

static void test_applied_and_fail_closed(void)
{
    ns2_motion_hybrid_projector_t state;
    uint32_t sequence = 0u;
    uint32_t us = 0u;
    warm_and_align(&state, &sequence, &us);

    const int16_t accel0[3] = {100, -200, 4080};
    const int16_t accel1[3] = {140, -160, 4070};
    const int16_t gyro0[3] = {20, -30, 40};
    const int16_t gyro1[3] = {30, -40, 50};
    us += 4000u;
    push_sample(&state, ++sequence, us, accel0, gyro0,
                NS2_MOTION_HYBRID_CALIBRATION_READY);
    us += 4000u;
    push_sample(&state, ++sequence, us, accel1, gyro1,
                NS2_MOTION_HYBRID_CALIBRATION_READY);

    uint8_t base[NS2_MOTION_PDU40_LENGTH];
    uint8_t out[NS2_MOTION_PDU40_LENGTH];
    ns2_motion_hybrid_project_result_t result;
    build_base(base, 0x00u, 8u);
    check(ns2_motion_hybrid_projector_project(
              &state, base, sizeof(base), us,
              NS2_MOTION_HYBRID_ACCEL | NS2_MOTION_HYBRID_GYRO,
              out, &result),
          "fresh aligned donor is applied");
    check(result.reason == NS2_MOTION_HYBRID_LIVE_APPLIED,
          "applied reason is explicit");
    check(result.applied_groups ==
              (NS2_MOTION_HYBRID_ACCEL | NS2_MOTION_HYBRID_GYRO),
          "only requested physical groups are marked applied");
    check(result.changed_bits != 0u, "applied donor changes payload bits");
    check(changed_outside(result.applied_groups, base, out,
                          sizeof(base)) == 0u,
          "projection preserves every bit outside selected groups");
    check(out[3] == base[3] && out[38] == base[38] && out[39] == base[39],
          "status and genuine temperature tail remain immutable");

    uint8_t fallback[NS2_MOTION_PDU40_LENGTH];
    check(!ns2_motion_hybrid_projector_project(
              &state, base, sizeof(base), us,
              NS2_MOTION_HYBRID_GYRO, fallback, &result),
          "unchanged donor sequence is rejected");
    check(result.reason == NS2_MOTION_HYBRID_LIVE_SOURCE_NOT_ADVANCED,
          "source-not-advanced fallback is diagnosed");
    check(memcmp(base, fallback, sizeof(base)) == 0,
          "source-not-advanced fallback is byte-exact genuine");

    us += 4000u;
    push_sample(&state, ++sequence, us, accel1, gyro1,
                NS2_MOTION_HYBRID_CALIBRATION_READY);
    build_base(base, NS2_MOTION40_STATUS_HIGH_RATE, 10u);
    check(!ns2_motion_hybrid_projector_project(
              &state, base, sizeof(base), us + 25000u,
              NS2_MOTION_HYBRID_GYRO, fallback, &result),
          "stale donor is rejected");
    check(result.reason == NS2_MOTION_HYBRID_LIVE_STALE_DONOR,
          "stale donor fallback is diagnosed");
    check(memcmp(base, fallback, sizeof(base)) == 0,
          "stale donor fallback is byte-exact genuine");

    build_base(base, NS2_MOTION40_STATUS_HIGH_RATE, 10u);
    base[1] = (uint8_t)((base[1] & 0x0Fu) | 0xC0u);
    check(!ns2_motion_hybrid_projector_project(
              &state, base, sizeof(base), us,
              NS2_MOTION_HYBRID_GYRO, fallback, &result),
          "normal-layout base is not guessed");
    check(result.reason == NS2_MOTION_HYBRID_LIVE_UNSUPPORTED_LAYOUT,
          "unsupported layout fallback is diagnosed");
    check(memcmp(base, fallback, sizeof(base)) == 0,
          "unsupported layout fallback is byte-exact genuine");
}

static void test_alignment_guards(void)
{
    ns2_motion_hybrid_projector_t state;
    ns2_motion_hybrid_projector_reset(&state);
    uint8_t carrier[NS2_MOTION_PDU30_LENGTH] = {0};
    uint8_t out[NS2_MOTION_PDU40_LENGTH];
    ns2_motion_hybrid_project_result_t result;
    check(!ns2_motion_hybrid_projector_project(
              &state, carrier, sizeof(carrier), 100u,
              NS2_MOTION_HYBRID_PREFIX, out, &result),
          "carrier waits for DualSense source");
    check(result.reason == NS2_MOTION_HYBRID_LIVE_WAIT_DS5,
          "missing source is diagnosed");
    check(memcmp(carrier, out, sizeof(carrier)) == 0,
          "missing source leaves carrier genuine");

    const uint8_t genuine_strict_invalid[NS2_MOTION_PDU30_LENGTH] = {
        0xDC,0x5E,0x00,0x0C,0x01,0xA8,0x2D,0x7C,0x00,0x8C,
        0xAB,0xCC,0x01,0xE2,0xC3,0xF3,0xD0,0x5A,0x23,0x0F,
        0xD4,0xB4,0x95,0x03,0x90,0x2E,0x9C,0x00,0x80,0x02,
    };
    float quaternion[4];
    check(!ns2_motion_pdu30_get_quaternion(
              genuine_strict_invalid, quaternion, NULL),
          "known genuine non-unit carrier is rejected, never normalized");
}

static void test_prefix_owns_both_carrier_lengths(void)
{
    ns2_motion_hybrid_projector_t state;
    uint32_t sequence = 0u;
    uint32_t us = 0u;
    warm_and_align(&state, &sequence, &us);

    const int16_t accel[3] = {0, 0, 4096};
    const int16_t still[3] = {0, 0, 0};
    us += 4000u;
    push_sample(&state, ++sequence, us, accel, still,
                NS2_MOTION_HYBRID_CALIBRATION_READY);

    uint8_t genuine30[NS2_MOTION_PDU30_LENGTH];
    check(ns2_ds5_motion_build(&state.translator, genuine30),
          "coherent donor carrier builds");
    genuine30[5] ^= 0x40u;  // Distinguishable but structurally valid base.

    uint8_t out[NS2_MOTION_PDU40_LENGTH];
    ns2_motion_hybrid_project_result_t result;
    check(ns2_motion_hybrid_projector_project(
              &state, genuine30, sizeof(genuine30), us,
              NS2_MOTION_HYBRID_PREFIX, out, &result),
          "prefix mode replaces the interleaved 0x1e carrier");
    check(result.reason == NS2_MOTION_HYBRID_LIVE_APPLIED,
          "0x1e donor carrier reports applied");
    check(result.applied_groups == NS2_MOTION_HYBRID_PREFIX,
          "0x1e applies only the prefix group");
    check(result.changed_bits != 0u,
          "0x1e prefix replacement changes the divergent base");

    uint8_t selected30[NS2_MOTION_PDU30_LENGTH];
    check(ns2_motion_hybrid_splice(
              genuine30, out, sizeof(genuine30), NS2_MOTION_HYBRID_PREFIX,
              selected30) == NS2_MOTION_HYBRID_OK,
          "0x1e selected-prefix verification splice succeeds");
    check(memcmp(selected30, out, sizeof(genuine30)) == 0,
          "0x1e replacement preserves every non-prefix bit");

    uint8_t genuine40[NS2_MOTION_PDU40_LENGTH];
    build_base(genuine40, NS2_MOTION40_STATUS_HIGH_RATE, 8u);
    check(ns2_motion_hybrid_projector_project(
              &state, genuine40, sizeof(genuine40), us + 60000u,
              NS2_MOTION_HYBRID_PREFIX, out, &result),
          "prefix mode holds donor orientation through a source gap");
    check(result.reason == NS2_MOTION_HYBRID_LIVE_APPLIED,
          "held 0x28 prefix remains applied instead of mixing sources");
    check(result.ds5_age_us > NS2_MOTION_HYBRID_MAX_DONOR_AGE_US,
          "test exercised a donor age beyond the physical-group freshness gate");
    check(changed_outside(NS2_MOTION_HYBRID_PREFIX, genuine40, out,
                          sizeof(genuine40)) == 0u,
          "held 0x28 prefix preserves every non-prefix bit");

    const uint16_t longer_elapsed[2] = {12u, 20u};
    for (unsigned i = 0; i < 2u; ++i) {
        uint8_t longer[NS2_MOTION_PDU40_LENGTH];
        memcpy(longer, genuine40, sizeof(longer));
        longer[1] = (uint8_t)((longer[1] & 0x0Fu) |
                              ((longer_elapsed[i] & 0x0Fu) << 4));
        longer[2] = (uint8_t)(longer_elapsed[i] >> 4);
        check(ns2_motion_hybrid_projector_project(
                  &state, longer, sizeof(longer), us + 60000u,
                  NS2_MOTION_HYBRID_PREFIX, out, &result),
              "prefix ownership spans normal and catch-up 0x28 layouts");
        check(result.reason == NS2_MOTION_HYBRID_LIVE_APPLIED,
              "long-cadence prefix remains donor-owned");
        check(changed_outside(NS2_MOTION_HYBRID_PREFIX, longer, out,
                              sizeof(longer)) == 0u,
              "long-cadence prefix preserves every non-prefix bit");
    }

    genuine30[5] ^= 0x20u;
    check(ns2_motion_hybrid_projector_project(
              &state, genuine30, sizeof(genuine30), us + 60000u,
              NS2_MOTION_HYBRID_PREFIX, out, &result),
          "stale donor orientation also remains owned on 0x1e");
    check(changed_outside(NS2_MOTION_HYBRID_PREFIX, genuine30, out,
                          sizeof(genuine30)) == 0u,
          "held 0x1e prefix preserves every non-prefix bit");
}

int main(void)
{
    test_applied_and_fail_closed();
    test_alignment_guards();
    test_prefix_owns_both_carrier_lengths();
    if (failures) {
        fprintf(stderr, "ns2_motion_hybrid_projector: %d failure(s)\n",
                failures);
        return 1;
    }
    puts("ns2_motion_hybrid_projector: all tests passed");
    return 0;
}
