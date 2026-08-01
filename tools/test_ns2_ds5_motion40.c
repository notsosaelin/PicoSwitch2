// Offline validation of the DualSense -> length-0x28 catch-up translation.
//
// The packer itself is proven byte-exact against 981 genuine packets by
// test_ns2_motion_pdu40.c. This covers the layer above it: sample scaling,
// slot placement across the emit window, cadence, saturation, and the modular
// prefix slice -- the parts that decide whether a well-formed packet also
// carries correct data.
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

static const uint32_t k_carrier[3] = {33538654u, 25282388u, 814118u};

// One sample whose x axis encodes its own timestamp, so a test can read a slot
// back and say exactly which sample landed in it.
static void tag(ns2_ds5_motion40_t *state, int16_t id, uint32_t us)
{
    const int16_t accel[3] = {(int16_t)(id * 2), 0, 0};
    const int16_t gyro[3] = {id, 0, 0};
    ns2_ds5_motion40_sample(state, accel, gyro, us);
}

static void test_cadence(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);

    check(!ns2_ds5_motion40_build(&state, k_carrier, 1000u, pdu),
          "first build primes rather than emitting");

    // Interval elapsed but no samples: must refuse rather than invent data.
    check(!ns2_ds5_motion40_build(&state, k_carrier, 1000u + 20000u, pdu),
          "refuses to emit without samples");
    check(state.skipped_no_samples == 1u, "records the starved interval");

    // Two samples cannot fill three slots without repeating one.
    tag(&state, 1, 1000u + 5000u);
    tag(&state, 2, 1000u + 15000u);
    check(!ns2_ds5_motion40_build(&state, k_carrier, 1000u + 20000u, pdu),
          "refuses rather than repeating a sample into two slots");
    check(state.skipped_no_samples == 2u, "counts the second starved interval");

    // A third sample completes the window.
    tag(&state, 3, 1000u + 20000u);
    check(ns2_ds5_motion40_build(&state, k_carrier, 1000u + 20000u, pdu),
          "emits once the window holds enough distinct samples");
    check(state.emitted == 1u, "counts the emission");
    check(pdu[3] == NS2_MOTION40_STATUS_CATCHUP, "status is catch-up");
    const uint16_t elapsed = (uint16_t)((pdu[1] >> 4) | ((uint16_t)pdu[2] << 4));
    check(elapsed == 16u, "elapsed is the span the samples cover, in ticks");

    // The 15-tick minimum is 18750 us, so a 10 ms gap must not emit.
    tag(&state, 4, 1000u + 25000u);
    tag(&state, 5, 1000u + 28000u);
    tag(&state, 6, 1000u + 30000u);
    check(!ns2_ds5_motion40_build(&state, k_carrier, 1000u + 30000u, pdu),
          "does not emit before the minimum interval");
}

// The elapsed count must describe the span the samples actually cover. If it
// were taken from the poll time instead, a late poll would inflate it and the
// error would accumulate against the console's clock.
static void test_elapsed_tracks_samples_not_poll_time(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, k_carrier, 0u, pdu), "prime");

    // Samples stop at 20 ms but the poll lands 9 ms later.
    tag(&state, 1, 4000u);
    tag(&state, 2, 12000u);
    tag(&state, 3, 20000u);
    check(ns2_ds5_motion40_build(&state, k_carrier, 29000u, pdu), "emits");
    const uint16_t elapsed = (uint16_t)((pdu[1] >> 4) | ((uint16_t)pdu[2] << 4));
    check(elapsed == 16u, "elapsed is 16 ticks (20 ms of samples), not 23");

    // The next window must start where the reported elapsed ended, so a sample
    // at 21 ms still belongs to it rather than being swallowed by the gap.
    tag(&state, 4, 21000u);
    tag(&state, 5, 30000u);
    tag(&state, 6, 40000u);
    check(ns2_ds5_motion40_build(&state, k_carrier, 40000u, pdu),
          "second packet emits");
    const uint16_t elapsed2 = (uint16_t)((pdu[1] >> 4) | ((uint16_t)pdu[2] << 4));
    check(elapsed2 == 16u, "second elapsed spans 20 ms to 40 ms");
    check(state.accel[0][0] == 4, "the 21 ms sample opens the second window");
}

// The defect this test exists for: filling slots from the first three samples
// and dropping the rest makes a packet cover only the head of its window and
// discard the freshest data. Genuine packets span the window, oldest slot to
// newest.
static void test_slots_span_the_window(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, k_carrier, 0u, pdu), "prime");

    // Five samples evenly spread across a 20 ms window, ids 1..5.
    for (int16_t i = 0; i < 5; ++i) tag(&state, (int16_t)(i + 1), (uint32_t)(i + 1) * 4000u);
    check(ns2_ds5_motion40_build(&state, k_carrier, 20000u, pdu), "emits");

    // accel x is id*2, halved to ordinary counts, so slot value == id.
    check(state.accel[0][0] == 1, "accel slot 0 is the OLDEST sample (id 1)");
    check(state.accel[1][0] == 3 / 2 || state.accel[1][0] == 2,
          "accel slot 1 is the midpoint sample (id 3, half-resolution)");
    check(state.accel[2][0] == 5, "accel slot 2 is the NEWEST sample (id 5)");

    // Gyro sits at the quarter points: 5 ms and 15 ms of a 20 ms window are
    // nearest to ids 1/2 and 4.
    check(state.gyro[0][0] == 2 * 4 || state.gyro[0][0] == 1 * 4,
          "gyro slot 0 comes from the first half of the window");
    check(state.gyro[1][0] == 4 * 4, "gyro slot 1 comes from the second half");
    check(state.gyro[0][0] < state.gyro[1][0],
          "gyro slots are in chronological order");

    // A tenth sample must not push the oldest out of slot 0 the way a
    // first-three-wins policy would.
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, k_carrier, 0u, pdu), "prime");
    for (int16_t i = 0; i < 10; ++i)
        tag(&state, (int16_t)(i + 1), (uint32_t)(i + 1) * 2000u);
    check(ns2_ds5_motion40_build(&state, k_carrier, 20000u, pdu), "emits");
    check(state.accel[0][0] == 1, "ten samples: slot 0 is still the oldest");
    check(state.accel[2][0] == 10, "ten samples: slot 2 is the newest");
}

// Samples are pushed in timestamp order in practice, but the ring wraps, so
// selection must not assume the array is chronological.
static void test_ring_wrap_preserves_order(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, k_carrier, 0u, pdu), "prime");

    // Push more than the ring holds so head wraps mid-window.
    for (int16_t i = 0; i < (int16_t)NS2_DS5_MOTION40_RING + 3; ++i)
        tag(&state, (int16_t)(i + 1), (uint32_t)(i + 1) * 1000u);
    const int16_t newest = (int16_t)NS2_DS5_MOTION40_RING + 3;
    check(ns2_ds5_motion40_build(&state, k_carrier,
                                 (uint32_t)newest * 1000u, pdu),
          "emits after the ring wrapped");
    check(state.accel[2][0] == newest,
          "newest sample still lands in the last slot after a wrap");
    check(state.accel[0][0] < state.accel[2][0],
          "slot 0 is still older than slot 2 after a wrap");
}

static void test_scaling(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, k_carrier, 0u, pdu), "prime");

    // DualSense reports 8192 counts/g; the Pro 2 uses 4096, so 1 g in must
    // become 4096 ordinary counts. Slot 1 is half-resolution on the wire.
    for (unsigned i = 0; i < 3u; ++i) {
        const int16_t accel[3] = {8192, 0, 0};
        const int16_t gyro[3] = {164, 0, 0};  // ~10 dps at 16.4 counts/dps
        ns2_ds5_motion40_sample(&state, accel, gyro, (i + 1u) * 7000u);
    }
    check(ns2_ds5_motion40_build(&state, k_carrier, 21000u, pdu), "emits");
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
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];

    // The 16-bit gyro slot at 4x scale caps near +/-499 dps, well inside a
    // DualSense's range. Wrapping here would invert the rotation direction --
    // far worse than clipping it.
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, k_carrier, 0u, pdu), "prime");
    for (unsigned i = 0; i < 3u; ++i) {
        const int16_t accel[3] = {0, 0, 0};
        const int16_t fast[3] = {20000, -20000, 0};
        ns2_ds5_motion40_sample(&state, accel, fast, (i + 1u) * 7000u);
    }
    check(ns2_ds5_motion40_build(&state, k_carrier, 21000u, pdu), "emits");
    check(state.gyro[0][0] == 32767, "positive gyro clamps to the slot limit");
    check(state.gyro[0][1] == -32768, "negative gyro clamps to the slot limit");
    check(state.saturated_gyro == 4u, "saturation is counted on both slots");

    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, k_carrier, 0u, pdu), "prime");
    for (unsigned i = 0; i < 3u; ++i) {
        const int16_t hard[3] = {32767, -32768, 0};
        const int16_t still[3] = {0, 0, 0};
        ns2_ds5_motion40_sample(&state, hard, still, (i + 1u) * 7000u);
    }
    check(ns2_ds5_motion40_build(&state, k_carrier, 21000u, pdu), "emits");
    check(state.accel[0][0] == 8191, "positive accel clamps to the 14-bit slot");
    check(state.accel[0][1] == -8192, "negative accel clamps to the 14-bit slot");
    check(state.saturated_accel > 0u, "accel saturation is counted");
}

static void test_emitted_packet_decodes_to_what_we_fed(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, k_carrier, 0u, pdu), "prime");

    for (unsigned i = 0; i < 3u; ++i) {
        const int16_t accel[3] = {8192, -4096, 2048};
        const int16_t gyro[3] = {164, -328, 82};
        ns2_ds5_motion40_sample(&state, accel, gyro, (i + 1u) * 7000u);
    }
    check(ns2_ds5_motion40_build(&state, k_carrier, 21000u, pdu), "emits");

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
    ns2_ds5_motion40_prefix(k_carrier, prefix_expected);
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
    test_cadence();
    test_elapsed_tracks_samples_not_poll_time();
    test_slots_span_the_window();
    test_ring_wrap_preserves_order();
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
