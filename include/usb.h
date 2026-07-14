#ifndef _USB_H_
#define _USB_H_

#include <stdbool.h>
#include <stdint.h>

// Set true by the USB core (core0) once it has registered as a multicore
// lockout victim, so the Bluetooth core may safely park it to read BOOTSEL.
extern volatile bool usb_lockout_ready;

#ifdef NS2_PRO
// Runtime USB output personality (NS2_PRO builds only). Cycled by a ~5s BOOTSEL
// hold; volatile for the current power-on session only -- never persisted to
// flash (see docs/switch2-gc/usb-personality.md "Runtime mode cycle").
//
// USB_PERSONALITY_SWITCH2_PRO2 = 0 so the correct boot default falls out of
// zero-initialization with no explicit init needed. USB_PERSONALITY_JOYCON2 is
// a reserved identity for a not-yet-implemented future personality -- it must
// never be enumerated; the mode-cycle logic skips it (see usb_next_personality()
// in usb.c).
typedef enum {
    USB_PERSONALITY_SWITCH2_PRO2 = 0,
    USB_PERSONALITY_NSO_GAMECUBE,
    USB_PERSONALITY_JOYCON2,       // reserved, not implemented -- always skipped
    USB_PERSONALITY_CDC_CONFIG,
} usb_personality_t;

// Single authoritative value, owned and written ONLY by the USB core (core0),
// only between tud_disconnect() and tud_connect() in usb_core_task(). The
// Bluetooth/gesture core (core1) must never write this directly -- it may only
// set g_usb_mode_cycle_requested below and wait for core0 to act on it.
extern volatile usb_personality_t g_usb_personality;

// core1 (bootsel hold gesture) sets this to request "advance to the next
// available personality"; core0's main loop polls it, performs the full
// transition, and clears it. Mirrors the existing g_usb_enter_config
// cross-core contract exactly -- no new synchronization pattern introduced.
extern volatile bool g_usb_mode_cycle_requested;

// Derived, read-only compatibility view -- NOT an independent source of truth.
// True exactly when g_usb_personality == USB_PERSONALITY_CDC_CONFIG. Kept as a
// macro (not a second variable) so it can never drift out of sync with the enum.
#define g_usb_config_mode ((bool)(g_usb_personality == USB_PERSONALITY_CDC_CONFIG))

// Mode-transition LED acknowledgement. The LED itself is only ever touched by
// core1 (see ns2_bt_host.c's control_timer_handler) -- cyw43_arch_gpio_put()
// is not safe to call concurrently from both cores. So core0 (which owns the
// actual transition) only publishes WHEN a transition completed and WHICH
// personality it landed on; core1's existing LED priority chain reads these
// two fields and renders a short, bounded flash pattern from them, the same
// way it already renders the wipe-flash pattern from wipe_until_ms. 0 = idle.
extern volatile uint32_t g_usb_mode_ack_until_ms;
extern volatile usb_personality_t g_usb_mode_ack_personality;

#else  // !NS2_PRO: Switch 1 build -- unchanged two-state behavior, no cycling.

// Configuration mode. The Bluetooth core sets g_usb_enter_config (via the
// BOOTSEL hold gesture) to request it; the USB core then re-enumerates as a CDC
// serial device and sets g_usb_config_mode true. Used by the descriptor
// callbacks (HID vs CDC) and the LED feedback. Preserved exactly as before for
// the Switch 1 build axis -- NSO GameCube mode-cycling must not become
// user-visible here (docs/switch2-gc/usb-personality.md "Runtime mode cycle").
extern volatile bool g_usb_enter_config;
extern volatile bool g_usb_config_mode;

#endif  // NS2_PRO

void usb_core_task();

#endif
