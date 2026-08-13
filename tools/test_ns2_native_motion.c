#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_native_motion.h"

static void test_valid_lengths_and_freshness(void)
{
    uint8_t report[63] = {0};
    ns2_native_motion_snapshot_t out;
    ns2_native_motion_clear();

    report[0] = 0x42;
    report[0x0E] = 0x1E;
    for (unsigned i = 0; i < 0x1E; ++i) report[0x0F + i] = (uint8_t)(i + 1);
    assert(ns2_native_motion_publish(2, 0, 0, report, sizeof(report), 1000));
    assert(ns2_native_motion_snapshot(&out, 1100, 500));
    assert(out.length == 0x1E && out.source_counter == 0x42 && out.captured_us == 1000);
    assert(out.source_conn_index == 2);
    assert(out.source_generation == 0);
    assert(memcmp(out.data, &report[0x0F], 0x1E) == 0);
    for (unsigned i = 0x1E; i < sizeof(out.data); ++i) assert(out.data[i] == 0);
    assert(!ns2_native_motion_snapshot(&out, 1501, 500));

    report[0] = 0x43;
    report[0x0E] = 0x28;
    for (unsigned i = 0; i < 0x28; ++i) report[0x0F + i] = (uint8_t)(0x80 + i);
    assert(ns2_native_motion_publish(2, 0, 0, report, sizeof(report), UINT32_MAX - 100));
    // Unsigned subtraction intentionally handles the 32-bit timer wrapping.
    assert(ns2_native_motion_snapshot(&out, 99, 200));
    assert(out.length == 0x28 && memcmp(out.data, &report[0x0F], 0x28) == 0);
}

static void test_rejection_and_clear(void)
{
    uint8_t report[63] = {0};
    ns2_native_motion_snapshot_t out;
    ns2_native_motion_clear();

    report[0x0E] = 0x1D;
    assert(!ns2_native_motion_publish(0, 0, 0, report, sizeof(report), 1));
    report[0x0E] = 0x28;
    assert(!ns2_native_motion_publish(0, 0, 0, report, 54, 1));

    assert(ns2_native_motion_publish(0, 0, 0, report, sizeof(report), 10));
    ns2_native_motion_clear();
    assert(!ns2_native_motion_snapshot(&out, 10, 100));
}

static void test_disconnect_holds_last_motion30(void)
{
    uint8_t report[63] = {0};
    ns2_native_motion_snapshot_t out;
    ns2_native_motion_clear();

    report[0] = 7;
    report[0x0E] = 0x1E;
    for (unsigned i = 0; i < 0x1E; ++i) report[0x0F + i] = (uint8_t)(0x20 + i);
    assert(ns2_native_motion_publish(0, 0, 0, report, sizeof(report), 100));

    // A later 0x28 PDU may be current at disconnect; stationary hold deliberately uses the last
    // length-30 phase/acceleration sample whose field boundaries are confirmed.
    report[0] = 8;
    report[0x0E] = 0x28;
    memset(&report[0x0F], 0xEE, 0x28);
    assert(ns2_native_motion_publish(0, 0, 0, report, sizeof(report), 200));
    ns2_native_motion_source_disconnected(300);

    assert(ns2_native_motion_snapshot(&out, 1000000, 50));
    assert(out.held_after_disconnect == 1 && out.length == 0x1E);
    assert(out.source_counter == 7 && out.captured_us == 300);
    assert(out.source_conn_index == 0);
    for (unsigned i = 0; i < 0x1E; ++i) assert(out.data[i] == (uint8_t)(0x20 + i));

    ns2_native_motion_clear();
    assert(!ns2_native_motion_snapshot(&out, 1000000, 50));
}

static void test_generation_guard_rejects_stale_disconnect(void)
{
    uint8_t report[63] = {0};
    ns2_native_motion_snapshot_t out;
    ns2_native_motion_clear();
    report[0x0E] = 0x1E;
    assert(ns2_native_motion_publish_generation(3, 22, 0x057E, 0x2069,
                                                report, sizeof(report), 500));
    assert(!ns2_native_motion_source_disconnected_generation(3, 21, 600));
    assert(ns2_native_motion_snapshot(&out, 601, 200));
    assert(!out.held_after_disconnect && out.source_generation == 22);
    assert(ns2_native_motion_source_disconnected_generation(3, 22, 700));
    assert(ns2_native_motion_snapshot(&out, 701, 200));
    assert(out.held_after_disconnect && out.source_generation == 22);
    ns2_native_motion_clear();
}

static void test_transport_index_to_output_slot(void)
{
    assert(ns2_native_motion_output_slot(0) == 0);
    assert(ns2_native_motion_output_slot(2) == 2);
    assert(ns2_native_motion_output_slot(3) == 3);
    assert(ns2_native_motion_output_slot(4) == 0);   // first BLE transport index
    assert(ns2_native_motion_output_slot(0xFF) == 0);
}

int main(void)
{
    test_valid_lengths_and_freshness();
    test_rejection_and_clear();
    test_disconnect_holds_last_motion30();
    test_generation_guard_rejects_stale_disconnect();
    test_transport_index_to_output_slot();
    puts("ns2_native_motion: all tests passed");
    return 0;
}
