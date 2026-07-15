#include "usb.h"

#include <stdint.h>
#include <stdio.h>

#include <tusb.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>

#include "switch_pro.h"
#include "report.h"
#include "config.h"
#include "bootsel.h"

#ifdef NS2_PRO
#include "switch_pro2.h"
#include "switch_gc.h"
#include "switch_joycon2.h"
#include "usb_mode_cycle.h"
#endif

volatile bool usb_lockout_ready = false;

#ifdef NS2_PRO

volatile usb_personality_t g_usb_personality = USB_PERSONALITY_SWITCH2_PRO2;
volatile bool g_usb_mode_cycle_requested = false;
volatile uint32_t g_usb_mode_ack_until_ms = 0;
volatile usb_personality_t g_usb_mode_ack_personality = USB_PERSONALITY_SWITCH2_PRO2;

// How long the LED shows the post-transition acknowledgement pattern before
// core1's control_timer_handler falls back to its normal status display.
#define USB_MODE_ACK_MS 1200

// Bounded detach interval between tud_disconnect() and tud_connect(). Kept at
// the same 100ms this project's own CDC-config-mode entry has already used
// reliably (this exact re-enumeration mechanism, just previously only ever
// exercised for one specific transition) -- no new, unvalidated value
// introduced. See docs/switch2-gc/usb-personality.md "Transition sequence".
#define USB_DETACH_MS 100

// Internal/log-facing identifier only -- NOT the USB string descriptor text a host sees (that's
// switch_joycon2_strings_l/r's real, Confirmed-byte-exact "Joy-Con 2 (L)"/"(R)" strings, which
// must stay authentic and are untouched by this). This is where "experimental" needs to be
// visible per the project owner's explicit instruction: anywhere this project's own text shows a
// personality name (currently: serial debug logs only), Joy-Con2 L/R must read unambiguously as
// experimental/test personalities, not the recommended full-controller mode (that's Pro2).
static const char *usb_personality_name(usb_personality_t p) {
    switch (p) {
        case USB_PERSONALITY_SWITCH2_PRO2: return "Switch2Pro2";
        case USB_PERSONALITY_NSO_GAMECUBE: return "NSOGameCube";
        case USB_PERSONALITY_JOYCON2_L:    return "Joy-Con 2 Left (Experimental)";
        case USB_PERSONALITY_JOYCON2_R:    return "Joy-Con 2 Right (Experimental)";
        case USB_PERSONALITY_CDC_CONFIG:   return "CDCConfig";
        default:                           return "?";
    }
}

// usb_personality_available()/usb_next_personality(): see usb_mode_cycle.c --
// extracted there so this pure enum-walking logic (no pico-sdk/TinyUSB
// dependency) can be compiled and tested on the host. CDC_CONFIG has a live
// BOOTSEL-hold exit back to Pro2 (2026-07-14 -- see usb-personality.md
// "Runtime mode cycle"): usb_next_personality(CDC_CONFIG) wraps to Pro2, and
// ns2_bt_host.c's control_timer_handler() no longer suppresses BOOTSEL_HOLD
// detection while g_usb_config_mode is true (pairing/wipe gestures remain
// suppressed there, since those still make no sense in config mode).

// Bring up the personality we're about to switch TO with a guaranteed-clean
// slate. This is sufficient to satisfy "old-personality state must not leak
// into the new personality" (NSO-GC.md's transition requirement): each
// personality's own state (ns2_streaming/report_id/imu_enabled/factory[] for
// Pro2; presently nothing for GameCube's Stage-B stub) is entirely private to
// that personality's own module and is never read while a DIFFERENT
// personality is active, so resetting the incoming side to its own clean
// baseline is equivalent to -- and simpler than -- separately tracking a
// "reset the outgoing side" step.
static void usb_reset_personality_state(usb_personality_t p) {
    switch (p) {
        case USB_PERSONALITY_SWITCH2_PRO2:
            ns2_init();
            break;
        case USB_PERSONALITY_NSO_GAMECUBE:
            switch_gc_reset();
            break;
        case USB_PERSONALITY_JOYCON2_L:
            // Side is set here, automatically, as a direct consequence of which of the two
            // separate Joy-Con2 personalities was selected -- never a user-facing toggle inside
            // the module itself (see switch_joycon2.h's own comment).
            switch_joycon2_set_side(JOYCON2_SIDE_LEFT);
            switch_joycon2_reset();
            break;
        case USB_PERSONALITY_JOYCON2_R:
            switch_joycon2_set_side(JOYCON2_SIDE_RIGHT);
            switch_joycon2_reset();
            break;
        default:
            break;  // CDC config has no equivalent per-session runtime state to reset
    }
}

// Perform one full personality transition: disconnect, quiesce, switch,
// reconnect. Called only from usb_core_task()'s own loop, between tud_task()
// iterations -- never while TinyUSB is mid-enumeration, so the descriptor
// callbacks always observe one coherent g_usb_personality value throughout
// any single enumeration (NSO-GC.md's explicit requirement).
static void usb_apply_mode_cycle(void) {
    usb_personality_t old = g_usb_personality;
    usb_personality_t next = usb_next_personality(old);
    if (next == old) {
        printf("[USB] Mode cycle requested but already terminal (%s); ignoring\n",
               usb_personality_name(old));
        return;
    }

    printf("[USB] Mode cycle: %s -> %s (BOOTSEL hold)\n",
           usb_personality_name(old), usb_personality_name(next));

    tud_disconnect();
    sleep_ms(USB_DETACH_MS);
    usb_reset_personality_state(next);
    g_usb_personality = next;
    tud_connect();

    g_usb_mode_ack_personality = next;
    g_usb_mode_ack_until_ms = to_ms_since_boot(get_absolute_time()) + USB_MODE_ACK_MS;

    printf("[USB] Mode cycle complete: now %s\n", usb_personality_name(next));
}

#else  // !NS2_PRO: Switch 1 build -- unchanged two-state storage (see usb.h).

volatile bool g_usb_enter_config = false;
volatile bool g_usb_config_mode = false;

#endif  // NS2_PRO

// Runs on core0. Owns the TinyUSB device stack. In normal mode it emulates the
// active USB personality (Switch 2 Pro Controller 2 or, since 2026-07-13, the
// NSO GameCube Controller -- see usb_personality_t); on a BOOTSEL hold it
// cycles to the next available personality, terminating at CDC config mode.
void usb_core_task() {
    tusb_init();
#ifdef NS2_PRO
    ns2_init();
#else
    switch_pro_init();
#endif

    // Register as a multicore lockout victim. NOTE: this is no longer for BOOTSEL -- core0 now
    // samples BOOTSEL itself and parks core1 instead (see bootsel.h's ARCHITECTURE note; core0
    // cannot grant a lockout promptly from this tight TinyUSB loop while streaming, which broke
    // BOOTSEL and rumble in turn on 2026-07-15). This registration is still required for core1's
    // flash writes (config_service_save() parks core0 before erase/program), so it must stay.
    multicore_lockout_victim_init();
    usb_lockout_ready = true;

#ifndef NS2_PRO
    uint8_t report[64];
#endif

    while (1) {
#ifdef NS2_PRO
        // Advance the runtime USB personality cycle. Volatile, not persisted
        // to flash -- see docs/switch2-gc/usb-personality.md "Runtime mode
        // cycle". Consume the request before acting on it (edge-triggered):
        // core1's gesture detector already latches BOOTSEL_HOLD to fire only
        // once per qualifying press (bootsel.c's hold_fired), so this flag is
        // never re-set while still true, but clearing it first here keeps
        // this function itself idempotent against any future caller that
        // isn't as careful.
        if (g_usb_mode_cycle_requested) {
            g_usb_mode_cycle_requested = false;
            usb_apply_mode_cycle();
        }
#else
        // Switch to configuration mode by re-enumerating as a CDC serial device.
        // Unchanged Switch-1 behavior -- see usb.h's !NS2_PRO branch.
        if (g_usb_enter_config && !g_usb_config_mode) {
            tud_disconnect();
            sleep_ms(100);
            g_usb_config_mode = true;
            tud_connect();
        }
#endif

        tud_task();

        // Sample BOOTSEL from this core (self-rate-limited to ~5 ms; see bootsel.c). Core1 runs
        // the gesture state machine off the value published here and never parks anyone itself,
        // which is what keeps its rumble-forwarding timer on cadence. Placed after tud_task() so
        // a sample can never delay USB servicing, and outside the config-mode early-continue
        // below so gestures keep working in config mode (BOOTSEL_HOLD exits it on NS2_PRO).
        bootsel_sample_core0();

        if (g_usb_config_mode) {
#ifdef NS2_PRO
            ns2_motion_debug_tick();
#endif
            config_cdc_task();
            continue;
        }

        // While the console is asleep (USB suspended) the BT core keeps running,
        // so wake the console only when a controller button is actually pressed.
        if (tud_suspended()) {
            if (report_any_button_pressed())
                tud_remote_wakeup();
        }

#ifdef NS2_PRO
        if (g_usb_personality == USB_PERSONALITY_NSO_GAMECUBE)
            switch_gc_task();
        else if (g_usb_personality == USB_PERSONALITY_JOYCON2_L ||
                 g_usb_personality == USB_PERSONALITY_JOYCON2_R)
            switch_joycon2_task();
        else
            ns2_task();
#else
        for (uint8_t i = 0; i < SWITCH_PRO_MAX_CONTROLLERS; i++) {
            if (tud_hid_n_ready(i)) {
                switch_pro_generate_report(i, report);
                tud_hid_n_report(i, 0, report, sizeof(report));
            }
        }
#endif
    }
}
