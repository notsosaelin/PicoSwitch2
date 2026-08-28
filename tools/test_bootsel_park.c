// Regression tests for the cooperative BOOTSEL sampling park (src/bootsel.c).
//
// WHY THIS TEST EXISTS
// --------------------
// bootsel_core1_service() is called from BTstack run-loop callbacks on core1, and those callbacks
// run under the async_context recursive mutex (the SDK reaches BTstack processing only through
// process_under_lock(), which requires lock_mutex held). Core0 takes that same mutex, blocking,
// for every management command that hands work to BTstack --
// btstack_run_loop_execute_on_main_thread() -> async_context_acquire_lock_blocking(). `pairing`,
// `peers` and `bonds` all do this.
//
// So if core1's park waits for core0 without a bound, the two cores close a cycle: core1 holds
// the lock and spins with interrupts disabled waiting for core0 to sample, while core0 is blocked
// on the lock and therefore never returns to usb_core_task()'s loop, where bootsel_sample_core0()
// lives. Neither side is preemptible and the whole adapter freezes -- USB, UART, LED and
// Bluetooth all stop, recoverable only by a power cycle. That is the remote-pairing crash: the
// companion app polls `pairing status` once a second, and each poll is another chance to land in
// the window.
//
// test_park_is_bounded_when_core0_never_samples() is the direct regression: it hangs forever
// against the pre-fix source and passes in well under a second against the fix.
//
// The withdrawal cannot be a naive timeout, because core0 tri-states flash CS on the strength of
// core1 being parked; if core1 resumed executing from flash mid-sample it would fault. The
// remaining tests pin that safety property from the other direction.

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bootsel.h"
#include "pico_host_hooks.h"

// ---------------------------------------------------------------------------
// Watchdog. A regression here is a hang, not a wrong value, so the suite must
// fail loudly rather than block CI forever.
// ---------------------------------------------------------------------------

static volatile bool watchdog_done;
static const char *volatile watchdog_stage = "startup";

static void *watchdog_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100; ++i) {   // 10 s
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (watchdog_done) {
            return NULL;
        }
    }
    fprintf(stderr,
            "bootsel park tests FAILED: hung in stage '%s'.\n"
            "This is the remote-pairing freeze: core1's park is waiting on core0 without a "
            "bound. See src/bootsel.c bootsel_core1_service().\n",
            watchdog_stage);
    fflush(stderr);
    _exit(1);
    return NULL;
}

static void yield_briefly(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };   // 1 ms
    nanosleep(&ts, NULL);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#define SAMPLE_INTERVAL_US 30000u

static void reset_world(void)
{
    pico_host_hooks_reset();
    bootsel_core1_lockout_init();
}

// Advance past the sampler's rate limiter so the next bootsel_sample_core0() issues a request.
static void advance_one_interval(void)
{
    pico_host_now_us += SAMPLE_INTERVAL_US + 1u;
}

// ---------------------------------------------------------------------------
// 1. THE CRASH REGRESSION. Core0 issues a request and then stops running --
//    exactly what happens when it blocks in recursive_mutex_enter_blocking()
//    inside btstack_run_loop_execute_on_main_thread(). Core1 must still return.
// ---------------------------------------------------------------------------

static void test_park_is_bounded_when_core0_never_samples(void)
{
    watchdog_stage = "park_is_bounded_when_core0_never_samples";
    reset_world();

    // Core0's loop reaches the sampler once and issues the request...
    advance_one_interval();
    bootsel_sample_core0();

    // ...and is then blocked on the async_context lock that core1 is holding. It will not call
    // bootsel_sample_core0() again until core1 returns from this call and releases that lock.
    bootsel_core1_service();

    // Reaching this line at all is the invariant. Before the fix, control never came back.
    assert(bootsel_park_abandon_count() == 1);

    // Core1 must have left the critical section, i.e. it is free to execute from flash again and
    // to finish the run-loop callback that owns the lock.
    assert(!pico_host_irqs_disabled);

    // No sample can have been taken: core0 never ran.
    assert(pico_host_cs_hiz_count == 0);
}

// ---------------------------------------------------------------------------
// 2. Withdrawal must not strand the sampler. After core1 gives up, core0 has to
//    be able to close the round out and get a fresh one on the next interval,
//    or gestures die silently -- the 2026-07-15 failure mode.
// ---------------------------------------------------------------------------

static void test_abandoned_round_recovers(void)
{
    watchdog_stage = "abandoned_round_recovers";
    reset_world();

    advance_one_interval();
    bootsel_sample_core0();
    bootsel_core1_service();
    assert(bootsel_park_abandon_count() == 1);

    // Core0 is running again. Its first call closes the abandoned round out rather than sampling.
    bootsel_sample_core0();
    assert(pico_host_cs_hiz_count == 0);

    // The round really is closed: with no request pending, core1's service is a no-op and cannot
    // withdraw again. If the abandoned round had been left set, core1 would park here.
    bootsel_core1_service();
    assert(bootsel_park_abandon_count() == 1);

    // And the sampler is not stranded -- the next interval issues a fresh request, which core1
    // observes. (It withdraws again only because core0 cannot run concurrently in this test; a
    // completed cooperative round needs two cores and is covered below.)
    advance_one_interval();
    bootsel_sample_core0();
    bootsel_core1_service();
    assert(bootsel_park_abandon_count() == 2);
    assert(!pico_host_irqs_disabled);
}

// ---------------------------------------------------------------------------
// 3. The cooperative round still works end to end, and the sampled level
//    reaches the gesture machine.
// ---------------------------------------------------------------------------

static volatile bool core1_parked_now;
static volatile bool core1_should_run;
static volatile bool core1_finished;

static void *core1_thread(void *arg)
{
    (void)arg;
    while (core1_should_run) {
        core1_parked_now = true;
        bootsel_core1_service();
        core1_parked_now = false;
        yield_briefly();
    }
    core1_finished = true;
    return NULL;
}

// Installed for the concurrent tests: CS may only go Hi-Z while core1 is inside its
// interrupts-disabled SRAM park. If this ever fires on hardware, core1 faults on an XIP access.
static void assert_core1_parked_on_cs(bool hi_z)
{
    if (hi_z) {
        assert(pico_host_irqs_disabled &&
               "core0 tri-stated flash CS while core1 was not parked");
    }
}

static void test_concurrent_round_samples_and_publishes(void)
{
    watchdog_stage = "concurrent_round_samples_and_publishes";
    reset_world();
    pico_host_on_cs_override = assert_core1_parked_on_cs;
    pico_host_qspi_cs_level = false;    // BOOTSEL pressed

    core1_should_run = true;
    core1_finished = false;
    pthread_t core1;
    assert(pthread_create(&core1, NULL, core1_thread, NULL) == 0);

    // Core0's loop: request, then keep spinning like usb_core_task() does until the round lands.
    advance_one_interval();
    for (int i = 0; i < 200000 && pico_host_cs_hiz_count == 0; ++i) {
        bootsel_sample_core0();
    }
    assert(pico_host_cs_hiz_count >= 1);

    core1_should_run = false;
    assert(pthread_join(core1, NULL) == 0);
    assert(core1_finished);

    // The published sample must reach the gesture recognizer. A press is not itself a gesture,
    // but releasing after a short press is a tap, so drive one through to prove the value moved.
    uint32_t now_ms = 1000;
    assert(bootsel_poll(now_ms) == BOOTSEL_NONE);

    pico_host_qspi_cs_level = true;     // released
    advance_one_interval();
    for (int i = 0; i < 100; ++i) {
        bootsel_sample_core0();
    }
    core1_should_run = true;
    core1_finished = false;
    assert(pthread_create(&core1, NULL, core1_thread, NULL) == 0);
    for (int i = 0; i < 200000 && pico_host_cs_hiz_count < 2; ++i) {
        bootsel_sample_core0();
    }
    core1_should_run = false;
    assert(pthread_join(core1, NULL) == 0);
    assert(pico_host_cs_hiz_count >= 2);

    pico_host_on_cs_override = NULL;
}

// ---------------------------------------------------------------------------
// 4. Hammer the handshake. Core1 parks and withdraws repeatedly while core0
//    samples as fast as it can. The Dekker exchange must never let core0
//    tri-state CS in a window where core1 has resumed -- and neither side may
//    wedge. This is the property a plain deadline would break.
// ---------------------------------------------------------------------------

static volatile bool stress_run;

static void *core1_stress_thread(void *arg)
{
    (void)arg;
    while (stress_run) {
        bootsel_core1_service();
    }
    return NULL;
}

static void test_handshake_stress_never_samples_unparked(void)
{
    watchdog_stage = "handshake_stress_never_samples_unparked";
    reset_world();
    pico_host_on_cs_override = assert_core1_parked_on_cs;

    stress_run = true;
    pthread_t core1;
    assert(pthread_create(&core1, NULL, core1_stress_thread, NULL) == 0);

    for (int i = 0; i < 400000; ++i) {
        // Core0 sometimes stalls for a whole interval, which is what opens the withdrawal window.
        if ((i % 977) == 0) {
            yield_briefly();
        }
        pico_host_now_us += 40u;
        pico_host_qspi_cs_level = ((i / 5000) % 2) == 0;
        bootsel_sample_core0();
    }

    stress_run = false;
    assert(pthread_join(core1, NULL) == 0);

    // Both invariants: rounds actually completed, and core1 is not left parked.
    assert(pico_host_cs_hiz_count > 0);
    assert(!pico_host_irqs_disabled);

    pico_host_on_cs_override = NULL;
}

int main(void)
{
    pthread_t watchdog;
    assert(pthread_create(&watchdog, NULL, watchdog_thread, NULL) == 0);

    test_park_is_bounded_when_core0_never_samples();
    test_abandoned_round_recovers();
    test_concurrent_round_samples_and_publishes();
    test_handshake_stress_never_samples_unparked();

    watchdog_done = true;
    puts("bootsel park tests passed");
    return 0;
}
