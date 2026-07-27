/*
 * Host tests for the per-source IMU seam (src/bt_hid/motion/ns2_motion_seam.c).
 *
 * The load-bearing test is legacy_equivalence(): before the seam became a
 * per-source table, ns2_seam.c applied ONE hard-coded transform to every
 * source. The DualSense path is hardware-validated against genuine Pro
 * Controller 2 behaviour and Splatoon 3, so the refactor is only safe if the
 * DualSense row reproduces that old formula EXACTLY -- including operand order,
 * because C integer division truncates toward zero and (-x)/2 != -(x/2) is a
 * real hazard for odd negative values.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Iinclude -o build/host-tests/test_ns2_motion_seam.exe \
 *          tools/test_ns2_motion_seam.c src/bt_hid/motion/ns2_motion_seam.c
 */
#include "ns2_motion_seam.h"
#include "switch_pro.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static int16_t clamp16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

// The exact pre-refactor transform, transcribed from ns2_seam.c as it stood
// before NS2_MOTION_SEAMS existed. Do not "simplify" this -- it is a fixture.
static void legacy_seam(const int16_t a[3], const int16_t g[3],
                        int16_t ao[3], int16_t go[3])
{
    ao[0] = clamp16( a[0] / 2);
    ao[1] = clamp16(-a[2] / 2);
    ao[2] = clamp16( a[1] / 2);
    go[0] = clamp16( g[0]);
    go[1] = clamp16(-g[2]);
    go[2] = clamp16( g[1]);
}

// Deterministic pseudo-random sweep, so a failure is reproducible.
static uint32_t rng_state = 0x13579BDFu;
static int16_t next_val(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (int16_t)(rng_state >> 16);
}

static void legacy_equivalence(void)
{
    // Values chosen to hit the sign/rounding traps explicitly, then a broad sweep.
    static const int16_t edge[] = {
        0, 1, -1, 2, -2, 3, -3, 7, -7, 4095, -4095, 4096, -4096,
        8191, -8191, 8192, -8192, 32767, -32768, 32766, -32767,
    };
    const size_t n = sizeof(edge) / sizeof(edge[0]);

    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            for (size_t k = 0; k < n; k++) {
                const int16_t a[3] = {edge[i], edge[j], edge[k]};
                const int16_t g[3] = {edge[k], edge[i], edge[j]};
                int16_t la[3], lg[3], na[3], ng[3];
                legacy_seam(a, g, la, lg);
                ns2_motion_seam_apply(SWITCH_MOTION_SOURCE_DUALSENSE,
                                      a, g, na, ng);
                if (memcmp(la, na, sizeof(la)) != 0 ||
                    memcmp(lg, ng, sizeof(lg)) != 0) {
                    CHECK(0, "DualSense row diverged from legacy at "
                             "a=[%d,%d,%d] g=[%d,%d,%d]: "
                             "accel legacy[%d,%d,%d] new[%d,%d,%d] "
                             "gyro legacy[%d,%d,%d] new[%d,%d,%d]",
                          a[0], a[1], a[2], g[0], g[1], g[2],
                          la[0], la[1], la[2], na[0], na[1], na[2],
                          lg[0], lg[1], lg[2], ng[0], ng[1], ng[2]);
                    return;  // one report is enough; the sweep is large
                }
            }
        }
    }

    for (int iter = 0; iter < 200000; iter++) {
        const int16_t a[3] = {next_val(), next_val(), next_val()};
        const int16_t g[3] = {next_val(), next_val(), next_val()};
        int16_t la[3], lg[3], na[3], ng[3];
        legacy_seam(a, g, la, lg);
        ns2_motion_seam_apply(SWITCH_MOTION_SOURCE_DUALSENSE, a, g, na, ng);
        if (memcmp(la, na, sizeof(la)) != 0 ||
            memcmp(lg, ng, sizeof(lg)) != 0) {
            CHECK(0, "DualSense row diverged from legacy on random iter %d "
                     "a=[%d,%d,%d] g=[%d,%d,%d]",
                  iter, a[0], a[1], a[2], g[0], g[1], g[2]);
            return;
        }
    }
    printf("  legacy equivalence: exhaustive edge sweep + 200k random OK\n");
}

// Wii publishes in the DualSense arrangement, so its row must stay identical to
// the DualSense row. If someone retunes Wii axes, this test should be updated
// deliberately -- that is the point.
static void wii_matches_dualsense(void)
{
    for (int iter = 0; iter < 20000; iter++) {
        const int16_t a[3] = {next_val(), next_val(), next_val()};
        const int16_t g[3] = {next_val(), next_val(), next_val()};
        int16_t da[3], dg[3], wa[3], wg[3];
        ns2_motion_seam_apply(SWITCH_MOTION_SOURCE_DUALSENSE, a, g, da, dg);
        ns2_motion_seam_apply(SWITCH_MOTION_SOURCE_WII, a, g, wa, wg);
        if (memcmp(da, wa, sizeof(da)) != 0 ||
            memcmp(dg, wg, sizeof(dg)) != 0) {
            CHECK(0, "Wii row diverged from DualSense at iter %d", iter);
            return;
        }
    }
}

// Isolation is the whole reason the table exists: no row may read another's data.
static void rows_are_independent(void)
{
    const int16_t a[3] = {1000, 2000, 3000};
    const int16_t g[3] = {111, 222, 333};

    int16_t sa[3], sg[3], da[3], dg[3];
    ns2_motion_seam_apply(SWITCH_MOTION_SOURCE_SWITCH1, a, g, sa, sg);
    ns2_motion_seam_apply(SWITCH_MOTION_SOURCE_DUALSENSE, a, g, da, dg);

    // Switch 1: accel {1,0,2} signs {1,-1,1}; gyro likewise. Slot 2 is the
    // gravity-measured lane (a resting Pro Controller reads +1 g there).
    CHECK(sa[0] == 1000,  "sw1 accel[0] (got %d, want 1000)",  sa[0]);
    CHECK(sa[1] == -500,  "sw1 accel[1] (got %d, want -500)",  sa[1]);
    CHECK(sa[2] == 1500,  "sw1 accel[2] (got %d, want 1500)",  sa[2]);
    CHECK(sg[0] == 222,   "sw1 gyro[0] (got %d, want 222)",    sg[0]);
    CHECK(sg[1] == -111,  "sw1 gyro[1] (got %d, want -111)",   sg[1]);
    CHECK(sg[2] == 333,   "sw1 gyro[2] (got %d, want 333)",    sg[2]);

    // And the DualSense row is untouched by the presence of the Switch 1 row.
    CHECK(da[0] == 500,   "ds5 accel[0] (got %d, want 500)",   da[0]);
    CHECK(da[1] == -1500, "ds5 accel[1] (got %d, want -1500)", da[1]);
    CHECK(da[2] == 1000,  "ds5 accel[2] (got %d, want 1000)",  da[2]);
    CHECK(dg[0] == 111,   "ds5 gyro[0] (got %d, want 111)",    dg[0]);
    CHECK(dg[1] == -333,  "ds5 gyro[1] (got %d, want -333)",   dg[1]);
    CHECK(dg[2] == 222,   "ds5 gyro[2] (got %d, want 222)",    dg[2]);
}

// Every row must be a permutation: each source axis used exactly once, no axis
// dropped or duplicated. A typo like {0,2,2} silently discards an axis.
static void rows_are_permutations(void)
{
    const uint8_t sources[] = {
        SWITCH_MOTION_SOURCE_GENERIC, SWITCH_MOTION_SOURCE_DUALSENSE,
        SWITCH_MOTION_SOURCE_WII, SWITCH_MOTION_SOURCE_SWITCH1,
    };
    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); s++) {
        const ns2_motion_seam_t *row = ns2_motion_seam_for(sources[s]);
        int seen_a[3] = {0, 0, 0}, seen_g[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++) {
            CHECK(row->accel_src[i] >= 0 && row->accel_src[i] < 3,
                  "source %u accel_src[%d] out of range (%d)",
                  sources[s], i, row->accel_src[i]);
            CHECK(row->gyro_src[i] >= 0 && row->gyro_src[i] < 3,
                  "source %u gyro_src[%d] out of range (%d)",
                  sources[s], i, row->gyro_src[i]);
            CHECK(row->accel_sign[i] == 1 || row->accel_sign[i] == -1,
                  "source %u accel_sign[%d] must be +/-1 (%d)",
                  sources[s], i, row->accel_sign[i]);
            CHECK(row->gyro_sign[i] == 1 || row->gyro_sign[i] == -1,
                  "source %u gyro_sign[%d] must be +/-1 (%d)",
                  sources[s], i, row->gyro_sign[i]);
            if (row->accel_src[i] >= 0 && row->accel_src[i] < 3)
                seen_a[row->accel_src[i]]++;
            if (row->gyro_src[i] >= 0 && row->gyro_src[i] < 3)
                seen_g[row->gyro_src[i]]++;
        }
        for (int i = 0; i < 3; i++) {
            CHECK(seen_a[i] == 1, "source %u accel axis %d used %d times",
                  sources[s], i, seen_a[i]);
            CHECK(seen_g[i] == 1, "source %u gyro axis %d used %d times",
                  sources[s], i, seen_g[i]);
        }
    }
}

// A row describes a physical sensor remount, which is a ROTATION. Determinant
// must be +1; -1 is a reflection and cannot describe any real mounting.
//
// This is not pedantry. Gravity cannot detect a reflected frame -- a single
// vector looks perfectly correct -- so an improper row passes every static
// check and misbehaves only under rotation. The SWITCH1 row shipped at det -1:
// its accelerometer matched a genuine Pro Controller 2 to within 1% while its
// gyro produced no horizontal aim whatsoever.
static int permutation_parity(const int8_t src[3])
{
    int8_t p[3] = {src[0], src[1], src[2]};
    int parity = 1;
    for (int i = 0; i < 3; i++) {
        while (p[i] != i) {
            const int8_t j = p[i];
            const int8_t t = p[i]; p[i] = p[j]; p[j] = t;
            parity = -parity;
        }
    }
    return parity;
}

static int seam_determinant(const int8_t src[3], const int8_t sign[3])
{
    int det = permutation_parity(src);
    for (int i = 0; i < 3; i++) det *= sign[i];
    return det;
}

static void rows_are_proper_rotations(void)
{
    const uint8_t sources[] = {
        SWITCH_MOTION_SOURCE_GENERIC, SWITCH_MOTION_SOURCE_DUALSENSE,
        SWITCH_MOTION_SOURCE_WII, SWITCH_MOTION_SOURCE_SWITCH1,
    };
    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); s++) {
        const ns2_motion_seam_t *row = ns2_motion_seam_for(sources[s]);
        const int da = seam_determinant(row->accel_src, row->accel_sign);
        const int dg = seam_determinant(row->gyro_src, row->gyro_sign);
        CHECK(da == 1,
              "source %u accel row has determinant %d; a remount is a rotation, "
              "so it must be +1 (a reflection cannot be a real mounting)",
              sources[s], da);
        CHECK(dg == 1,
              "source %u gyro row has determinant %d; must be +1",
              sources[s], dg);
    }
}

// An unknown//out-of-range source must fall back rather than read past the table.
static void unknown_source_falls_back(void)
{
    const ns2_motion_seam_t *generic =
        ns2_motion_seam_for(SWITCH_MOTION_SOURCE_GENERIC);
    CHECK(ns2_motion_seam_for(200) == generic, "out-of-range source must fall back");
    CHECK(ns2_motion_seam_for(255) == generic, "255 must fall back");
}

int main(void)
{
    legacy_equivalence();
    wii_matches_dualsense();
    rows_are_independent();
    rows_are_permutations();
    rows_are_proper_rotations();
    unknown_source_falls_back();

    if (failures) {
        printf("ns2_motion_seam: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("ns2_motion_seam: all tests passed\n");
    return 0;
}
