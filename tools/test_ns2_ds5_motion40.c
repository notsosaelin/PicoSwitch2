// Offline validation of the DualSense -> length-0x28 HIGH-RATE translation.
//
// The packer itself is proven byte-exact against 858 genuine high-rate packets
// by test_ns2_motion_pdu40.c. This covers the layer above it: sample scaling,
// slot placement across the emit window, the cadence band, saturation, and the
// prefix epoch -- the parts that decide whether a well-formed packet also
// carries correct data. Every hardware defect this feature has had lived here,
// not in the bit layout.
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

static const uint32_t k_carrier[3] = {33538654u, 25282388u, 814118u};

#define TICK_US 1250u
#define ACCEL0_OFFSET 74u
#define GYRO0_OFFSET 140u
#define ACCEL1_OFFSET 206u
#define VECTOR_WIDTH 22u
#define TAIL_OFFSET 272u

// The prefix slice is where a C reimplementation most easily diverges: right
// shift of a negative value is implementation-defined, the reference takes a
// floor, and high-rate lanes are two bits wider with no precision shift. These
// vectors are genuine carriers plus the field extremes, sliced narrow.
static void test_prefix_matches_reference(void)
{
    unsigned matched = 0;
    for (unsigned i = 0; i < NS2_MOTION40_PREFIX_VECTOR_COUNT; ++i) {
        const ns2_motion40_prefix_vector_t *vector =
            &ns2_motion40_prefix_vectors[i];
        int32_t got[3];
        ns2_ds5_motion40_prefix(vector->carrier, got, false);
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
          "narrow prefix slice matches the reference on every vector");
    printf("  prefix vectors matching reference: %u/%u\n",
           matched, NS2_MOTION40_PREFIX_VECTOR_COUNT);
}

// High-rate lanes are wider and unshifted, so they must differ from the narrow
// slice of the same carrier. Sharing one width table would be silently wrong:
// the packet would be well formed and carry a quartered orientation.
static void test_high_rate_prefix_differs_from_narrow(void)
{
    int32_t narrow[3], wide[3];
    ns2_ds5_motion40_prefix(k_carrier, narrow, false);
    ns2_ds5_motion40_prefix(k_carrier, wide, true);
    check(wide[0] != narrow[0] || wide[1] != narrow[1] || wide[2] != narrow[2],
          "high-rate prefix is not the narrow slice");
    // No precision shift means the wide lane keeps the two low bits the narrow
    // one discards, so it is four times larger for the same carrier.
    check(wide[0] / 4 == narrow[0] || wide[0] / 4 == narrow[0] + 1,
          "high-rate lane 0 keeps the two bits the narrow slice shifts away");
}

// One sample whose x axis encodes its own id, so a test can read a slot back
// and say exactly which sample landed in it.
static void tag(ns2_ds5_motion40_t *state, int16_t id, uint32_t us)
{
    const int16_t accel[3] = {id, 0, 0};
    const int16_t gyro[3] = {id, 0, 0};
    const uint16_t timing =
        (uint16_t)((us / NS2_DS5_MOTION40_TICK_US) & 0x0FFFu);
    ns2_ds5_motion40_sample(state, accel, gyro, k_carrier, timing, us);
}

static uint16_t wire_elapsed(const uint8_t pdu[NS2_MOTION_PDU40_LENGTH])
{
    return (uint16_t)((pdu[1] >> 4) | ((uint16_t)pdu[2] << 4));
}

static uint16_t wire_tick(const uint8_t *pdu)
{
    return (uint16_t)(pdu[0] | ((uint16_t)(pdu[1] & 0x0Fu) << 8));
}

static void carrier_for_tick(uint8_t pdu[NS2_MOTION_PDU30_LENGTH],
                             uint16_t tick)
{
    memset(pdu, 0, NS2_MOTION_PDU30_LENGTH);
    pdu[0] = (uint8_t)tick;
    pdu[1] = (uint8_t)((tick >> 8) & 0x0Fu);
    check(ns2_motion_pdu30_set_orientation(pdu, k_carrier),
          "carrier fixture accepts its orientation");
}

// Read a signed field straight off the payload, so the test sees what the
// console would rather than trusting the module's own bookkeeping.
static int32_t wire_field(const uint8_t pdu[NS2_MOTION_PDU40_LENGTH],
                          unsigned offset, unsigned width)
{
    const uint8_t *payload = &pdu[4];
    int32_t value = 0;
    for (unsigned bit = 0; bit < width; ++bit) {
        if (payload[(offset + bit) >> 3] & (1u << ((offset + bit) & 7u)))
            value |= (int32_t)1 << bit;
    }
    const int32_t half = (int32_t)1 << (width - 1u);
    return (value >= half) ? value - 2 * half : value;
}

static int32_t coherent_accel_wire(int16_t raw)
{
    const int64_t product =
        (int64_t)raw * NS2_MOTION30_ACCEL_Q16_PER_COUNT;
    const int64_t half = 1ll << 7;
    return (int32_t)((product >= 0 ? product + half : product - half) /
                     (1ll << 8));
}

static void test_cadence_band(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);

    check(!ns2_ds5_motion40_build(&state, 0u, pdu),
          "first build primes rather than emitting");
    check(!ns2_ds5_motion40_build(&state, 10000u, pdu),
          "refuses to emit without samples");
    check(state.skipped_no_samples == 1u, "records the starved interval");

    // One sample cannot fill two acceleration slots without repeating it.
    tag(&state, 1, 4000u);
    check(!ns2_ds5_motion40_build(&state, 10000u, pdu),
          "refuses rather than repeating a sample into two slots");

    // 8 ticks = 10000 us, inside the high-rate band.
    tag(&state, 2, 7000u);
    tag(&state, 3, 10000u);
    check(ns2_ds5_motion40_build(&state, 10000u, pdu), "emits inside the band");
    check(state.emitted == 1u, "counts the emission");
    check(pdu[3] == NS2_MOTION40_STATUS_HIGH_RATE, "status is high-rate");
    check(wire_elapsed(pdu) == 8u, "elapsed is the span the samples cover");
    check(wire_elapsed(pdu) <= NS2_MOTION40_HIGH_RATE_MAX_ELAPSED,
          "elapsed stays inside the band that selects this layout");

    // 7 ticks is 8750 us, so a 5 ms gap is too soon.
    tag(&state, 4, 12000u);
    tag(&state, 5, 15000u);
    check(!ns2_ds5_motion40_build(&state, 15000u, pdu),
          "does not emit before the minimum interval");
}

// Elapsed selects the layout. A window longer than the high-rate band cannot
// be sent as high-rate at all: the console would read the fields with the
// normal or catch-up map. Clamping the count would lie about the span.
static void test_overlong_window_is_dropped_not_clamped(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, 0u, pdu), "prime");

    // 20 ms = 16 ticks, well past the 10-tick ceiling.
    tag(&state, 1, 5000u);
    tag(&state, 2, 12000u);
    tag(&state, 3, 20000u);
    check(!ns2_ds5_motion40_build(&state, 20000u, pdu),
          "refuses a window that would decode as another layout");
    check(state.skipped_overlong == 1u, "counts the overlong window");
    check(state.emitted == 0u, "nothing was emitted");

    // Re-anchored, so an ordinary window immediately after still works.
    tag(&state, 4, 24000u);
    tag(&state, 5, 27000u);
    tag(&state, 6, 30000u);
    check(ns2_ds5_motion40_build(&state, 30000u, pdu),
          "recovers after dropping");
    check(wire_elapsed(pdu) == 8u, "the recovered packet spans 10 ms");
}

// Slot 0 is the oldest sample in the window and slot 1 the newest. Filling
// from the first samples to arrive would cover only the head of the window and
// discard the freshest data.
static void test_slots_span_the_window(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, 0u, pdu), "prime");

    // Five samples across a 10 ms window, ids 1..5.
    for (int16_t i = 1; i <= 5; ++i) tag(&state, i, (uint32_t)i * 2000u);
    check(ns2_ds5_motion40_build(&state, 10000u, pdu), "emits");

    // Acceleration uses the same output calibration as the 0x1E carrier.
    check(wire_field(pdu, ACCEL0_OFFSET, VECTOR_WIDTH) ==
              coherent_accel_wire(1),
          "accel slot 0 is the OLDEST sample");
    check(wire_field(pdu, ACCEL1_OFFSET, VECTOR_WIDTH) ==
              coherent_accel_wire(5),
          "accel slot 1 is the NEWEST sample");
    // gyro x is id raw; wire = raw * 128. Every sample spans two of the ten
    // ticks, so the carrier-coherent interval mean is (1+2+3+4+5)/5 = 3.
    const int32_t gyro = wire_field(pdu, GYRO0_OFFSET, VECTOR_WIDTH);
    check(gyro == 3 * 128,
          "the gyro slot is the tick-weighted mean over the window");
}

// One flashed image must be able to reproduce the former 0.5 g defect and
// return to the carrier-coherent path without changing any other generator code.
// This makes the hardware A/B causal instead of comparing separate builds.
static void test_accel_diagnostic_modes(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];

    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, 0u, pdu), "prime live accel");
    tag(&state, 4096, 5000u);
    tag(&state, 4096, 10000u);
    check(ns2_ds5_motion40_build(&state, 10000u, pdu), "live accel emits");
    check(wire_field(pdu, ACCEL0_OFFSET, VECTOR_WIDTH) ==
              coherent_accel_wire(4096),
          "live accel matches the validated 0x1E output calibration");

    ns2_ds5_motion40_reset(&state);
    state.accel_mode = NS2_DS5_MOTION40_ACCEL_HALF;
    check(!ns2_ds5_motion40_build(&state, 0u, pdu), "prime half accel");
    tag(&state, 4096, 5000u);
    tag(&state, 4096, 10000u);
    check(ns2_ds5_motion40_build(&state, 10000u, pdu), "half accel emits");
    check(wire_field(pdu, ACCEL0_OFFSET, VECTOR_WIDTH) == 4096 * 128,
          "half accel recreates the former 0.5 g wire value");

    ns2_ds5_motion40_reset(&state);
    state.accel_mode = NS2_DS5_MOTION40_ACCEL_ZERO;
    check(!ns2_ds5_motion40_build(&state, 0u, pdu), "prime zero accel");
    tag(&state, 4096, 5000u);
    tag(&state, 4096, 10000u);
    check(ns2_ds5_motion40_build(&state, 10000u, pdu), "zero accel emits");
    check(wire_field(pdu, ACCEL0_OFFSET, VECTOR_WIDTH) == 0,
          "zero accel clears the generated lane");
}

// Delayed source samples cover more carrier integration time than ordinary
// samples. The one high-rate gyro vector must preserve that area rather than
// averaging records as though they were equally spaced.
static void test_gyro_mean_is_weighted_by_shared_ticks(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, 0u, pdu), "prime");

    const int16_t accel[3] = {0, 0, 0};
    const int16_t gyro_a[3] = {100, 0, 0};
    const int16_t gyro_b[3] = {-20, 0, 0};
    // First sample covers 2 ticks; the delayed second covers 6. The interval
    // mean is therefore (100*2 + -20*6) / 8 = 10 counts, not the unweighted
    // record mean of 40 counts.
    ns2_ds5_motion40_sample(&state, accel, gyro_a, k_carrier, 2u, 2500u);
    ns2_ds5_motion40_sample(&state, accel, gyro_b, k_carrier, 8u, 10000u);
    check(ns2_ds5_motion40_build(&state, 10000u, pdu),
          "delayed-sample window emits");
    check(wire_elapsed(pdu) == 8u, "delayed-sample elapsed is preserved");
    check(wire_field(pdu, GYRO0_OFFSET, VECTOR_WIDTH) == 10 * 128,
          "gyro preserves the carrier-integrated tick area");
}

// A DualSense supplying a valid sensor timestamp feeds this at its own IMU
// rate -- measured at ~763 Hz on hardware, not the ~250 Hz a report-rate
// assumption suggests. The ring must outlast a window at that rate or it
// evicts the oldest samples before the build can select them.
static void test_ring_outlasts_a_window_at_source_rate(void)
{
    check(NS2_DS5_MOTION40_RING > NS2_DS5_MOTION40_MAX_TICKS,
          "ring holds more than one full-length window at 800 Hz");

    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, 0u, pdu), "prime");

    for (unsigned window = 1; window <= 4u; ++window) {
        const unsigned first = (window - 1u) * 8u + 1u;
        const unsigned last = window * 8u;
        for (unsigned i = first; i <= last; ++i)
            tag(&state, (int16_t)i, i * TICK_US);
        check(ns2_ds5_motion40_build(&state, last * TICK_US, pdu),
              "consecutive window emits");
        check(wire_field(pdu, ACCEL0_OFFSET, VECTOR_WIDTH) ==
                  coherent_accel_wire((int16_t)first),
              "each window keeps its own oldest sample");
        check(wire_field(pdu, ACCEL1_OFFSET, VECTOR_WIDTH) ==
                  coherent_accel_wire((int16_t)last),
              "each window keeps its own newest sample");
    }
}

// The prefix must describe a PAST instant -- a fixed lag after the window
// START -- not the packet's own tick. Sending the current orientation while
// also sending the window's IMU samples double-counts the window's rotation.
// Every other test uses one constant carrier and cannot tell the two apart.
static void test_prefix_comes_from_the_window_start_not_now(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, 0u, pdu), "prime");

    uint32_t carriers[6][3];
    for (unsigned id = 1; id <= 5u; ++id) {
        carriers[id][0] = k_carrier[0] + id * 100000u;
        carriers[id][1] = k_carrier[1] + id * 100000u;
        carriers[id][2] = k_carrier[2] + id * 100000u;
        const int16_t accel[3] = {(int16_t)(id * 2), 0, 0};
        const int16_t gyro[3] = {(int16_t)id, 0, 0};
        // 1000, 3000, 5000, 7000, 9000 us -- deliberately placing one sample
        // exactly on the epoch, so the assertion does not depend on how ties
        // are broken. Genuine hardware specifies no tie-break.
        const uint32_t us = id * 2000u - 1000u;
        const uint16_t timing =
            (uint16_t)((us / NS2_DS5_MOTION40_TICK_US) & 0x0FFFu);
        ns2_ds5_motion40_sample(&state, accel, gyro, carriers[id], timing, us);
    }
    check(ns2_ds5_motion40_build(&state, 9000u, pdu), "emits");

    // The window starts at 0, so the epoch is 4 ticks = 5000 us: exactly id 3,
    // not id 5 at 9000 us.
    int32_t expected[3], newest[3];
    ns2_ds5_motion40_prefix(carriers[3], expected, true);
    ns2_ds5_motion40_prefix(carriers[5], newest, true);
    const int32_t got = wire_field(pdu, 2u, 24u);
    check(expected[0] != newest[0], "the test can distinguish the two");
    check(got == expected[0],
          "prefix comes from the sample nearest the window start + 4 ticks");
    check(got != newest[0], "prefix is NOT the current orientation");
}

static void test_scaling(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, 0u, pdu), "prime");

    // The translator receives post-seam acceleration at the Pro 2's
    // 4096-count/g scale. The wire adds eight fractional bits and preserves
    // the validated 0x1E output calibration without a second normalization.
    for (unsigned i = 1; i <= 3u; ++i) {
        const int16_t accel[3] = {4096, 0, 0};
        const int16_t gyro[3] = {164, 0, 0};  // ~10 dps at 16.4 counts/dps
        const uint32_t us = i * 3000u;
        const uint16_t timing =
            (uint16_t)((us / NS2_DS5_MOTION40_TICK_US) & 0x0FFFu);
        ns2_ds5_motion40_sample(&state, accel, gyro, k_carrier, timing, us);
    }
    check(ns2_ds5_motion40_build(&state, 9000u, pdu), "emits");
    check(wire_field(pdu, ACCEL0_OFFSET, VECTOR_WIDTH) ==
              coherent_accel_wire(4096),
          "0x28 acceleration matches the 0x1E calibrated vector");
    check(wire_field(pdu, GYRO0_OFFSET, VECTOR_WIDTH) == 164 * 128,
          "gyro passes through at ordinary scale, then seven fractional bits");
    check(state.saturated_accel == 0u && state.saturated_gyro == 0u,
          "1 g and 10 dps do not saturate");
    check((uint16_t)wire_field(pdu, TAIL_OFFSET, 16u) ==
              (uint16_t)NS2_DS5_MOTION40_TEMPERATURE_TAIL,
          "the temperature tail carries the modal genuine value");
}

static void test_saturation_is_clamped_not_wrapped(void)
{
    ns2_ds5_motion40_t state;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];

    // The 22-bit slots cap at +/-2 g and about +/-999 dps. Wrapping would
    // invert the reported direction of motion.
    ns2_ds5_motion40_reset(&state);
    check(!ns2_ds5_motion40_build(&state, 0u, pdu), "prime");
    for (unsigned i = 1; i <= 3u; ++i) {
        const int16_t accel[3] = {32767, -32768, 0};
        const int16_t fast[3] = {20000, -20000, 0};
        const uint32_t us = i * 3000u;
        const uint16_t timing =
            (uint16_t)((us / NS2_DS5_MOTION40_TICK_US) & 0x0FFFu);
        ns2_ds5_motion40_sample(&state, accel, fast, k_carrier, timing, us);
    }
    check(ns2_ds5_motion40_build(&state, 9000u, pdu), "emits");
    check(wire_field(pdu, ACCEL0_OFFSET, VECTOR_WIDTH) == 2097151,
          "positive acceleration clamps to the slot limit");
    check(wire_field(pdu, ACCEL0_OFFSET + VECTOR_WIDTH, VECTOR_WIDTH) ==
              -2097152,
          "negative acceleration clamps to the slot limit");
    check(state.saturated_accel > 0u, "acceleration saturation is counted");
    check(state.saturated_gyro > 0u, "gyro saturation is counted");
}

// Interleaved 0x1E and 0x28 are one native-rate PDU stream, not two encoders
// whose clocks happen to run near each other. The console-visible output is
// held between frame boundaries, every timing field refers to the immediately
// preceding selected PDU, and a 0x28 uses the established 0x1E tick epoch.
static void test_interleaved_scheduler_shares_one_tick_timeline(void)
{
    ns2_ds5_motion40_t state;
    uint8_t carrier[NS2_MOTION_PDU30_LENGTH];
    uint8_t out[NS2_MOTION_PDU40_LENGTH];
    uint8_t length = 0u;
    ns2_ds5_motion40_reset(&state);

    for (uint16_t tick = 1u; tick <= 20u; ++tick) {
        tag(&state, (int16_t)tick, tick * TICK_US);
        carrier_for_tick(carrier, tick);
        const bool fresh =
            ns2_ds5_motion40_select(&state, carrier, out, &length);

        if (tick == 1u) {
            check(fresh && length == NS2_MOTION_PDU30_LENGTH,
                  "first frame seeds with a carrier");
            check(wire_tick(out) == 1u, "seed carrier uses source tick");
        } else if (tick == 7u || tick == 13u) {
            check(fresh && length == NS2_MOTION_PDU30_LENGTH,
                  "native-rate carrier frame selected");
            check(wire_tick(out) == tick, "carrier stays on source tick");
            check((out[1] >> 4) == 6u,
                  "carrier elapsed is delta from preceding selected PDU");
        } else if (tick == 20u) {
            check(fresh && length == NS2_MOTION_PDU40_LENGTH,
                  "fourth frame selects high-rate 0x28");
            check(wire_tick(out) == 20u,
                  "0x28 continues the carrier's source tick epoch");
            check(wire_elapsed(out) == 7u,
                  "0x28 elapsed reaches the immediately preceding carrier");
        } else {
            check(!fresh, "poll/sample between native frames holds output");
        }
    }

    check(state.carrier_frames == 3u,
          "three carrier frames precede the high-rate frame");
    check(state.emitted == 1u, "one high-rate frame emitted");
    check(state.skipped_no_samples == 0u && state.skipped_overlong == 0u,
          "coherent schedule neither starves nor overruns");

    uint8_t held[NS2_MOTION_PDU40_LENGTH];
    memcpy(held, out, length);
    carrier_for_tick(carrier, 20u);
    check(!ns2_ds5_motion40_select(&state, carrier, out, &length),
          "same source tick is held on another USB poll");
    check(length == NS2_MOTION_PDU40_LENGTH &&
              memcmp(out, held, length) == 0,
          "held 0x28 is byte-identical until the next native frame");
}

int main(void)
{
    test_prefix_matches_reference();
    test_high_rate_prefix_differs_from_narrow();
    test_cadence_band();
    test_overlong_window_is_dropped_not_clamped();
    test_slots_span_the_window();
    test_accel_diagnostic_modes();
    test_gyro_mean_is_weighted_by_shared_ticks();
    test_ring_outlasts_a_window_at_source_rate();
    test_prefix_comes_from_the_window_start_not_now();
    test_scaling();
    test_saturation_is_clamped_not_wrapped();
    test_interleaved_scheduler_shares_one_tick_timeline();
    if (failures) {
        fprintf(stderr, "ns2_ds5_motion40: %d failure(s)\n", failures);
        return 1;
    }
    printf("ns2_ds5_motion40: all tests passed\n");
    return 0;
}
