#ifndef _USB_H_
#define _USB_H_

#include <stdbool.h>
#include <stdint.h>

// Set true by the USB core (core0) once it has registered as a multicore
// lockout victim, so the Bluetooth core may safely park it to read BOOTSEL.
extern volatile bool usb_lockout_ready;

#ifdef NS2_PRO
// Runtime USB output personality (NS2_PRO builds only). A paired-controller
// single tap cycles controller personalities; a two-second hold enters Config.
// Selection is volatile for the current power-on session only -- never persisted to
// flash (see docs/switch2-gc/usb-personality.md "Runtime mode cycle").
//
// USB_PERSONALITY_SWITCH2_PRO2 = 0 so the correct boot default falls out of
// zero-initialization with no explicit init needed -- Pro Controller 2 is the
// primary, recommended, production-quality personality for using one paired
// controller as a complete Switch 2 controller. USB_PERSONALITY_JOYCON2_L/_R
// (Stage B+C implemented 2026-07-14) are separate, individually-selectable
// EXPERIMENTAL/test personalities, not a recommended full-controller mode --
// see docs/switch2-joycon2/protocol.md "Why not simultaneous L+R" for why
// there is deliberately no combined/paired Joy-Con personality: real Joy-Con
// pairs are two independently-addressed USB devices behind a genuine hub
// (the Charging Grip), and this project's single Pico USB peripheral can
// only ever hold one USB address at a time (confirmed at the register level,
// not a TinyUSB limitation) -- so Left and Right exist as two separate,
// individually-cycled personalities, never a merged/paired one.
typedef enum {
    USB_PERSONALITY_SWITCH2_PRO2 = 0,
    USB_PERSONALITY_NSO_GAMECUBE,
    USB_PERSONALITY_JOYCON2_L,
    USB_PERSONALITY_JOYCON2_R,
    USB_PERSONALITY_CDC_CONFIG,
} usb_personality_t;

// Single authoritative value, owned and written ONLY by the USB core (core0),
// only between tud_disconnect() and tud_connect() in usb_core_task(). The
// Bluetooth/gesture core (core1) must never write this directly -- it may only
// set g_usb_mode_cycle_requested below and wait for core0 to act on it.
extern volatile usb_personality_t g_usb_personality;

// core1 (paired-controller single tap) sets this to request the next
// controller-only personality. Config is excluded from this cycle.
extern volatile bool g_usb_mode_cycle_requested;

// core1 (two-second hold) requests a direct Config transition. If already in
// Config, core0 returns directly to Pro2.
extern volatile bool g_usb_config_mode_requested;

// App/management-requested switch to a SPECIFIC controller personality (config
// `personality <target>`). Set g_usb_requested_personality then raise the flag;
// core0 consumes it at the same safe loop point as the gesture cycle and applies
// it through the identical usb_apply_personality() re-enumeration path (owner
// hardware-confirmed via the BOOTSEL single-tap cycle). Ignored for CDC_CONFIG.
extern volatile usb_personality_t g_usb_requested_personality;
extern volatile bool g_usb_personality_request_pending;

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

// In-band BLE management feature gate (docs/bluetooth/in-band-management-plan.md).
// When true, the config BLE service (RX/TX GATT + wireless bridge) is armed and
// connectable in a normal controller personality, so a phone/web portal can
// manage the adapter (amiibo, colors, personality, bonds) WITHOUT the CDC Config
// re-enumeration that drops the console. Default OFF: when false the management
// path is byte-identical to today (the proven zero-cost early return in
// config_ble_service_task). Shared across cores; only ever written on core0
// (the `mgmt` config command) and read on both. Present in both build axes so
// the shared btstack_host.c gate compiles without #ifdef.
extern volatile bool g_mgmt_enabled;

void usb_core_task();

#endif
