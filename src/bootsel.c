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

// Park the USB core (it may run from flash), sample BOOTSEL, then release it.
static bool read_bootsel_locked(void) {
    multicore_lockout_start_blocking();
    bool pressed = read_bootsel_raw();
    multicore_lockout_end_blocking();
    return pressed;
}

// Gesture timing.
#define TAP_WINDOW_MS 400  // max gap between taps of the same gesture
#define HOLD_MS 5000       // press duration that counts as a "hold"

bootsel_gesture_t bootsel_poll(uint32_t now_ms) {
    static bool was_pressed = false;
    static uint32_t press_started = 0;
    static bool hold_fired = false;
    static uint8_t tap_count = 0;
    static uint32_t last_tap_ms = 0;

    // Don't touch the flash-CS pin until the USB core is ready to be locked out;
    // otherwise multicore_lockout_start_blocking() would hang waiting for it.
    if (!usb_lockout_ready)
        return BOOTSEL_NONE;

    bool pressed = read_bootsel_locked();

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
