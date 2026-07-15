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
// CORE0 SAMPLER
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
// Core1 is a cooperative BTstack run loop that reaches a safe point almost immediately, so
// parking *it* is cheap and bounded. Hence the inversion.

// Published by core0, consumed by core1. Single-writer/single-reader of a naturally atomic type,
// so no lock is needed: core1 only ever reads the most recent sample, and a bool cannot tear.
// Worst case core1 sees a sample one interval old, which the gesture timings (500 ms tap window,
// 5 s hold) are wildly tolerant of.
static volatile bool g_bootsel_pressed;
static volatile bool g_bootsel_sampled;    // false until core0 lands its first successful sample
static volatile bool g_core1_lockout_ready;

// Sample cadence. The gesture machine only needs edges resolved well inside its 500 ms tap
// window, so ~5 ms is far finer than required while keeping core1's parked time negligible
// (~20 us per sample => well under 1% of core1's time; its 3 ms rumble timer is unaffected).
// Deliberately NOT sampling on every core0 iteration: that loop is unbounded in rate, and
// parking core1 that often would be exactly the self-inflicted starvation this rewrite removes.
#define BOOTSEL_SAMPLE_INTERVAL_US 5000

// Bounded so a momentarily unresponsive core1 can never stall core0's USB loop. Unlike the old
// core1-side bound, a miss here is genuinely harmless: core0 retries in 5 ms and core1 keeps
// using the previous sample, so no gesture is lost -- a miss costs latency, not function. This
// is why the same "bounded" idea that broke BOOTSEL before is safe on this side of the split.
#define BOOTSEL_CORE1_PARK_TIMEOUT_US 500

void bootsel_core1_lockout_init(void) {
    multicore_lockout_victim_init();
    g_core1_lockout_ready = true;
}

void bootsel_sample_core0(void) {
    if (!g_core1_lockout_ready)
        return;  // core1 not up yet; nothing safe to park

    static uint32_t last_sample_us = 0;
    uint32_t now_us = time_us_32();
    if (last_sample_us != 0 && (now_us - last_sample_us) < BOOTSEL_SAMPLE_INTERVAL_US)
        return;
    last_sample_us = now_us;

    if (!multicore_lockout_start_timeout_us(BOOTSEL_CORE1_PARK_TIMEOUT_US))
        return;  // core1 busy this instant -- keep the last sample, retry in 5 ms

    bool pressed = read_bootsel_raw();

    // Release must also be bounded, for the same reason the acquire is.
    (void)multicore_lockout_end_timeout_us(BOOTSEL_CORE1_PARK_TIMEOUT_US);

    g_bootsel_pressed = pressed;
    g_bootsel_sampled = true;
}

// ============================================================================
// CORE1 GESTURE STATE MACHINE
// ============================================================================

// Gesture timing.
#define TAP_WINDOW_MS 500  // max gap between taps of the same gesture
#define HOLD_MS 5000       // press duration that counts as a "hold"

bootsel_gesture_t bootsel_poll(uint32_t now_ms) {
    static bool was_pressed = false;
    static uint32_t press_started = 0;
    static bool hold_fired = false;
    static uint8_t tap_count = 0;
    static uint32_t last_tap_ms = 0;

    // Nothing sampled yet (core0 hasn't reached its loop, or core1's victim registration hasn't
    // run). Report no gesture rather than treating "unknown" as "released", which would fabricate
    // a release edge the moment the first real sample arrives.
    if (!g_bootsel_sampled)
        return BOOTSEL_NONE;

    // Just a read of core0's published sample -- no lockout, no blocking, no way for this to
    // stall core1's run loop. That property is the entire point of this design; preserve it.
    bool pressed = g_bootsel_pressed;

    if (pressed && !was_pressed) {  // press edge
        press_started = now_ms;
        hold_fired = false;
    }

    if (pressed && !hold_fired && (now_ms - press_started) >= HOLD_MS) {  // hold (fires once)
        hold_fired = true;
        tap_count = 0;
        was_pressed = pressed;
        return BOOTSEL_HOLD;
    }

    if (!pressed && was_pressed) {  // release edge
        if (!hold_fired) {
            tap_count++;
            last_tap_ms = now_ms;
        }
        hold_fired = false;
    }
    was_pressed = pressed;

    // No further taps within the window -> classify the gesture.
    if (tap_count > 0 && (now_ms - last_tap_ms) >= TAP_WINDOW_MS) {
        uint8_t n = tap_count;
        tap_count = 0;
        if (n == 2)
            return BOOTSEL_DOUBLE_TAP;
        if (n >= 3)
            return BOOTSEL_TRIPLE_TAP;
        // single tap: unused
    }
    return BOOTSEL_NONE;
}
