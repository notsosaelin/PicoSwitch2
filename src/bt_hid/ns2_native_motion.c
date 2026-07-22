#include "ns2_native_motion.h"
#include "switch_pro.h"

#include <string.h>

typedef struct {
    uint8_t valid;
    ns2_native_motion_snapshot_t snapshot;
    uint8_t last_motion30_valid;
    ns2_native_motion_snapshot_t last_motion30;
} ns2_native_motion_state_t;

// Single producer (BTstack/core1), single consumer (USB/core0). A seqlock avoids holding a
// cross-core spinlock in either timing-sensitive path: odd sequence = write in progress; an
// unchanged even sequence brackets a coherent copy.
static volatile uint32_t s_sequence;
static ns2_native_motion_state_t s_state;

static void begin_write(void)
{
    (void)__atomic_add_fetch(&s_sequence, 1u, __ATOMIC_ACQ_REL);
}

static void end_write(void)
{
    (void)__atomic_add_fetch(&s_sequence, 1u, __ATOMIC_RELEASE);
}

bool ns2_native_motion_publish(uint8_t source_conn_index,
                               const uint8_t *report, uint16_t report_length,
                               uint32_t captured_us)
{
    if (!report || report_length < 15u) return false;
    const uint8_t length = report[0x0E];
    if (length != 0x1Eu && length != 0x28u) return false;
    if ((uint16_t)(15u + length) > report_length || length > NS2_NATIVE_MOTION_MAX_DATA) {
        return false;
    }

    begin_write();
    s_state.valid = 1;
    s_state.snapshot.length = length;
    s_state.snapshot.source_counter = report[0];
    s_state.snapshot.source_conn_index = source_conn_index;
    s_state.snapshot.held_after_disconnect = 0;
    s_state.snapshot.captured_us = captured_us;
    memcpy(s_state.snapshot.data, &report[0x0F], length);
    if (length < NS2_NATIVE_MOTION_MAX_DATA) {
        memset(&s_state.snapshot.data[length], 0, NS2_NATIVE_MOTION_MAX_DATA - length);
    }
    if (length == 0x1E) {
        s_state.last_motion30_valid = 1;
        s_state.last_motion30 = s_state.snapshot;
    }
    end_write();
    return true;
}

bool ns2_native_motion_snapshot(ns2_native_motion_snapshot_t *out, uint32_t now_us,
                                uint32_t max_age_us)
{
    if (!out) return false;

    ns2_native_motion_state_t local;
    uint32_t before;
    uint32_t after;
    for (;;) {
        before = __atomic_load_n(&s_sequence, __ATOMIC_ACQUIRE);
        if (before & 1u) continue;
        local = s_state;
        after = __atomic_load_n(&s_sequence, __ATOMIC_ACQUIRE);
        if (before == after && !(after & 1u)) break;
    }

    if (!local.valid ||
        (!local.snapshot.held_after_disconnect &&
         (uint32_t)(now_us - local.snapshot.captured_us) > max_age_us)) {
        return false;
    }
    *out = local.snapshot;
    return true;
}

void ns2_native_motion_clear(void)
{
    begin_write();
    memset(&s_state, 0, sizeof(s_state));
    end_write();
}

void ns2_native_motion_source_disconnected(uint32_t disconnected_us)
{
    begin_write();
    if (s_state.last_motion30_valid) {
        s_state.snapshot = s_state.last_motion30;
        s_state.snapshot.captured_us = disconnected_us;
        s_state.snapshot.held_after_disconnect = 1;
        s_state.valid = 1;
    } else {
        s_state.valid = 0;
    }
    end_write();
}

uint8_t ns2_native_motion_output_slot(uint8_t source_conn_index)
{
    return source_conn_index < SWITCH_PRO_MAX_CONTROLLERS ? source_conn_index : 0u;
}
