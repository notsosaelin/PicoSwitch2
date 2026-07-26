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

void bootsel_core1_lockout_init(void) {
    g_core1_coop_ready = true;
}

void __no_inline_not_in_flash_func(bootsel_core1_service)(void) {
    if (!g_sample_requested) return;

    // From this point until the request clears, core1 executes only from SRAM
    // with interrupts disabled, so it cannot touch flash while CS is tri-stated.
    uint32_t flags = save_and_disable_interrupts();
    g_core1_parked = true;
    __dmb();
    while (g_sample_requested) {
        __asm volatile("nop");
    }
    g_core1_parked = false;
    __dmb();
    restore_interrupts(flags);
}

void bootsel_sample_core0(void) {
    if (!g_core1_coop_ready) return;

    static uint32_t last_sample_us = 0;
    uint32_t now_us = time_us_32();

    if (g_sample_requested) {
        if (!g_core1_parked) return;  // asynchronous: keep servicing USB while core1 reaches us

        bool pressed = read_bootsel_raw();
        g_bootsel_pressed = pressed;
        g_bootsel_sampled = true;
        __dmb();
        g_sample_requested = false;   // releases core1's SRAM park loop
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
