#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_motion_pdu.h"
#include "ns2_motion_probe.h"
#include "ns2_native_motion.h"

static uint32_t fake_now_us = 1000;
static bool fake_snapshot_valid;
static ns2_native_motion_snapshot_t fake_snapshot;

uint32_t time_us_32(void)
{
    return fake_now_us;
}

bool ns2_native_motion_snapshot_30(ns2_native_motion_snapshot_t *out,
                                   uint32_t now_us, uint32_t max_age_us)
{
    (void)now_us;
    (void)max_age_us;
    if (!fake_snapshot_valid) return false;
    *out = fake_snapshot;
    return true;
}

static void prepare_snapshot(void)
{
    static const uint8_t genuine_pdu[30] = {
        0x3C,0x44,0x00,0x0C,0x00,0x06,0xE2,0xED,0x01,0x43,
        0x6C,0xE7,0x00,0x44,0xBF,0x86,0x3B,0xBC,0x1E,0x00,
        0x70,0x7F,0x24,0xFD,0xE0,0x7F,0x98,0x10,0x00,0x02
    };
    memset(&fake_snapshot, 0, sizeof(fake_snapshot));
    fake_snapshot.length = sizeof(genuine_pdu);
    fake_snapshot.captured_us = fake_now_us;
    memcpy(fake_snapshot.data, genuine_pdu, sizeof(genuine_pdu));
    fake_snapshot_valid = true;
}

int main(void)
{
    ns2_motion_probe_status_t status;
    uint8_t out[30];
    uint32_t decoded[3];
    const uint32_t target[3] = {0x00012345u, 0x02345678u, 0x03ABCDEFu};
    const uint32_t overflow[3] = {0, 0, NS2_MOTION_ORIENTATION_MASK + 1u};

    assert(!ns2_motion_probe_set_orientation(target));
    assert(!ns2_motion_probe_seed(4));
    assert(ns2_motion_probe_seed(0));
    ns2_motion_probe_get_status(&status);
    assert(status.latched && !status.enabled);
    assert(status.orientation[0] == 0x02000000u);
    assert(status.orientation[1] == 0x02000000u);
    assert(status.orientation[2] == 0x00800000u);
    const int32_t accel[3] = {123, -456, 789};
    assert(ns2_motion_probe_set_accel(accel));
    assert(ns2_motion_probe_set_enabled(true));
    assert(ns2_motion_probe_build(out));
    assert((int32_t)((uint32_t)out[16] | ((uint32_t)out[17] << 8) |
                     ((uint32_t)out[18] << 16) |
                     ((uint32_t)out[19] << 24)) == 123);
    assert(!ns2_motion_probe_set_accel(accel));
    assert(ns2_motion_probe_set_enabled(false));

    prepare_snapshot();
    assert(ns2_motion_probe_latch());
    assert(!ns2_motion_probe_set_orientation(NULL));
    assert(!ns2_motion_probe_set_orientation(overflow));
    assert(ns2_motion_probe_set_orientation(target));

    ns2_motion_probe_get_status(&status);
    assert(status.latched && !status.enabled);
    assert(memcmp(status.orientation, target, sizeof(target)) == 0);
    assert(status.rate[0] == 0 && status.rate[1] == 0 && status.rate[2] == 0);
    assert(status.updates == 0);
    assert(!ns2_motion_probe_build(out));

    assert(ns2_motion_probe_set_enabled(true));
    assert(!ns2_motion_probe_set_orientation(target));
    assert(ns2_motion_probe_build(out));
    assert(ns2_motion_pdu30_get_orientation(out, decoded));
    assert(memcmp(decoded, target, sizeof(target)) == 0);

    assert(ns2_motion_probe_set_enabled(false));
    ns2_motion_probe_reset();
    ns2_motion_probe_get_status(&status);
    assert(status.orientation[0] == 0x01EDE206u);
    assert(status.orientation[1] == 0x00E76C43u);
    assert(status.orientation[2] == 0x0086BF44u);

    puts("ns2_motion_probe: all tests passed");
    return 0;
}
