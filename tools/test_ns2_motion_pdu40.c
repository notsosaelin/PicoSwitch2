// Hold the firmware length-0x28 catch-up packer to the same bar as the Python
// reference: byte-exactness against genuine hardware captures.
//
// The fixture carries the fields tools/ns2_motion_reference.py decoded from
// each genuine PDU alongside the PDU's own 40 bytes. This test rebuilds the
// packet from those fields and compares. A round trip against our own encoder
// would prove only self-consistency; comparing to hardware through an
// independent implementation is what makes agreement mean something.
//
// If this fails, the firmware packer and the validated Python map disagree,
// and nothing built on it should reach a console.
//
// Build:
//   gcc -Iinclude -Itools/fixtures
//       -o build/host-tests/build-host-test-ns2-motion-pdu40
//       tools/test_ns2_motion_pdu40.c src/bt_hid/motion/ns2_motion_pdu.c

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_motion_pdu.h"
#include "ns2_motion40_catchup.h"
#include "ns2_motion40_high_rate.h"

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void load(ns2_motion40_catchup_t *out,
                 const ns2_motion40_catchup_fixture_t *fixture)
{
    memset(out, 0, sizeof(*out));
    out->tick = fixture->tick;
    out->elapsed_ticks = fixture->elapsed_ticks;
    out->status = fixture->status;
    out->packing_mode = fixture->packing_mode;
    out->tail_bit = fixture->tail_bit;
    memcpy(out->carrier, fixture->carrier, sizeof(out->carrier));
    memcpy(out->accel, fixture->accel, sizeof(out->accel));
    memcpy(out->gyro, fixture->gyro, sizeof(out->gyro));
}

static void test_corpus_is_byte_exact(void)
{
    unsigned rebuilt = 0;
    unsigned mismatched = 0;
    for (unsigned i = 0; i < NS2_MOTION40_CATCHUP_FIXTURE_COUNT; ++i) {
        const ns2_motion40_catchup_fixture_t *fixture =
            &ns2_motion40_catchup_fixtures[i];
        ns2_motion40_catchup_t fields;
        uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
        load(&fields, fixture);
        memset(pdu, 0xAA, sizeof(pdu));
        if (!ns2_motion_pdu40_build_catchup(pdu, &fields)) {
            fprintf(stderr, "FAIL: packet %u refused a genuine field set\n", i);
            failures++;
            continue;
        }
        if (memcmp(pdu, fixture->bytes, sizeof(pdu)) != 0) {
            if (mismatched < 3u) {
                fprintf(stderr, "FAIL: packet %u differs\n  genuine:", i);
                for (unsigned b = 0; b < sizeof(pdu); ++b)
                    fprintf(stderr, " %02X", fixture->bytes[b]);
                fprintf(stderr, "\n  rebuilt:");
                for (unsigned b = 0; b < sizeof(pdu); ++b)
                    fprintf(stderr, " %02X", pdu[b]);
                fprintf(stderr, "\n  first differing byte:");
                for (unsigned b = 0; b < sizeof(pdu); ++b) {
                    if (pdu[b] != fixture->bytes[b]) {
                        fprintf(stderr, " %u\n", b);
                        break;
                    }
                }
            }
            mismatched++;
            failures++;
            continue;
        }
        rebuilt++;
    }
    check(rebuilt == NS2_MOTION40_CATCHUP_FIXTURE_COUNT,
          "every genuine catch-up packet rebuilds byte-for-byte");
    printf("  rebuilt byte-exactly: %u/%u genuine catch-up packets\n",
           rebuilt, NS2_MOTION40_CATCHUP_FIXTURE_COUNT);
}

static void test_edge_values_agree_with_the_reference_encoder(void)
{
    unsigned rebuilt = 0;
    for (unsigned i = 0; i < NS2_MOTION40_CATCHUP_EDGE_COUNT; ++i) {
        const ns2_motion40_catchup_fixture_t *fixture =
            &ns2_motion40_catchup_edges[i];
        ns2_motion40_catchup_t fields;
        uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
        load(&fields, fixture);
        if (!ns2_motion_pdu40_build_catchup(pdu, &fields)) {
            fprintf(stderr, "FAIL: edge case %u refused a valid field set\n", i);
            failures++;
            continue;
        }
        if (memcmp(pdu, fixture->bytes, sizeof(pdu)) != 0) {
            fprintf(stderr, "FAIL: edge case %u differs from the reference\n"
                            "  reference:", i);
            for (unsigned b = 0; b < sizeof(pdu); ++b)
                fprintf(stderr, " %02X", fixture->bytes[b]);
            fprintf(stderr, "\n  ours     :");
            for (unsigned b = 0; b < sizeof(pdu); ++b)
                fprintf(stderr, " %02X", pdu[b]);
            fprintf(stderr, "\n");
            failures++;
            continue;
        }
        rebuilt++;
    }
    check(rebuilt == NS2_MOTION40_CATCHUP_EDGE_COUNT,
          "slot-limit and negative-extreme values match the reference encoder");
    printf("  edge cases matching reference encoder: %u/%u\n",
           rebuilt, NS2_MOTION40_CATCHUP_EDGE_COUNT);
}

static void test_fails_closed(void)
{
    ns2_motion40_catchup_t fields;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    load(&fields, &ns2_motion40_catchup_fixtures[0]);
    check(ns2_motion_pdu40_build_catchup(pdu, &fields), "baseline builds");

    // Elapsed selects the layout. Below 15 the decoder reads a different
    // layout entirely, so these fields would land in the wrong slots.
    ns2_motion40_catchup_t bad = fields;
    bad.elapsed_ticks = 14u;
    check(!ns2_motion_pdu40_build_catchup(pdu, &bad),
          "elapsed below 15 is refused (would select a different layout)");
    bad = fields;
    bad.elapsed_ticks = 0x1000u;
    check(!ns2_motion_pdu40_build_catchup(pdu, &bad), "elapsed over 12 bits refused");

    bad = fields;
    bad.tick = 0x1000u;
    check(!ns2_motion_pdu40_build_catchup(pdu, &bad), "tick over 12 bits refused");

    bad = fields;
    bad.carrier[1] = 1 << 20;  // lane 1 is 21-bit signed
    check(!ns2_motion_pdu40_build_catchup(pdu, &bad), "carrier overflow refused");

    bad = fields;
    bad.accel[1][0] = 1 << 12;  // middle slot is 13-bit signed
    check(!ns2_motion_pdu40_build_catchup(pdu, &bad), "accel overflow refused");

    bad = fields;
    bad.gyro[0][2] = 1 << 15;  // gyro is 16-bit signed
    check(!ns2_motion_pdu40_build_catchup(pdu, &bad), "gyro overflow refused");

    bad = fields;
    bad.tail_bit = 2u;
    check(!ns2_motion_pdu40_build_catchup(pdu, &bad), "tail bit over 1 refused");

    check(!ns2_motion_pdu40_build_catchup(NULL, &fields), "null pdu refused");
    check(!ns2_motion_pdu40_build_catchup(pdu, NULL), "null fields refused");
}

static void test_buffer_untouched_on_failure(void)
{
    ns2_motion40_catchup_t fields;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
    load(&fields, &ns2_motion40_catchup_fixtures[0]);
    fields.accel[0][1] = 1 << 13;  // 14-bit signed slot
    memset(pdu, 0x5C, sizeof(pdu));
    check(!ns2_motion_pdu40_build_catchup(pdu, &fields), "overflow refused");
    for (unsigned i = 0; i < sizeof(pdu); ++i) {
        if (pdu[i] != 0x5Cu) {
            check(0, "caller buffer must be untouched when the build fails");
            break;
        }
    }
}

// Status is written verbatim, including zero. A `status ? status : default`
// idiom cannot tell a genuine zero from an unset field, and zero IS genuine
// wire data: 5 of 858 high-rate packets carry status 0x00. That idiom silently
// rewrote those five into 0x0D.
static void test_status_is_written_verbatim(void)
{
    ns2_motion40_catchup_t fields;
    memset(&fields, 0, sizeof(fields));
    fields.elapsed_ticks = NS2_MOTION40_CATCHUP_MIN_ELAPSED;
    fields.packing_mode = 3u;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];

    fields.status = 0u;
    check(ns2_motion_pdu40_build_catchup(pdu, &fields), "builds with status 0");
    check(pdu[3] == 0u, "a zero status reaches the wire unchanged");

    fields.status = NS2_MOTION40_STATUS_CATCHUP;
    check(ns2_motion_pdu40_build_catchup(pdu, &fields), "builds with status");
    check(pdu[3] == NS2_MOTION40_STATUS_CATCHUP, "status reaches the wire");
}


// --- High-rate layout -------------------------------------------------------
//
// The layout the translator targets, because it is the one with paired
// length-0x1E ground truth: 768 of the 773 genuine 0x28 packets that have a
// carrier alongside them are high-rate. Same bar as catch-up -- rebuild real
// hardware bytes from the fields an independent decoder recovered.
static void test_high_rate_rebuilds_genuine_packets(void)
{
    unsigned matched = 0;
    for (unsigned i = 0; i < NS2_MOTION40_HIGH_RATE_FIXTURE_COUNT; ++i) {
        const ns2_motion40_high_rate_fixture_t *f =
            &ns2_motion40_high_rate_fixtures[i];
        ns2_motion40_high_rate_t fields;
        memset(&fields, 0, sizeof(fields));
        fields.tick = f->tick;
        fields.elapsed_ticks = f->elapsed_ticks;
        fields.status = f->status;
        fields.packing_mode = f->packing_mode;
        fields.tail_value = f->tail_value;
        memcpy(fields.carrier, f->carrier, sizeof(fields.carrier));
        memcpy(fields.accel, f->accel, sizeof(fields.accel));
        memcpy(fields.gyro, f->gyro, sizeof(fields.gyro));

        uint8_t pdu[NS2_MOTION_PDU40_LENGTH];
        if (!ns2_motion_pdu40_build_high_rate(pdu, &fields)) {
            fprintf(stderr, "FAIL: high-rate packet %u refused\n", i);
            failures++;
            continue;
        }
        if (memcmp(pdu, f->bytes, NS2_MOTION_PDU40_LENGTH) != 0) {
            fprintf(stderr, "FAIL: high-rate packet %u differs\n", i);
            for (unsigned b = 0; b < NS2_MOTION_PDU40_LENGTH; ++b) {
                if (pdu[b] != f->bytes[b])
                    fprintf(stderr, "  byte %2u: built 0x%02X genuine 0x%02X\n",
                            b, pdu[b], f->bytes[b]);
            }
            failures++;
            continue;
        }
        matched++;
    }
    check(matched == NS2_MOTION40_HIGH_RATE_FIXTURE_COUNT,
          "every genuine high-rate packet rebuilt byte-for-byte");
    printf("  rebuilt byte-exactly: %u/%u genuine high-rate packets\n",
           matched, NS2_MOTION40_HIGH_RATE_FIXTURE_COUNT);
}

// Elapsed selects the layout, so high-rate fields must never be emitted under
// an elapsed count a decoder would read as normal or catch-up.
static void test_high_rate_fails_closed(void)
{
    ns2_motion40_high_rate_t fields;
    memset(&fields, 0, sizeof(fields));
    fields.packing_mode = 3u;
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH];

    fields.elapsed_ticks = NS2_MOTION40_HIGH_RATE_MAX_ELAPSED;
    check(ns2_motion_pdu40_build_high_rate(pdu, &fields),
          "accepts the largest high-rate elapsed count");
    fields.elapsed_ticks = NS2_MOTION40_HIGH_RATE_MAX_ELAPSED + 1u;
    check(!ns2_motion_pdu40_build_high_rate(pdu, &fields),
          "refuses an elapsed count that would decode as another layout");

    fields.elapsed_ticks = 7u;
    fields.accel[0][0] = 1 << 21;  // one past the signed 22-bit maximum
    check(!ns2_motion_pdu40_build_high_rate(pdu, &fields),
          "refuses acceleration that overflows its 22-bit slot");
    fields.accel[0][0] = 0;
    fields.carrier[2] = 1 << 24;   // one past the signed 25-bit maximum
    check(!ns2_motion_pdu40_build_high_rate(pdu, &fields),
          "refuses a carrier lane that overflows its slot");

    check(!ns2_motion_pdu40_build_high_rate(NULL, &fields), "null pdu");
    check(!ns2_motion_pdu40_build_high_rate(pdu, NULL), "null fields");
}

int main(void)
{
    test_corpus_is_byte_exact();
    test_edge_values_agree_with_the_reference_encoder();
    test_fails_closed();
    test_buffer_untouched_on_failure();
    test_status_is_written_verbatim();
    test_high_rate_rebuilds_genuine_packets();
    test_high_rate_fails_closed();
    if (failures) {
        fprintf(stderr, "ns2_motion_pdu40: %d failure(s)\n", failures);
        return 1;
    }
    printf("ns2_motion_pdu40: all tests passed\n");
    return 0;
}
