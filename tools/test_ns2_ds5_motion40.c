// Offline validation of the DualSense -> length-0x28 catch-up translation.
//
// The packer itself is proven byte-exact against 981 genuine packets by
// test_ns2_motion_pdu40.c. This covers the layer above it: sample scaling,
// slot filling, cadence, saturation, and the modular prefix slice -- the parts
// that decide whether a well-formed packet also carries correct data.
//
// Build:
//   gcc -Iinclude -Itools/fixtures
//       -o build/host-tests/build-host-test-ns2-ds5-motion40
//       tools/test_ns2_ds5_motion40.c src/bt_hid/motion/ns2_ds5_motion40.c
//       src/bt_hid/motion/ns2_motion_pdu.c

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_ds5_motion40.h"
#include "ns2_motion40_catchup.h"

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

// The prefix slice is where a C reimplementation most easily diverges: right
// shift of a negative value is implementation-defined, and the reference takes
// a floor. These vectors are genuine carriers plus the field extremes.
static void test_prefix_matches_reference(void)
{
    unsigned matched = 0;
    for (unsigned i = 0; i < NS2_MOTION40_PREFIX_VECTOR_COUNT; ++i) {
        const ns2_motion40_prefix_vector_t *vector =
            &ns2_motion40_prefix_vectors[i];
        int32_t got[3];
        ns2_ds5_motion40_prefix(vector->carrier, got);
        if (got[0] != vector->prefix[0] || got[1] != vector->prefix[1] ||
            got[2] != vector->prefix[2]) {
            fprintf(stderr,
                    "FAIL: prefix vector %u: got {%d, %d, %d}, "
                    "reference {%d, %d, %d}\n",
                    i, got[0], got[1], got[2],
                    vector->prefix[0], vector->prefix[1], vector->prefix[2]);
            failures++;
            continue;
        }
        matched++;
    }
    check(matched == NS2_MOTION40_PREFIX_VECTOR_COUNT,
          "prefix slice matches the reference on every vector");
    printf("  prefix vectors matching reference: %u/%u\n",
           matched, NS2_MOTION40_PREFIX_VECTOR_COUNT);
}

static void feed(ns2_ds5_motion40_t *state, unsigned count,
                 int16_t ax, int16_t gy)
{
    for (unsigned i = 0; i < count; ++i) {
        const int16_t accel[3] = {ax, 0, 0};
        const int16_t gyro[3] = {gy, 0, 0};
        ns2_ds5_motion40_sample(state, accel, gyro);
    }
}

static void test_cadence_and_slot_filling(void)
{
    ns2_ds5_motion40_t state;
    const uint32_t carrier[3] = {33538654u, 25282388u, 814118u};
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);

    // First call only establishes the epoch.
    check(!ns2_ds5_motion40_build(&state, carrier, 1000u, pdu),
          "first build primes rather than emitting");

    // Interval elapsed but no samples: must refuse rather than invent data.
    check(!ns2_ds5_motion40_build(&state, carrier, 1000u + 20000u, pdu),
          "refuses to emit without samples");
    check(state.skipped_no_samples == 1u, "records the starved interval");

    // Partial fill is still not enough.
    feed(&state, 2u, 2000, 100);
    check(!ns2_ds5_motion40_build(&state, carrier, 1000u + 20000u, pdu),
          "refuses a partially filled packet");

    // Full slots, interval elapsed.
    feed(&state, 1u, 2000, 100);
    check(ns2_ds5_motion40_build(&state, carrier, 1000u + 20000u, pdu),
          "emits once slots are full and the interval elapsed");
    check(state.emitted == 1u, "counts the emission");
    check(pdu[3] == NS2_MOTION40_STATUS_CATCHUP, "status is catch-up");
    // 20000 us / 1250 us = 16 ticks.
    const uint16_t elapsed = (uint16_t)((pdu[1] >> 4) | ((uint16_t)pdu[2] << 4));
    check(elapsed == 16u, "elapsed count is the emit interval in ticks");

    // Slots reset after emission.
    check(state.accel_count == 0u && state.gyro_count == 0u,
          "slots clear after emission");

    // Too soon: the 15-tick minimum is 18750 us, so a 10 ms gap must not emit.
    feed(&state, 3u, 2000, 100);
    check(!ns2_ds5_motion40_build(&state, carrier, 1000u + 30000u, pdu),
          "does not emit before the minimum interval");
}

static void test_scaling(void)
{
    ns2_ds5_motion40_t state;
    ns2_ds5_motion40_reset(&state);
    // DualSense reports 8192 counts/g; the Pro 2 uses 4096, so 1 g in must
    // become 4096 ordinary counts. Slot 1 is half-resolution on the wire.
    const int16_t accel[3] = {8192, 0, 0};
    const int16_t gyro[3] = {164, 0, 0};  // ~10 dps at 16.4 counts/dps
    ns2_ds5_motion40_sample(&state, accel, gyro);
    ns2_ds5_motion40_sample(&state, accel, gyro);
    check(state.accel[0][0] == 4096, "slot 0 carries ordinary counts");
    check(state.accel[1][0] == 2048, "slot 1 is half-resolution");
    // Gyro sits at four times the ordinary scale.
    check(state.gyro[0][0] == 656, "gyro is stored at 4x ordinary scale");
    check(state.saturated_accel == 0u && state.saturated_gyro == 0u,
          "1 g and 10 dps do not saturate");
}

static void test_saturation_is_clamped_not_wrapped(void)
{
    ns2_ds5_motion40_t state;
    ns2_ds5_motion40_reset(&state);
    // The 16-bit gyro slot at 4x scale caps near +/-499 dps, well inside a
    // DualSense's range. Wrapping here would invert the rotation direction --
    // far worse than clipping it.
    const int16_t accel[3] = {0, 0, 0};
    const int16_t fast[3] = {20000, -20000, 0};
    ns2_ds5_motion40_sample(&state, accel, fast);
    check(state.gyro[0][0] == 32767, "positive gyro clamps to the slot limit");
    check(state.gyro[0][1] == -32768, "negative gyro clamps to the slot limit");
    check(state.saturated_gyro == 2u, "saturation is counted");

    ns2_ds5_motion40_reset(&state);
    const int16_t hard[3] = {32767, -32768, 0};
    const int16_t still[3] = {0, 0, 0};
    ns2_ds5_motion40_sample(&state, hard, still);
    check(state.accel[0][0] == 8191, "positive accel clamps to the 14-bit slot");
    check(state.accel[0][1] == -8192, "negative accel clamps to the 14-bit slot");
    check(state.saturated_accel == 2u, "accel saturation is counted");
}

static void test_emitted_packet_decodes_to_what_we_fed(void)
{
    ns2_ds5_motion40_t state;
    const uint32_t carrier[3] = {33538654u, 25282388u, 814118u};
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, carrier, 0u, pdu), "prime");

    const int16_t accel[3] = {8192, -4096, 2048};
    const int16_t gyro[3] = {164, -328, 82};
    for (unsigned i = 0; i < 3u; ++i)
        ns2_ds5_motion40_sample(&state, accel, gyro);
    check(ns2_ds5_motion40_build(&state, carrier, 20000u, pdu), "emits");

    // Payload bit 68, width 14, three axes: acceleration slot 0.
    const uint8_t *payload = &pdu[4];
    int32_t axis0 = 0;
    for (unsigned bit = 0; bit < 14u; ++bit) {
        if (payload[(68u + bit) >> 3] & (1u << ((68u + bit) & 7u)))
            axis0 |= (int32_t)1 << bit;
    }
    if (axis0 >= (1 << 13)) axis0 -= (1 << 14);
    check(axis0 == 4096, "acceleration slot 0 axis 0 survives to the wire");

    int32_t prefix_expected[3];
    ns2_ds5_motion40_prefix(carrier, prefix_expected);
    int32_t lane0 = 0;
    for (unsigned bit = 0; bit < 22u; ++bit) {
        if (payload[(2u + bit) >> 3] & (1u << ((2u + bit) & 7u)))
            lane0 |= (int32_t)1 << bit;
    }
    if (lane0 >= (1 << 21)) lane0 -= (1 << 22);
    check(lane0 == prefix_expected[0], "carrier prefix lane 0 survives to the wire");
    check((payload[0] & 0x03u) == 3u, "packing mode 3");
}

int main(void)
{
    test_prefix_matches_reference();
    test_cadence_and_slot_filling();
    test_scaling();
    test_saturation_is_clamped_not_wrapped();
    test_emitted_packet_decodes_to_what_we_fed();
    if (failures) {
        fprintf(stderr, "ns2_ds5_motion40: %d failure(s)\n", failures);
        return 1;
    }
    printf("ns2_ds5_motion40: all tests passed\n");
    return 0;
}
