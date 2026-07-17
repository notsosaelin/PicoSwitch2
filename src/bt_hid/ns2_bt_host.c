// ns2_bt_host.c — core1 Bluetooth bring-up for the BT_STACK_JOYPAD build.
//
// Replaces bluepad32's uni_init() path (main.c bluepad_core_task) with the
// joypad-os bthid stack: register the HID drivers, bring up BTstack + the HID
// host over the CYW43 transport, and run a periodic control timer that drives
// the stack, the LED, and the BOOTSEL pairing/wipe/config gestures — matching
// the behavior of pico_switch_platform.c's control_timer_handler, but on the
// bt_* transport API instead of bluepad32. Only compiled under -DBT_STACK_JOYPAD.

#include <string.h>

#include <btstack_run_loop.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>

#include "bt/transport/bt_transport.h"
#include "bt/bthid/bthid.h"
#include "bt/bthid/bthid_registry.h"
#include "bt/btstack/btstack_host.h"

#include "bootsel.h"
#include "config.h"
#include "ns2_wake.h"
#include "usb.h"

// The CYW43 transport instance (src/bt_hid/bt/transport/bt_transport_cyw43.c).
// Its init() does cyw43_arch_init + btstack_cyw43_init + HID-host power-on.
extern const bt_transport_t bt_transport_cyw43;

#define CONTROL_TICK_MS   30
// Rumble-forwarding poll rate: Confirmed 2026-07-14 to be a real bug, found while chasing an
// erratic real-console GameCube-mode rumble report ("fires nonstop, then randomly stops"). A
// real console polls the GC rumble OUT endpoint roughly every ~4ms (re-confirmed 2026-07-14 by
// re-analyzing ndeadly's rumble-procon-gccon.pcapng.gz at the usbll link-layer level -- the host
// writes to that endpoint on every ~4ms interrupt-OUT slot, sending a ZLP when idle and a data
// packet only when the commanded intensity changes). But `bthid_task()` -- the function whose
// per-device `.task()` callbacks actually forward the current rumble state to the connected
// Bluetooth pad (`gamepad_task()` in bthid_gamepad.c) -- was previously driven only by
// `control_timer_handler` below, a 30ms periodic timer. A real console/game driving fast,
// bursty rumble (repeated short pulses, not a single held level) can toggle the commanded
// intensity on and off faster than once per 30ms; since the Xbox/generic rumble bridge only
// resends on a detected amplitude *change* and arms a ~10-minute hardware sustain window per
// xpadneo's own documented convention (see btstack-implementation.md's "loop_count" section),
// any brief "off" dip that this 30ms sampling interval happens to skip over is never observed,
// so the physical motor just keeps coasting on its last-observed "on" trigger's sustain window --
// exactly matching the reported symptom, and explaining why it only appears with a real console's
// fast rumble traffic and not Steam's much sparser updates. Fixed with a dedicated, much faster
// timer (below) purely for this purpose -- deliberately NOT by shortening CONTROL_TICK_MS
// itself, since `control_tick`'s value is baked into every LED blink-rate calculation in
// `control_timer_handler` (heartbeat, pairing window, wipe flash, config mode, idle flash); redoing
// all of those divisors for a faster base tick would be needless regression risk for a symptom
// that's specific to rumble sampling, not LED timing.
#define RUMBLE_TICK_MS    3
// Discovery admission window. A modestly longer window than the original 10s
// is reasonable for usability (matched here); it is not the fix for the
// "pairing sometimes hangs" bug -- that was hid_state.state corruption in
// btstack_host_close_pairing_window(), independent of this constant's value.
// See docs/bluetooth/btstack-implementation.md "Pairing window vs in-flight connect".
#define PAIRING_WINDOW_MS 30000
#define WIPE_FLASH_MS     1200
// Must match usb.c's USB_MODE_ACK_MS -- core0 computes g_usb_mode_ack_until_ms
// as now+USB_MODE_ACK_MS and this file only needs the duration back to derive
// elapsed time for the flash pattern below; duplicated rather than shared via
// a header since it's a single cosmetic LED-timing constant, not shared logic.
#define USB_MODE_ACK_MS_FOR_LED 1200

static btstack_timer_source_t control_timer;
static btstack_timer_source_t rumble_timer;  // see RUMBLE_TICK_MS's own comment above
static uint32_t control_tick;
static uint32_t pairing_until_ms;  // 0 = locked; else scan window open until this time
static uint32_t wipe_until_ms;     // 0 = idle; else show the fast wipe flash until this time

#if defined(NS2_PRO) && defined(NS2_DIAG)
extern volatile uint8_t g_ns2_stage;  // Pro2 USB handshake progress (core0), blinked here
extern volatile uint8_t g_gc_stage;   // GameCube USB handshake progress (core0), blinked here --
                                       // deliberately a SEPARATE variable/blink path from
                                       // g_ns2_stage (not reused), per explicit instruction to
                                       // scope this diagnostic to GameCube mode only so the two
                                       // personalities' flash counts can never be confused with
                                       // each other -- see the blink logic below.
extern volatile uint8_t g_gc_bad_report_id;  // valid only when g_gc_stage==21 -- see switch_gc.h
#endif

static void open_pairing_window(uint32_t now_ms) {
    // Don't stomp an already-admitted candidate that's still finishing a
    // connection from a *previous* window (see btstack_host_close_pairing_window):
    // re-arming the scan here would overwrite hid_state.state out from under
    // its in-flight connect, exactly the bug this fix removed. Rare in
    // practice (would need a second BOOTSEL double-tap within the few-second
    // grace period right after a window's deadline), but cheap to guard.
    if (btstack_host_pairing_close_deferred()) {
        return;
    }
    bt_set_pairing_mode(true);  // start scanning for new controllers
    pairing_until_ms = now_ms + PAIRING_WINDOW_MS;
}

static void wipe_all_devices(void) {
    // Lock admission and erase trust first. The actual disconnects complete
    // asynchronously, so this prevents their completion events from reopening a
    // reconnect path before the wipe has taken effect.
    btstack_host_delete_all_bonds();
    btstack_host_disconnect_all_devices();
}

// Advance the gesture state machine and dispatch a completed gesture. This is
// intentionally separate from the 30 ms control timer: sustained Classic HID
// traffic can delay that timer, but the BOOTSEL sample itself is now serviced
// at report boundaries. Polling here as well lets the corresponding edges and
// deadlines be observed in the same traffic-driven path.
static void service_bootsel_gestures(uint32_t now) {
    // Pairing/wipe remain suppressed in config mode, while NS2_PRO's hold is
    // allowed there so the personality cycle can wrap from Config to Pro2.
    bool in_config = g_usb_config_mode;
    bootsel_gesture_t gesture = bootsel_poll(now);
    switch (gesture) {
        case BOOTSEL_DOUBLE_TAP:
            if (!in_config) open_pairing_window(now);
            break;
        case BOOTSEL_TRIPLE_TAP:
            if (!in_config) {
                pairing_until_ms = 0;
                // Consume any input edge before the asynchronous disconnect
                // frees the radio. A wipe gesture is maintenance, never wake
                // intent, even while the console is already asleep.
                ns2_wake_set_input_suppressed(true);
                wipe_all_devices();
                wipe_until_ms = now + WIPE_FLASH_MS;
            }
            break;
        case BOOTSEL_HOLD:
#ifdef NS2_PRO
            // Core0 owns the actual USB transition; core1 only requests it.
            g_usb_mode_cycle_requested = true;
#else
            if (!in_config) g_usb_enter_config = true;
#endif
            break;
        case BOOTSEL_NONE:
        default:
            break;
    }
}

// Dedicated fast poll purely so `gamepad_task()` (and any other driver `.task()`) observes
// rumble-state changes at a cadence that can keep up with a real console's ~4ms GC rumble
// traffic -- see RUMBLE_TICK_MS's own comment for the full root-cause account. Intentionally
// does nothing else (no LED/BOOTSEL/pairing logic here) so it stays cheap to run this often.
static void rumble_timer_handler(btstack_timer_source_t *ts) {
    // Cooperative SRAM park point for core0's BOOTSEL sample. Unlike the old
    // 200 us multicore-lockout IRQ deadline, this is guaranteed to be observed
    // on the next timer callback even under high-rate DualSense Classic traffic.
    bootsel_core1_service();
    bthid_task();
    btstack_run_loop_set_timer(ts, RUMBLE_TICK_MS);
    btstack_run_loop_add_timer(ts);
}

// Called by bthid at the start of every inbound HID report. Classic Bluetooth
// traffic (most visibly DualSense/Edge) can keep BTstack busy enough to delay
// timer callbacks. Make that traffic drive all three jobs it was starving:
// BOOTSEL's cooperative park, BOOTSEL gesture recognition, and per-device tasks
// (including DS5's initial LED/output setup and live rumble forwarding). This
// runs on core1 in the same BTstack run-loop context as the timers; the timers
// remain the fallback while a controller is quiet or disconnected.
void bthid_on_report_boundary(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bootsel_core1_service();
    service_bootsel_gestures(now);
    ns2_wake_service(now);
    bthid_task();
}

static void control_timer_handler(btstack_timer_source_t *ts) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (wipe_until_ms && now >= wipe_until_ms) {
        wipe_until_ms = 0;
        ns2_wake_set_input_suppressed(false);
    }

    // Also poll from the ordinary 30 ms path for quiet/disconnected periods.
    service_bootsel_gestures(now);
    ns2_wake_service(now);

    // Pending settings flash-write (runs here on core1, parking core0).
    config_service_save();

    // Close an expired pairing window. If a candidate is mid-connection,
    // btstack_host_close_pairing_window() (invoked via bt_set_pairing_mode)
    // defers the actual close until that attempt resolves instead of
    // cancelling it outright -- see btstack_host.c. pairing_until_ms is
    // cleared either way; the LED's "pairing" blink below independently
    // checks the deferred flag so it keeps blinking through the grace period.
    if (pairing_until_ms && now >= pairing_until_ms) {
        bt_set_pairing_mode(false);
        pairing_until_ms = 0;
    }

    // Service the Bluetooth stack. `bt_task()` (via cyw43_transport_task()) already calls
    // bthid_task() once per invocation; the dedicated rumble_timer above now additionally drives
    // it every RUMBLE_TICK_MS, so no separate explicit call is needed here anymore (removed
    // 2026-07-14 -- see RUMBLE_TICK_MS's comment for why the extra cadence was added).
    bt_task();

    // LED state (priority: mode-cycle ack > config > wipe burst > pairing window >
    // connected > idle). The mode-cycle ack is highest priority and self-expiring
    // (~1.2s) -- a brief, explicit "you just changed something" signal takes
    // precedence over the ambient status patterns below it, then gets out of the
    // way. See docs/switch2-gc/usb-personality.md "Transition sequence" (LED ack
    // requirement) -- core0 (usb.c) only publishes WHEN/WHICH; core1 renders it
    // here since it already owns all LED GPIO access.
    bool led;
#ifdef NS2_PRO
    if (g_usb_mode_ack_until_ms && now < g_usb_mode_ack_until_ms) {
        // N short flashes, 150ms on/150ms off, then LED goes dark for the remainder of the ack
        // window. N = the personality's position in the cycle + 1 (1 Pro2 / 2 GameCube /
        // 3 Joy-Con2 Left / 4 Joy-Con2 Right / 5 Config) -- derived directly from the enum's
        // ordinal value (usb.h) rather than a hardcoded per-personality chain, so this stays
        // correct automatically if the cycle ever gains or loses a stage. No personality gets
        // special-cased "experimental" LED behavior -- Joy-Con2 L/R just occupy their ordinary
        // position in the same counting scheme every other personality already uses.
        int flashes = (int)g_usb_mode_ack_personality + 1;
        uint32_t elapsed = USB_MODE_ACK_MS_FOR_LED - (g_usb_mode_ack_until_ms - now);
        uint32_t slot = elapsed / 150;
        led = (slot % 2 == 0) && (slot < (uint32_t)(2 * flashes));
    } else
#endif
#if defined(NS2_PRO) && defined(NS2_DIAG)
    // Scoped to GameCube personality ONLY (2026-07-13), per explicit instruction: showing this
    // same N-flash diagnostic for Pro2 too (reading g_ns2_stage) risked the two personalities'
    // flash counts being confused with each other during a GameCube-focused hardware session --
    // g_ns2_stage and g_gc_stage are separate variables with independently-defined stage meanings
    // (see switch_gc.c's own comment for GameCube's stage numbers), and only one can ever be
    // showing at a time anyway since the two personalities are mutually exclusive, but making the
    // condition explicit here removes any doubt about which is currently driving the LED. Pro2
    // mode falls through to its normal (non-diagnostic) LED behavior below even with NS2_DIAG on.
    if (g_usb_personality == USB_PERSONALITY_NSO_GAMECUBE) {
        uint8_t st = g_gc_stage;
        if (st == 255) {
            led = true;  // full success sentinel (report selected, streaming armed) -- solid on,
                         // deliberately distinct from the N-flash counting scheme below so it can
                         // never be mistaken for "9 or more commands received but still not armed"
        } else if (st == 0) {
            led = (control_tick / 25) % 2 == 0;  // ~0.75 s heartbeat = waiting
        } else if (st == 21) {
            // Two-digit tens/ones burst encoding g_gc_bad_report_id, instead of the flat N-flash
            // scheme below -- lets an arbitrary byte value (not just a small stage number) be
            // read directly off the LED: count the first burst (tens), a ~1s gap, then the
            // second burst (ones); a burst of 0 is simply no flashes for that digit.
            uint8_t val = g_gc_bad_report_id;
            uint32_t tens_ticks = (uint32_t)(val / 10) * 10;
            uint32_t gap_ticks = 33;
            uint32_t ones_ticks = (uint32_t)(val % 10) * 10;
            uint32_t tail_ticks = 60;
            uint32_t cycle = tens_ticks + gap_ticks + ones_ticks + tail_ticks;
            uint32_t pos = control_tick % cycle;
            if (pos < tens_ticks) {
                led = (pos % 10) < 5;
            } else if (pos < tens_ticks + gap_ticks) {
                led = false;
            } else if (pos < tens_ticks + gap_ticks + ones_ticks) {
                led = ((pos - tens_ticks - gap_ticks) % 10) < 5;
            } else {
                led = false;
            }
        } else {
            uint32_t per = 50, cycle = (uint32_t)st * per + 333, pos = control_tick % cycle;
            led = (pos < (uint32_t)st * per) && ((pos % per) < 10);  // N flashes, 1.5 s apart
        }
    } else
#endif
    if (g_usb_config_mode)
        led = (control_tick / 16) % 2 == 0;  // steady ~1 s blink = config mode
    else if (wipe_until_ms && now < wipe_until_ms)
        led = (control_tick & 1);  // very fast flash = erasing pairings
    else if (pairing_until_ms || btstack_host_pairing_close_deferred())
        led = (control_tick / 4) % 2 == 0;  // fast blink = pairing window (incl. in-flight-candidate grace period)
    else if (bt_get_connection_count() > 0)
        led = true;  // solid = controller connected
    else
        led = (control_tick % 66) < 3;  // brief flash every ~2 s = idle
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);

    control_tick++;
    btstack_run_loop_set_timer(ts, CONTROL_TICK_MS);
    btstack_run_loop_add_timer(ts);
}

// core1 entry (launched from main.c under BT_STACK_JOYPAD).
void ns2_bt_core_task(void) {
    // Arm the cooperative core1 SRAM park used by core0's BOOTSEL sampler.
    bootsel_core1_lockout_init();

    // Register the HID drivers first, then bring up BTstack + the HID host. The
    // CYW43 transport's init() performs cyw43_arch_init + btstack_cyw43_init and
    // powers on the controller, so no separate cyw43_arch_init() is needed here.
    bthid_registry_init();
    bt_init(&bt_transport_cyw43);

    btstack_run_loop_set_timer_handler(&control_timer, control_timer_handler);
    btstack_run_loop_set_timer(&control_timer, CONTROL_TICK_MS);
    btstack_run_loop_add_timer(&control_timer);

    btstack_run_loop_set_timer_handler(&rumble_timer, rumble_timer_handler);
    btstack_run_loop_set_timer(&rumble_timer, RUMBLE_TICK_MS);
    btstack_run_loop_add_timer(&rumble_timer);

    btstack_run_loop_execute();  // does not return
}
