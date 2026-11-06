#include "bootsel.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"

// Read BOOTSEL by briefly tri-stating the flash CS pin and sampling it. Must run
// from RAM with interrupts disabled — and the *other* core must be parked, since
// it may be executing from flash (XIP) which is unavailable during the sample.
static bool __no_inline_not_in_flash_func(read_bootsel_raw)(void) {
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();

    // Drive the CS output-enable override LOW -> output disabled (Hi-Z), so the
    // pin floats and BOOTSEL can pull it low when pressed.
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    for (volatile int i = 0; i < 1000; ++i)
        ;

#if PICO_RP2040
    bool pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));
#else
    bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
#endif

    // Restore normal (peripheral-driven) CS operation.
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}

// ============================================================================
// COOPERATIVE CROSS-CORE SAMPLER
// ============================================================================
//
// Why core0 samples and core1 consumes: see bootsel.h's ARCHITECTURE note. Short version --
// core0 (TinyUSB, tight unbounded loop) cannot promptly grant a lockout while streaming to a
// host, so having core1 park core0 was a dead end in BOTH directions, and 2026-07-15's hardware
// testing hit both ends of it:
//   - multicore_lockout_start_blocking() -> core1 waits however long core0 takes. BOOTSEL worked,
//     but core1's whole run loop (incl. rumble forwarding on its 3 ms timer) stalled with it, so
//     rumble stop commands landed late and Xbox motors ran on (their loop_count 0xEB keeps
//     pulsing ~11.75 s per trigger unless superseded) -- reported as uncontrollable rumble.
//   - a 2 ms bounded timeout -> core1 healthy and rumble fine, but the sample never succeeded
//     while a controller was connected, so bootsel_poll() returned BOOTSEL_NONE forever and every
//     gesture silently did nothing. Worst with DualSense (Classic BT), which loads core0 hardest.
// The original inversion still asked multicore_lockout to interrupt core1 and acknowledge within
// 200 us. Hardware testing with a DualSense proved that deadline can miss indefinitely under
// Classic-BT traffic. Use a cooperative request/ack instead: core0 never waits, core1 enters this
// RAM-resident park from its 3 ms timer, and core0 samples on a later USB-loop iteration.

// Published by core0, consumed by core1. Single-writer/single-reader of a naturally atomic type,
// so no lock is needed: core1 only ever reads the most recent sample, and a bool cannot tear.
// Worst case core1 sees a sample one interval old, which the gesture timings (500 ms tap window,
// 2 s hold) are tolerant of.
static volatile bool g_bootsel_pressed;
static volatile bool g_bootsel_sampled;    // false until core0 lands its first successful sample
static volatile bool g_core1_coop_ready;
static volatile bool g_sample_requested;
static volatile bool g_core1_parked;

// Park-withdrawal handshake. See bootsel_core1_service() for why core1's park MUST be bounded and
// why ending it needs a two-sided protocol rather than a plain timeout.
static volatile bool g_core0_sampling;    // core0 has claimed this round and is committed to it
static volatile bool g_park_abandoned;    // core1 withdrew; core0 must close the round out
static volatile uint32_t g_park_abandon_count;

// Sample cadence. MUST stay coarse -- 30 ms, matching the CONTROL_TICK_MS rate the old core1-side
// sampler used and which the gesture machine was always tuned around (500 ms tap window, 2 s
// hold), so nothing is lost by it.
//
// Do NOT shorten this "for responsiveness". A first cut used 5 ms and broke NSO GameCube rumble
// on real hardware (2026-07-15): a genuine console polls the GC rumble OUT endpoint every ~4 ms
// (see ns2_bt_host.c's RUMBLE_TICK_MS comment), and every sample here both disables interrupts on
// core0 for ~20 us (read_bootsel_raw) and parks core1 -- i.e. it drops a cross-core handshake and
// an IRQ blackout right on top of a 4 ms-critical USB endpoint, 200x/second, while also
// interrupting the CYW43/BTstack core. At 30 ms the same work costs ~0.7% of each core and sits
// well outside that endpoint's cadence.
#define BOOTSEL_SAMPLE_INTERVAL_US 30000

// Upper bound on how long core1 will hold the park waiting for core0 (~1.6 ms on Pico W at
// 125 MHz, ~1.3 ms on Pico 2 W at 150 MHz; the body is a handful of cycles per iteration).
//
// This bound is NOT a tuning knob -- it is the thing that stops a total system freeze. See
// bootsel_core1_service(). It only has to exceed one core0 USB-loop iteration, which is a few
// microseconds; anything in the millisecond range never fires in normal operation, and
// bootsel_park_abandon_count() is there to prove that on real hardware. Do not shrink it toward
// a normal iteration time: an abandoned round costs a whole 30 ms sample, and 2026-07-15's
// hardware testing showed that starving the sampler makes every gesture silently stop working.
// A count is used rather than time_us_32() so the wait touches nothing outside SRAM.
#define BOOTSEL_PARK_MAX_SPINS 40000u

void bootsel_core1_lockout_init(void) {
    g_core1_coop_ready = true;
}

// CORE1 ONLY. Voluntary SRAM park so core0 can tri-state flash CS and read the button.
//
// THE PARK MUST STAY BOUNDED. It waits for core0, with interrupts disabled, from inside a BTstack
// run-loop callback -- and BTstack callbacks run under the async_context recursive mutex
// (process_under_lock() in the SDK's async_context_threadsafe_background.c is only ever reached
// with lock_mutex held). Core0 takes that same mutex, blocking, whenever it hands work to core1:
// btstack_run_loop_execute_on_main_thread() -> async_context_acquire_lock_blocking(). That is
// every management command that touches BTstack state -- `pairing`, `peers`, `bonds`.
//
// So the two cores can close a cycle:
//   core1: holds lock_mutex -> parks here with IRQs off -> waits for core0
//   core0: blocked in recursive_mutex_enter_blocking() -> never returns to usb_core_task()'s
//          loop -> never reaches bootsel_sample_core0() -> never clears the request
// Neither side is preemptible, so the whole adapter freezes: USB, UART, LED and Bluetooth all
// stop and only a power cycle recovers it. That is exactly what an unbounded wait here did when
// the companion app began polling `pairing status` once a second during remote pairing -- each
// poll is another chance to land in the window, which is why it took a couple of seconds rather
// than being instant, and why the local BOOTSEL gesture (which generates no core0 management
// traffic) never showed it.
//
// The distinguishing detail, because it is easy to get wrong when reasoning about the other
// cross-core wait in this firmware: this park waits on core0's FOREGROUND progress, which only
// usb_core_task()'s loop advances. config_service_save()'s multicore_lockout_start_blocking()
// waits on core0's SIO FIFO IRQ instead, and core0 blocks on the async_context mutex inside
// __wfe() with interrupts enabled (spin_unlock restores the pre-spinlock state), so that one is
// still serviced and does not close a cycle. Only a wait on foreground progress is fatal here.
//
// Withdrawing needs a handshake, not just a deadline: core0 tri-states CS on the strength of
// g_core1_parked, and if core1 resumed executing from flash mid-sample it would fault. The
// exchange below is Dekker's -- each side publishes its intent, barriers, then reads the other's
// -- so exactly one of "core0 samples" and "core1 withdraws" can win. If core0 wins, it is
// committed and finishes in ~20 us, so the remaining wait is bounded anyway.
void __no_inline_not_in_flash_func(bootsel_core1_service)(void) {
    if (!g_sample_requested) return;

    // From this point until the request clears, core1 executes only from SRAM
    // with interrupts disabled, so it cannot touch flash while CS is tri-stated.
    uint32_t flags = save_and_disable_interrupts();
    g_core1_parked = true;
    __dmb();

    uint32_t spins = BOOTSEL_PARK_MAX_SPINS;
    bool core0_committed = false;
    while (g_sample_requested) {
        if (core0_committed || spins != 0u) {
            if (spins != 0u) --spins;
            __asm volatile("nop");
            continue;
        }

        // Deadline reached without core0 sampling. Publish the withdrawal, barrier,
        // then check whether core0 claimed the round inside the same window.
        g_core1_parked = false;
        __dmb();
        if (g_core0_sampling) {
            // Core0 won the race and may already be holding CS tri-stated. Stay in
            // SRAM until it finishes -- bounded, because it is committed to a ~20 us
            // read and is by definition not blocked on us.
            g_core1_parked = true;
            __dmb();
            core0_committed = true;
            continue;
        }

        // We won: core0 is not sampling and cannot start (it re-reads g_core1_parked
        // after publishing g_core0_sampling). Leave the request set and hand the
        // round to core0 to close out, so it is re-requested on the next interval.
        ++g_park_abandon_count;
        g_park_abandoned = true;
        __dmb();
        restore_interrupts(flags);
        return;
    }

    g_core1_parked = false;
    __dmb();
    restore_interrupts(flags);
}

uint32_t bootsel_park_abandon_count(void) {
    return g_park_abandon_count;
}

void bootsel_sample_core0(void) {
    if (!g_core1_coop_ready) return;

    static uint32_t last_sample_us = 0;
    uint32_t now_us = time_us_32();

    if (g_sample_requested) {
        if (g_park_abandoned) {
            // Core1 withdrew because we could not reach it inside its deadline -- it was
            // holding a lock we were blocked on, or otherwise could not afford to wait (see
            // bootsel_core1_service). Close the round out; the interval below re-requests, so
            // a collision costs one 30 ms sample and nothing else.
            g_park_abandoned = false;
            g_sample_requested = false;
            __dmb();
            return;
        }
        if (!g_core1_parked) return;  // asynchronous: keep servicing USB while core1 reaches us

        // Claim the round, then re-read core1's state after the barrier. This is our half of
        // the Dekker exchange in bootsel_core1_service(): if core1 withdrew in the same window
        // we must not tri-state CS, because it is executing from flash again.
        g_core0_sampling = true;
        __dmb();
        if (!g_sample_requested || !g_core1_parked) {
            g_core0_sampling = false;
            __dmb();
            return;   // the g_park_abandoned branch above closes the round out next call
        }

        bool pressed = read_bootsel_raw();
        g_bootsel_pressed = pressed;
        g_bootsel_sampled = true;
        __dmb();
        g_sample_requested = false;   // releases core1's SRAM park loop
        __dmb();
        g_core0_sampling = false;
        return;
    }

    if (last_sample_us != 0 && (now_us - last_sample_us) < BOOTSEL_SAMPLE_INTERVAL_US)
        return;
    last_sample_us = now_us;
    g_sample_requested = true;
    __dmb();
}

// ============================================================================
// CORE1 GESTURE STATE MACHINE
// ============================================================================

static bootsel_gesture_state_t g_bootsel_gesture_state;

bootsel_gesture_t bootsel_poll(uint32_t now_ms) {
    // Nothing sampled yet (core0 hasn't reached its loop, or core1's cooperative service hasn't
    // run). The pure recognizer preserves the established "unknown is not released" behavior.
    // Both inputs are single-read snapshots of core0's published atomic bools; no lockout or
    // blocking is introduced here.
    bool sample_valid = g_bootsel_sampled;
    bool pressed = g_bootsel_pressed;
    return bootsel_gesture_update(&g_bootsel_gesture_state, sample_valid, pressed, now_ms);
}
