#include "bootsel.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"

#include "usb.h"  // usb_lockout_ready

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

// Bounded timeout for parking the USB core. Confirmed 2026-07-14 as a real fix, not a
// precaution: the project owner reported BOOTSEL gestures (mode-cycle/pairing/wipe) failing
// specifically while plugged into a real Switch 2 console with a controller paired, but NOT while
// plugged into a PC with the same controller paired -- i.e. tied to the console specifically, not
// to Bluetooth traffic. The concrete architectural difference: `ns2_streaming` (switch_pro2.c) --
// and the equivalent for GameCube/Joy-Con2 -- only ever goes true against a genuine console, since
// only a real console completes the full EP0/vendor identity handshake before selecting a report
// and starting to poll; a PC/Steam session typically never reaches that state. Once streaming,
// core0's main loop (`usb_core_task()`) is continuously generating and pushing HID reports as fast
// as the console polls, `tud_hid_n_report()`+`get_global_gamepad_input()` reading cross-core state
// via `critical_section_enter_blocking()` on every cycle -- a much busier, tighter loop than an
// idle/PC-connected core0 ever sees. `multicore_lockout_start_blocking()` has NO timeout: if core0
// can't reach a safe point to grant the lockout promptly under that load, this call -- and with it
// core1's *entire* cooperative run loop (LED, rumble forwarding, the whole BTstack event loop) --
// simply waits, however long that takes. Switched to the bounded-timeout variant so a slow/busy
// core0 can only ever cost this one tick, never stall core1 altogether; a timed-out sample is
// treated as "no observation this tick" (see bootsel_poll()) rather than corrupting the gesture
// state machine with a guessed press/release value.
#define BOOTSEL_LOCKOUT_TIMEOUT_US 2000

// Park the USB core (it may run from flash), sample BOOTSEL, then release it. `*ok` is set false
// if the lockout couldn't be acquired within the timeout -- caller must not trust the returned
// press state in that case (see BOOTSEL_LOCKOUT_TIMEOUT_US's comment).
static bool read_bootsel_locked(bool *ok) {
    if (!multicore_lockout_start_timeout_us(BOOTSEL_LOCKOUT_TIMEOUT_US)) {
        *ok = false;
        return false;
    }
    bool pressed = read_bootsel_raw();
    // Release must also be bounded -- an unconditional multicore_lockout_end_blocking() here would
    // reintroduce the exact same unbounded-wait risk on the way back out.
    *ok = multicore_lockout_end_timeout_us(BOOTSEL_LOCKOUT_TIMEOUT_US);
    return pressed;
}

// Gesture timing.
#define TAP_WINDOW_MS 500  // max gap between taps of the same gesture
#define HOLD_MS 5000       // press duration that counts as a "hold"

bootsel_gesture_t bootsel_poll(uint32_t now_ms) {
    static bool was_pressed = false;
    static uint32_t press_started = 0;
    static bool hold_fired = false;
    static uint8_t tap_count = 0;
    static uint32_t last_tap_ms = 0;

    // Don't touch the flash-CS pin until the USB core is ready to be locked out;
    // otherwise multicore_lockout_start_timeout_us() would just time out waiting for it.
    if (!usb_lockout_ready)
        return BOOTSEL_NONE;

    bool ok;
    bool pressed = read_bootsel_locked(&ok);
    // A timed-out sample (core0 too busy to grant the lockout within
    // BOOTSEL_LOCKOUT_TIMEOUT_US -- confirmed 2026-07-14 to happen while a real console is
    // actively streaming input reports) carries no information about the button's real state.
    // Treat it as "no observation this tick" and retry next tick, rather than feeding a guessed
    // value into the press/release edge tracker below -- a wrong guess here could fabricate a
    // spurious press or release edge and desync tap counting/hold timing from the button's actual
    // physical state.
    if (!ok)
        return BOOTSEL_NONE;

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
