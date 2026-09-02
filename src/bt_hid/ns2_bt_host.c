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
#include "bt/bthid/devices/vendors/sony/ds5_bt.h"
#include "bt/btstack/btstack_host.h"

#include "bootsel.h"
#include "bootsel_action.h"
#include "config.h"
#include "ds5_audio_bridge.h"
#include "mgmt_pairing.h"
#include "ns2_kbm_runtime.h"
#include "ns2_owner_led.h"
#include "ns2_bt_recovery_runtime.h"
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
static btstack_timer_source_t control_timer;
static btstack_timer_source_t rumble_timer;  // see RUMBLE_TICK_MS's own comment above
#ifdef NS2_DS5_AUDIO
static btstack_timer_source_t audio_timer;
#define AUDIO_TICK_MS 2
#endif
static uint32_t pairing_until_ms;  // 0 = locked; else scan window open until this time
static bool owner_led_initialized;
static ns2_owner_led_reason_t owner_led_reason;
static uint16_t owner_led_detail;
static uint32_t owner_led_mode_started_ms;
static ns2_owner_led_output_state_t owner_led_output_state;
static uint32_t owner_led_last_timer_ms;
static uint32_t owner_led_timer_max_gap_ms;

// Bounded discovery for a partial KB/M source. Runtime only -- no persistence,
// no record of which peripherals exist; it tracks the CURRENT logical source
// lifecycle and nothing else. See ns2_kbm_completion_update().
static ns2_kbm_completion_t kbm_completion;
static bool pairing_wait_for_disconnect;
static uint32_t wipe_until_ms;     // 0 = idle; else show the fast wipe flash until this time

#if defined(NS2_PRO) && defined(NS2_DIAG)
extern volatile uint8_t g_gc_stage;   // GameCube USB handshake progress (core0), blinked here --
                                       // deliberately a SEPARATE variable/blink path from
                                       // g_ns2_stage (not reused), per explicit instruction to
                                       // scope this diagnostic to GameCube mode only so the two
                                       // personalities' flash counts can never be confused with
                                       // each other -- see the blink logic below.
extern volatile uint8_t g_gc_bad_report_id;  // valid only when g_gc_stage==21 -- see switch_gc.h
#endif

// One dongle serves one LOGICAL input source, which is not always one peer:
// Keyboard + Mouse mode fills two roles. The pairing window and the idle-scan
// rule must therefore ask "is the SELECTED source complete?", not "is anything
// connected?" -- otherwise pairing a keyboard would immediately close the
// window the mouse still needs, and in Keyboard mode a leftover connected
// gamepad would stop discovery before a keyboard could ever be found.
//
// Controller mode is byte-for-byte the pre-existing "a controller is HID-ready"
// test.
static bool logical_source_complete(void) {
    ns2_kbm_runtime_status_t status;
    ns2_kbm_runtime_status(&status);
    // The rule itself is pure and host-tested (see ns2_kbm_logical_source_complete
    // and tools/test_ns2_kbm.c): a premature "complete" permanently strands the
    // missing peer, so it is pinned by regression rather than restated here.
    return ns2_kbm_logical_source_complete(status.keyboard_connected,
                                           status.mouse_connected,
                                           btstack_host_controller_connected());
}

// True while any KB/M role is held. Such a source is assembled incrementally,
// which makes the historical "opening pairing replaces what is connected" rule
// wrong for it.
static bool kbm_source_present(void) {
    ns2_kbm_runtime_status_t status;
    ns2_kbm_runtime_status(&status);
    return status.keyboard_connected || status.mouse_connected;
}

/*
 * The one controller-pairing operation. Both triggers call it, so the radio
 * behaves identically either way; `grant_management_bonding` is the only
 * difference between them and it is passed explicitly rather than patched up
 * afterwards.
 *
 * true  -- the local BOOTSEL gesture. Someone is holding the adapter, and
 *          physical presence is the authority for handing out a NEW management
 *          relationship as well as a controller one.
 * false -- a management client asked over the air. It may open controller
 *          discovery and nothing else.
 */
static void open_pairing_window(uint32_t now_ms, bool grant_management_bonding) {
    // Don't stomp an already-admitted candidate that's still finishing a
    // connection from a *previous* window (see btstack_host_close_pairing_window):
    // re-arming the scan here would overwrite hid_state.state out from under
    // its in-flight connect, exactly the bug this fix removed. Rare in
    // practice (would need a second BOOTSEL double-tap within the few-second
    // grace period right after a window's deadline), but cheap to guard.
    if (btstack_host_pairing_close_deferred()) {
        return;
    }
    // Historical replacement semantics apply to a STANDALONE controller source
    // only: opening pairing with a gamepad connected still means "replace it".
    //
    // A KB/M composite is assembled role by role, so opening pairing while one
    // of its roles is connected means "add the other one" -- disconnecting what
    // is already there is exactly wrong, and is what made keyboard + mouse
    // impossible to establish. A KB/M role is replaced by powering its device
    // off (which frees the role) or by the pairing wipe gesture; that
    // limitation is deliberate, because the BOOTSEL gesture cannot say which
    // role the user means.
    pairing_wait_for_disconnect =
        !kbm_source_present() && btstack_host_controller_connected();
    if (pairing_wait_for_disconnect) {
        btstack_host_disconnect_all_devices();
    }
    bt_set_pairing_mode(true);  // controller discovery only
    // Ordered after, and in the same run-loop callback, so no security
    // procedure can observe the intermediate state.
    btstack_host_set_management_bond_window_open(grant_management_bonding);
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
    bool in_config = g_usb_config_mode;
    bootsel_gesture_t gesture = bootsel_poll(now);
    bootsel_action_t action = bootsel_action_resolve(
        gesture, in_config, btstack_host_controller_connected());
    switch (action) {
        case BOOTSEL_ACTION_CYCLE_CONTROLLER:
#ifdef NS2_PRO
            // Core0 owns the USB detach/reset/re-enumeration sequence.
            g_usb_mode_cycle_requested = true;
#endif
            break;
        case BOOTSEL_ACTION_OPEN_PAIRING:
            // Local gesture: the user is at the adapter, so this is the one
            // trigger allowed to admit a new management bond as well.
            open_pairing_window(now, true);
            break;
        case BOOTSEL_ACTION_WIPE_DEVICES:
            pairing_until_ms = 0;
            pairing_wait_for_disconnect = false;
            // Consume any input edge before the asynchronous disconnect frees
            // the radio. A wipe gesture is maintenance, never wake intent.
            ns2_wake_set_input_suppressed(true);
            wipe_all_devices();
            wipe_until_ms = now + WIPE_FLASH_MS;
            break;
        case BOOTSEL_ACTION_TOGGLE_CONFIG:
#ifdef NS2_PRO
            g_usb_config_mode_requested = true;
#else
            if (!in_config) g_usb_enter_config = true;
#endif
            break;
        case BOOTSEL_ACTION_NONE:
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
    // Mouse-to-stick recenter. A mouse that stops moving stops reporting, so
    // without a periodic tick the translated stick would hold its last
    // deflection indefinitely. Cheap early-out in every other mode.
    ns2_kbm_runtime_service();
    btstack_run_loop_set_timer(ts, RUMBLE_TICK_MS);
    btstack_run_loop_add_timer(ts);
}

#ifdef NS2_DS5_AUDIO
// Live Opus runs in core1's foreground worker, never from this background
// BTstack timer or bthid_on_report_boundary()'s nested receive stack. This
// callback only performs short maintenance and transports completed frames.
static void audio_timer_handler(btstack_timer_source_t *ts) {
    // Combined with the report-boundary sample below, this is a core-1
    // liveness heartbeat rather than a timer-punctuality-only measurement.
    ds5_audio_diag_note_core1_activity(time_us_32());
    ns2_bt_recovery_note_core1_activity(
        to_ms_since_boot(get_absolute_time()));
    bootsel_core1_service();
    ds5_bt_audio_service();
    // The genuine Pro Controller 2 transport interleaves Opus headphone audio
    // and sparse HD-haptic streams 5 ms apart on a 20 ms cycle. Keep its UART-gated replay on
    // this existing fast timer; btstack_host_process() runs only every 30 ms.
    btstack_host_service_switch2_pro2_audio_replay();
    btstack_host_service_switch2_pro2_audio_live();
    btstack_run_loop_set_timer(ts, AUDIO_TICK_MS);
    btstack_run_loop_add_timer(ts);
}
#endif

// Called by bthid at the start of every inbound HID report. Classic Bluetooth
// traffic (most visibly DualSense/Edge) can keep BTstack busy enough to delay
// timer callbacks. Make that traffic drive all three jobs it was starving:
// BOOTSEL's cooperative park, BOOTSEL gesture recognition, and per-device tasks
// (including DS5's initial LED/output setup and live rumble forwarding). This
// runs on core1 in the same BTstack run-loop context as the timers; the timers
// remain the fallback while a controller is quiet or disconnected.
void bthid_on_report_boundary(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    ns2_bt_recovery_note_core1_activity(now);
#ifdef NS2_DS5_AUDIO
    // If inbound HID traffic delays the audio timer while core 1 remains live,
    // report boundaries keep this heartbeat moving. A long gap therefore means
    // the BTstack core itself stopped making progress, not just timer starvation.
    ds5_audio_diag_note_core1_activity(time_us_32());
#endif
    bootsel_core1_service();
    service_bootsel_gestures(now);
    ns2_wake_service(now);
    bthid_task();
#ifdef NS2_DS5_AUDIO
    // DualSense Classic reports can keep the run loop busy enough to delay
    // timers. Drive the lightweight audio transport from the same proven
    // report-boundary safe point used for BOOTSEL and rumble. Live Opus encode
    // remains confined to the shallow audio timer.
    ds5_bt_audio_report_service();
#endif
}

static void control_timer_handler(btstack_timer_source_t *ts) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    ns2_bt_recovery_note_control_tick(now);

    if (owner_led_last_timer_ms != 0u) {
        uint32_t gap_ms = now - owner_led_last_timer_ms;
        if (gap_ms > owner_led_timer_max_gap_ms)
            owner_led_timer_max_gap_ms = gap_ms;
    }
    owner_led_last_timer_ms = now;

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
    // A management client asked to pair a controller. This is the ONLY place
    // that acts on it, and it calls the same open_pairing_window() the BOOTSEL
    // gesture calls, so the radio behaves identically whichever trigger started
    // it (design §32). The difference is authority, not behaviour: the remote
    // path opens controller discovery WITHOUT admitting a new management bond.
    if (btstack_host_pairing_take_remote_start()) {
        // Controller discovery only. Passed in rather than opened-then-
        // downgraded: the downgrade did not work, because the function it
        // called only ever clears the management window on CLOSE.
        open_pairing_window(now, false);
        if (pairing_until_ms) {
            btstack_host_pairing_note_state(MGMT_PAIRING_DISCOVERING,
                                            MGMT_PAIRING_REASON_NONE,
                                            pairing_until_ms);
        } else {
            // open_pairing_window() declined -- a previous candidate is still
            // finishing. Report it rather than leaving the client watching a
            // window that never opened.
            btstack_host_pairing_note_state(MGMT_PAIRING_BLOCKED,
                                            MGMT_PAIRING_REASON_BUSY, now);
        }
    }
    if (btstack_host_pairing_take_remote_cancel() && pairing_until_ms) {
        bt_set_pairing_mode(false);
        pairing_until_ms = 0;
        pairing_wait_for_disconnect = false;
        btstack_host_pairing_note_state(MGMT_PAIRING_CANCELLED,
                                        MGMT_PAIRING_REASON_NONE, now);
    }

    if (pairing_until_ms && now >= pairing_until_ms) {
        bt_set_pairing_mode(false);
        pairing_until_ms = 0;
        pairing_wait_for_disconnect = false;
        // Firmware owns the deadline. The app never has to send a cancel for
        // safety, and losing the app cannot leave the adapter discoverable
        // (design §34).
        if (btstack_host_pairing_active()) {
            btstack_host_pairing_note_state(MGMT_PAIRING_TIMED_OUT,
                                            MGMT_PAIRING_REASON_NO_CONTROLLER,
                                            now);
        }
    }

    // One dongle serves one controller: a controller finishing connection
    // fulfills an open pairing window's purpose. Close it immediately so the LED
    // goes solid (via the "connected" branch below) instead of blinking for the
    // rest of the 30 s window, and discovery stops. Keyed on hid_ready so it
    // never closes on a mid-handshake connection. bt_set_pairing_mode(false)
    // routes through close_pairing_window(), which defers if a BLE candidate is
    // still in flight -- see btstack_host.c.
    if (pairing_until_ms && pairing_wait_for_disconnect &&
        !btstack_host_controller_connected()) {
        pairing_wait_for_disconnect = false;
    }
    if (pairing_until_ms && !pairing_wait_for_disconnect &&
        logical_source_complete()) {
        bt_set_pairing_mode(false);
        pairing_until_ms = 0;
        // A controller completed during this window: success, whichever trigger
        // opened it. The bond is already persisted by the ordinary pairing
        // path; nothing extra is written here (design §39).
        if (btstack_host_pairing_active()) {
            btstack_host_pairing_note_state(MGMT_PAIRING_PAIRED,
                                            MGMT_PAIRING_REASON_NONE, now);
        }
    }

    // ---------------------------------------------------------------------
    // Discovery ownership
    // ---------------------------------------------------------------------
    // Discovery must be RUNNING, not merely "not stopped". Every BLE HID peer
    // that reaches ready calls btstack_host_stop_scan() unconditionally (the
    // legacy 1-dongle-1-controller rule, three call sites in btstack_host.c),
    // and the idle safety-net cannot restore it: its final term is
    // btstack_classic_get_connection_count() == 0, which counts BLE links
    // despite its name, so with any peer connected the predicate short-circuits
    // and never reaches start_scan(). Whatever wants discovery must therefore
    // re-arm it explicitly, every tick.
    //
    // There are TWO independent reasons discovery may be required, and neither
    // may suppress the other:
    //
    //   1. an explicit pairing window the user opened, while the selected
    //      source is still incomplete;
    //   2. the bounded completion window for a partial KB/M source.
    //
    // This decision used to sit inside `if (pairing_until_ms == 0)`, which meant
    // reason 2 could never run during a pairing window -- so the first peer to
    // finish connecting inside an explicit pairing window stopped the scan and
    // nothing re-armed it for the rest of that window. Hardware showed exactly
    // that: keyboard connected, source still partial, yet hid_state=0 /
    // scan_active=false with scan starts == stops.
    ns2_kbm_runtime_status_t discovery_status;
    ns2_kbm_runtime_status(&discovery_status);
    // "A controller is driving the console", not "a Bluetooth HID link exists".
    //
    // Path C's companion source is not a BT HID connection, so keying this on
    // btstack_host_controller_connected() alone left the owner LED idle while
    // the console was being driven at 125 Hz -- the adapter looked asleep with a
    // controller in the user's hands. Android goes solid once input is
    // confirmed, and this is the same statement on the same LED.
    const bool controller_ready = btstack_host_controller_connected() ||
                                  btstack_host_companion_link_streaming();
    const bool source_complete = ns2_kbm_logical_source_complete(
        discovery_status.keyboard_connected, discovery_status.mouse_connected,
        controller_ready);

    // Always advance the completion window, including during a pairing window,
    // so its notion of the current partial state cannot go stale. It is keyed to
    // logical-source transitions, so neither this tick rate nor any amount of
    // keyboard/mouse traffic can extend it.
    const ns2_kbm_discovery_t timed_discovery = ns2_kbm_completion_update(
        &kbm_completion, discovery_status.keyboard_connected,
        discovery_status.mouse_connected, controller_ready, now);

    // The matrix itself is pure and host-tested (ns2_kbm_discovery_policy), so it
    // is pinned by regression rather than restated here. Re-asserted every tick:
    // btstack_host_scan_for_additional_peer() is idempotent and returns early
    // when a scan or inquiry is already running, so this costs nothing in the
    // steady state and self-heals whichever path stopped the scan.
    switch (ns2_kbm_discovery_policy(pairing_until_ms != 0, source_complete,
                                     timed_discovery)) {
    case NS2_KBM_DISCOVERY_ARM:
        btstack_host_scan_for_additional_peer();
        break;
    case NS2_KBM_DISCOVERY_RETIRE:
        // Retired multi-controller scanning; frees Bluetooth bandwidth. The host
        // stays connectable, so a bonded controller still reconnects.
        btstack_host_idle_scan_if_connected();
        break;
    case NS2_KBM_DISCOVERY_LEAVE:
        break;   // an open pairing window owns the scan; it closes itself
    }

    // Service the Bluetooth stack. `bt_task()` (via cyw43_transport_task()) already calls
    // bthid_task() once per invocation; the dedicated rumble_timer above now additionally drives
    // it every RUMBLE_TICK_MS, so no separate explicit call is needed here anymore (removed
    // 2026-07-14 -- see RUMBLE_TICK_MS's comment for why the extra cadence was added).
    bt_task();

    // Owner LED policy is based on authoritative HID readiness, not raw ACL or
    // ATT slot occupancy. All cadences use elapsed milliseconds, so delayed
    // callbacks cannot compress the idle heartbeat into an apparent 2-3 s
    // second flash.
    ns2_owner_led_inputs_t led_inputs = {
        .config_mode = g_usb_config_mode,
        .wipe_active = wipe_until_ms && now < wipe_until_ms,
        .pairing_active = pairing_until_ms ||
                          btstack_host_pairing_close_deferred(),
        .controller_ready = controller_ready,
    };
    uint8_t flash_count = 0u;
    uint8_t gc_stage = 0u;
    uint8_t gc_bad_report_id = 0u;
#ifdef NS2_PRO
    led_inputs.mode_ack = g_usb_mode_ack_until_ms &&
                          now < g_usb_mode_ack_until_ms;
    flash_count = (uint8_t)g_usb_mode_ack_personality + 1u;
#endif
#if defined(NS2_PRO) && defined(NS2_DIAG)
    led_inputs.gc_diag =
        g_usb_personality == USB_PERSONALITY_NSO_GAMECUBE;
    gc_stage = g_gc_stage;
    gc_bad_report_id = g_gc_bad_report_id;
#endif

    ns2_owner_led_reason_t next_reason = ns2_owner_led_decide(led_inputs);
    uint16_t next_detail = 0u;
    if (next_reason == NS2_OWNER_LED_MODE_ACK)
        next_detail = flash_count;
    else if (next_reason == NS2_OWNER_LED_GC_DIAG)
        next_detail = (uint16_t)gc_stage << 8 | gc_bad_report_id;
    if (!owner_led_initialized || next_reason != owner_led_reason ||
        next_detail != owner_led_detail) {
        owner_led_initialized = true;
        owner_led_reason = next_reason;
        owner_led_detail = next_detail;
        owner_led_mode_started_ms = now;
    }

    bool led = ns2_owner_led_render(
        owner_led_reason, now - owner_led_mode_started_ms, flash_count,
        gc_stage, gc_bad_report_id);
    ns2_owner_led_track_output(&owner_led_output_state, led, now);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
    ns2_owner_led_diag_publish(owner_led_reason, led,
                               owner_led_output_state.last_transition_ms,
                               owner_led_timer_max_gap_ms);

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

#ifdef NS2_DS5_AUDIO
    btstack_run_loop_set_timer_handler(&audio_timer, audio_timer_handler);
    btstack_run_loop_set_timer(&audio_timer, AUDIO_TICK_MS);
    btstack_run_loop_add_timer(&audio_timer);
#endif
#ifdef NS2_DS5_AUDIO_LIVE_OPUS
    // The CYW43 threadsafe-background context owns BTstack on a core1 IRQ.
    // Its normal execute() foreground only sleeps. Give that foreground to
    // the blocking PCM/Opus worker while leaving all Bluetooth ownership here.
    ds5_audio_bridge_codec_worker();  // does not return
#else
    btstack_run_loop_execute();  // does not return
#endif
}
