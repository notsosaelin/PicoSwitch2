// ns2_seam.c — the PicoSwitch2 side of the joypad-os bthid stack.
//
// The vendored bthid drivers parse each controller's raw HID into joypad-os's
// unified input_event_t and hand it to a small framework (router / players /
// feedback / flash-bonds). PicoSwitch2 does NOT use that framework — it has its
// own cross-core seam (switch_pro_input_t via report.c). So instead of vendoring
// joypad-os's core services, we implement the exact five framework entry points
// the drivers call, bridging them to our seam. This file replaces bluepad32's
// fill_input()/forward_rumble() for the BT_STACK_JOYPAD build.
//
// Only compiled under -DBT_STACK_JOYPAD (see CMakeLists.txt / src/bt_hid/README.md).

#include <string.h>

#include "core/router/router.h"                 // input_event_t + router_submit_input()
#include "core/buttons.h"                        // JP_BUTTON_*
#include "core/services/players/feedback.h"      // feedback_state_t
#include "core/services/storage/flash.h"         // flash_on_bt_disconnect()

#include "report.h"                              // set_global_gamepad_input(), report_get_rumble()
#include "ns2_active_input.h"                    // one authoritative source gate
#include "switch_pro.h"                           // switch_pro_input_t, SWITCH_MASK_*, pack_stick
#include "bt/bthid/bthid.h"                       // bthid_get_device() — connected controller identity
#include "bt/bthid/devices/vendors/sony/ds5_bt.h" // exact decoder provenance (not late SDP PID)
#include "bt/bthid/devices/vendors/nintendo/wiimote_bt.h" // Wii motion provenance
#include "bt/bthid/devices/vendors/nintendo/switch_pro_bt.h" // Switch 1 motion provenance
#include "config.h"                               // config_get_body_color()
#include "ns2_motion_seam.h"                  // per-source IMU axis rows
#ifdef NS2_PRO
#include "switch_pro2.h"                          // ns2_motion_negotiated() (motion-demand hook)
#endif
#include "ns2_remap.h"                            // locked base button mapping
#include "ns2_player_led.h"                       // Switch wire bitfield -> player number
#include "ns2_wake.h"                             // post-sleep real-input wake intent
#include "platform/platform.h"                    // platform_time_ms()
#ifdef NS2_PRO
#include "usb.h"                                  // g_usb_personality (read-only cross-core check,
                                                   // GameCube-mode analog-fold suppression only)
#endif

#define NS2_SLOTS SWITCH_PRO_MAX_CONTROLLERS
#define NS2_WAKE_SESSION_SOURCES 8
static bool wake_session_active[NS2_WAKE_SESSION_SOURCES];

// bthid conn_index (0..N) arrives as dev_addr; map it to an output slot.
static inline uint8_t ns2_slot(uint8_t dev_addr) {
    return (dev_addr < NS2_SLOTS) ? dev_addr : 0;
}

// 0-255 (center 128) -> 12-bit (center 2048). Piecewise linear, not a single `v*4095/255` scale:
// that single-scale formula treats 0-255 as a plain linear range, but the source convention
// (every bthid driver, `input_event_t.analog[]`) is "128 is the nominal rest/center value", and
// 128 isn't the exact midpoint of 0..255 (127.5 is) -- so `128*4095/255` truncates to 2055, not
// 2048, and the inverted Y axis (4095 - ns2_to12(v)) rests at 2040, leaving X and Y resting
// 7-8 units off center in opposite directions. Confirmed 2026-07-14 as the cause of "stick
// mapping slightly off" reported when calibrating in Steam (its calibration screen is sensitive
// to exactly this kind of small, asymmetric center-point bias). Fixed by scaling each half of the
// range (0..128 and 128..255) independently so v=0/128/255 map to exactly 0/2048/4095 -- the two
// halves have very slightly different granularity (128 steps vs 127 steps), imperceptible for an
// analog stick and the standard way to handle an odd-centered source range.
static inline uint16_t ns2_to12(uint8_t v) {
    if (v == 128) return SWITCH_STICK_MID;
    if (v < 128)
        return (uint16_t)(((uint32_t)v * SWITCH_STICK_MID) / 128u);
    return (uint16_t)(SWITCH_STICK_MID +
                       (((uint32_t)(v - 128) * (SWITCH_STICK_MAX - SWITCH_STICK_MID)) / (255u - 128u)));
}

static inline int16_t ns2_clamp16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

// Source order: index -> JP button. MUST match NS2_BASE_BUTTON_MAP.
// (analog L2/R2 are folded into JP_BUTTON_L2/R2 below.)
static const uint32_t SRC_TO_JP[NS2_SRC_COUNT] = {
    JP_BUTTON_B1, JP_BUTTON_B2, JP_BUTTON_B3, JP_BUTTON_B4,
    JP_BUTTON_L1, JP_BUTTON_R1, JP_BUTTON_L2, JP_BUTTON_R2,
    JP_BUTTON_S1, JP_BUTTON_S2, JP_BUTTON_L3, JP_BUTTON_R3,
    JP_BUTTON_DU, JP_BUTTON_DD, JP_BUTTON_DL, JP_BUTTON_DR,
    JP_BUTTON_A1, JP_BUTTON_A2, JP_BUTTON_A3, JP_BUTTON_A4,
    JP_BUTTON_L4, JP_BUTTON_R4, JP_BUTTON_A5, JP_BUTTON_L5, JP_BUTTON_R5,
};

// Apply one locked base-map destination to the shared Pro Controller semantic
// input. Output personalities may translate those semantics further (for
// example, the single-Joy-Con sideways profile).
static void ns2_apply_dst(uint8_t dst, switch_pro_input_t *in) {
    switch (dst) {
        case NS2_DST_B:       in->buttons[0] |= SWITCH_MASK_B; break;
        case NS2_DST_A:       in->buttons[0] |= SWITCH_MASK_A; break;
        case NS2_DST_Y:       in->buttons[0] |= SWITCH_MASK_Y; break;
        case NS2_DST_X:       in->buttons[0] |= SWITCH_MASK_X; break;
        case NS2_DST_L:       in->buttons[2] |= SWITCH_MASK_L; break;
        case NS2_DST_R:       in->buttons[0] |= SWITCH_MASK_R; break;
        case NS2_DST_ZL:      in->buttons[2] |= SWITCH_MASK_ZL; break;
        case NS2_DST_ZR:      in->buttons[0] |= SWITCH_MASK_ZR; break;
        case NS2_DST_L3:      in->buttons[1] |= SWITCH_MASK_L3; break;
        case NS2_DST_R3:      in->buttons[1] |= SWITCH_MASK_R3; break;
        case NS2_DST_MINUS:   in->buttons[1] |= SWITCH_MASK_MINUS; break;
        case NS2_DST_PLUS:    in->buttons[1] |= SWITCH_MASK_PLUS; break;
        case NS2_DST_HOME:    in->buttons[1] |= SWITCH_MASK_HOME; break;
        case NS2_DST_CAPTURE: in->buttons[1] |= SWITCH_MASK_CAPTURE; break;
        case NS2_DST_DUP:     in->buttons[2] |= SWITCH_MASK_DPAD_UP; break;
        case NS2_DST_DDOWN:   in->buttons[2] |= SWITCH_MASK_DPAD_DOWN; break;
        case NS2_DST_DLEFT:   in->buttons[2] |= SWITCH_MASK_DPAD_LEFT; break;
        case NS2_DST_DRIGHT:  in->buttons[2] |= SWITCH_MASK_DPAD_RIGHT; break;
        case NS2_DST_GL:      in->extra |= SWITCH_EXTRA_GL; break;
        case NS2_DST_GR:      in->extra |= SWITCH_EXTRA_GR; break;
        case NS2_DST_C:       in->extra |= SWITCH_EXTRA_C; break;
        default: break;  // NS2_DST_NONE
    }
}

static bool ns2_is_switch2_pro(uint8_t dev_addr) {
    const bthid_device_t *dev = bthid_get_device(dev_addr);
    return dev && dev->vendor_id == 0x057E && dev->product_id == 0x2069;
}

// -------------------------------------------------------------------------
// Router: the ONE call every input driver makes. Translate the unified event
// into the Pro Controller wire format (via the locked base map) and publish it.
// -------------------------------------------------------------------------
void router_submit_input(const input_event_t *e) {
    if (!e) return;
    ns2_input_route_decision_t route;
    // Do this before decoding/publishing any field.  An inactive source must
    // not affect slot 0, identity, raw buttons, wake, motion, or mouse state.
    if (!ns2_active_input_submit(e, &route))
        return;
    switch_pro_input_t in;
    memset(&in, 0, sizeof(in));
    in.battery_level = e->battery_level;
    in.battery_valid = e->battery_source != INPUT_BATTERY_NONE;
    in.battery_charging = e->battery_charging;
    if (e->joycon_side == INPUT_JOYCON_SIDE_LEFT)
        in.source_joycon_side = SWITCH_SOURCE_JOYCON_LEFT;
    else if (e->joycon_side == INPUT_JOYCON_SIDE_RIGHT)
        in.source_joycon_side = SWITCH_SOURCE_JOYCON_RIGHT;

    // Digital buttons, plus analog triggers folded in as L2/R2: Switch ZL/ZR are digital,
    // but Xbox/DualSense triggers are analog and often don't set the digital JP bit.
    // Skipped when the driver sets suppress_l2r2_analog_fold (e.g. the 8BitDo NGC Modkit,
    // which has a real analog trigger it wants preserved AND has deliberately repurposed
    // JP_BUTTON_L2/R2 for something unrelated to the trigger itself -- see input_event.h).
    //
    // Also skipped whenever the active USB output personality is NSO GameCube, for EVERY
    // device, not just ones with a confirmed native GameCube layout: that personality's L/R are
    // a real continuous analog trigger (already forwarded unconditionally below) with an
    // independent digital detent, never a Pro2-style "any real press -> ZL/ZR" digital
    // approximation -- see the generic-controller block further down for what GC mode does
    // instead with a device that has no native layout of its own. This does NOT change Pro2's
    // own behavior -- Pro2 mode still gets its existing fold unchanged, since g_usb_personality
    // is only ever NSO_GAMECUBE when that personality is actually active.
#ifdef NS2_PRO
    bool gc_active = (g_usb_personality == USB_PERSONALITY_NSO_GAMECUBE);
#else
    bool gc_active = false;
#endif
    uint32_t b = e->buttons;
    if (!e->suppress_l2r2_analog_fold && !gc_active) {
        if (e->analog[ANALOG_L2] > 64) b |= JP_BUTTON_L2;
        if (e->analog[ANALOG_R2] > 64) b |= JP_BUTTON_R2;
    }

    // NSO GameCube-only fields: forwarded unconditionally regardless of active output
    // personality (Pro2/Switch 1 encoders never read switch_pro_input_t's gc_extra/
    // left_trigger/right_trigger). Placed before the Pro2-specific fold above per the
    // continuous-trigger-value preservation requirement -- these are the RAW values, never
    // thresholded or collapsed to booleans.
    in.left_trigger = e->analog[ANALOG_L2];
    in.right_trigger = e->analog[ANALOG_R2];
    in.gc_extra = 0;
    if (e->gc_native_zl) in.gc_extra |= GC_MASK_ZL;
    if (e->gc_native_z) in.gc_extra |= GC_MASK_Z;
    if (e->gc_l_detent) in.gc_extra |= GC_MASK_L_DETENT;
    if (e->gc_r_detent) in.gc_extra |= GC_MASK_R_DETENT;

    // GC-mode policy: generic shoulders become ZL/Z, while analog triggers supply native L/R and
    // a high-press detent. Native-layout devices bypass this synthesis. The provisional threshold
    // and physical test matrix live in docs/switch2-gc/mapping.md.
    if (gc_active && e->gc_has_native_layout) {
        // BattlerGC transports its physical trigger clicks in XInput's L3/R3
        // slots. They are the SAME physical actions as the native L/R trigger
        // detents, so aliasing them to Capture/C makes every full trigger click
        // fire two gameplay controls. The NSO GameCube output has no L3/R3:
        // discard those transported meanings and retain only analog L/R plus
        // gc_l/r_detent. Pro2 never enters this branch and keeps normal L3/R3.
        b &= ~(JP_BUTTON_L3 | JP_BUTTON_R3);
    } else if (gc_active && ns2_is_switch2_pro(e->dev_addr)) {
        // A real Pro Controller 2 already names these four controls L/R/ZL/ZR.
        // Preserve those names in the GC personality: its digital L/R buttons
        // synthesize a full trigger pull plus detent, while ZL/ZR become the
        // GC personality's ZL/Z buttons. The generic Xbox/DualSense policy
        // below intentionally remains unchanged.
        if (b & JP_BUTTON_L1) {
            in.left_trigger = 0xFF;
            in.gc_extra |= GC_MASK_L_DETENT;
        }
        if (b & JP_BUTTON_R1) {
            in.right_trigger = 0xFF;
            in.gc_extra |= GC_MASK_R_DETENT;
        }
        if (b & JP_BUTTON_L2) in.buttons[2] |= SWITCH_MASK_ZL;
        if (b & JP_BUTTON_R2) in.gc_extra |= GC_MASK_Z;
        b &= ~(JP_BUTTON_L1 | JP_BUTTON_R1 | JP_BUTTON_L2 | JP_BUTTON_R2);
    } else if (gc_active) {
        if (b & JP_BUTTON_L1) in.buttons[2] |= SWITCH_MASK_ZL;
        if (b & JP_BUTTON_R1) in.gc_extra |= GC_MASK_Z;
        if (e->analog[ANALOG_L2] > 224) in.gc_extra |= GC_MASK_L_DETENT;
        if (e->analog[ANALOG_R2] > 224) in.gc_extra |= GC_MASK_R_DETENT;
        // Real bug found via hardware feedback (2026-07-13): some pads (DualSense, several
        // 8BitDo tables -- not Xbox, whose own button map has no L2/R2 destination at all) have
        // a genuine native digital click bit for the trigger, separate from its analog axis and
        // from the seam's analog fold above (which is already suppressed for gc_active). That
        // native click bit still flowed through the base-map loop below into its
        // Pro2-appropriate legacy destination (NS2_DST_ZL/NS2_DST_ZR), stacking a second path on
        // top of this block's own shoulder->ZL/Z mapping. Only the left side was visible --
        // SWITCH_MASK_ZL has a live bit in the GC encoder, SWITCH_MASK_ZR does not -- which
        // produced exactly the reported asymmetry (an early spurious "ZL" partway through the
        // left trigger's travel, right trigger unaffected). Clear all four physical sources this
        // block already owns before the base-map loop runs, so nothing routes them a second time.
        b &= ~(JP_BUTTON_L1 | JP_BUTTON_R1 | JP_BUTTON_L2 | JP_BUTTON_R2);
    }

    // Apply the stable base mapping. Users can remap the emulated Nintendo
    // identity on the console, which then persists across physical controllers.
    uint8_t slot = ns2_slot(e->dev_addr);
    const bthid_device_t *dev = bthid_get_device(e->dev_addr);
    for (int src = 0; src < NS2_SRC_COUNT; src++) {
        if (b & SRC_TO_JP[src])
            ns2_apply_dst(NS2_BASE_BUTTON_MAP[src], &in);
    }

    // Sticks: 0-255 -> 12-bit; Y inverted (Switch is up-positive, HID is up=0).
    switch_pro_pack_stick(ns2_to12(e->analog[ANALOG_LX]),
                          (uint16_t)(4095 - ns2_to12(e->analog[ANALOG_LY])), in.left_stick);
    switch_pro_pack_stick(ns2_to12(e->analog[ANALOG_RX]),
                          (uint16_t)(4095 - ns2_to12(e->analog[ANALOG_RY])), in.right_stick);

    // IMU: per-source axis transform, see NS2_MOTION_SEAMS. The derivation below
    // is for the DualSense row.
    // accel /2 matches the genuine PC2 range (Experiment A). Gyro is passed ~1:1: the DualSense
    // raw gyro is already close to the Switch int16 gyro scale. The old /64 collapsed it ~60x
    // (Experiment A: genuine gyro peaked 7401 LSB, ours only 122) -> imperceptible in Steam.
    // (Report 0x05 consumes these directly; report 0x09's int32 phase/Q16.16 rewrite is separate.)
    //
    // Axis permutation — corrected from a genuine-controller report-0x05 capture
    // (usbpcaptures/genuine_procon_2.pcapng, Experiment A's golden trace). That capture's
    // "still, then rotate pitch/yaw/roll in turn" protocol lets the raw gyro channels be
    // identified: the genuine device's raw gyro **X** is the long, clean, first-rotated axis
    // (=pitch, by capture order) — confirmed independently by accel: during that segment accel-X
    // stays near its resting value while accel-Y/Z swing widely, i.e. X is the physical rotation
    // axis, consistent with pitch about a roughly horizontal axis. Raw gyro **Z** is the second
    // segment (=yaw) — this is the channel our *old* mapping already fed from DS5 yaw
    // (e->gyro[1]), which matches the hardware report "yaw appears mostly correct"
    // (SESSION.md 2026-07-10). By elimination raw gyro **Y** = roll, which the old mapping fed
    // from DS5 pitch (e->gyro[0]) instead of DS5 roll (e->gyro[2]) — the "pitch/roll appear
    // incorrect" symptom. Fix: route DS5 pitch (gyro[0]/accel[0]) to output X and DS5 roll
    // (gyro[2]/accel[2]) to output Y, keep DS5 yaw (gyro[1]/accel[1]) on output Z unchanged.
    // The old X/Y formulas are reused (not re-derived) so this is a **row swap**, which on its
    // Paired native-Pro2/DualSense capture on 2026-07-22 resolves the remaining
    // signs: during the same physical pitch, DS5 gyro-X and native state-0 G0
    // increase together. Gravity simultaneously maps DS5 [X,Y,Z] to Pro2
    // [X,-Z,Y]. Apply that same proper rotation (determinant +1) to gyro and
    // accel; the former [-X,+Z,+Y] mapping inverted pitch and roll and made
    // the console's gravity correction bleed one axis into another.
    if (e->has_motion) {
        in.has_motion = 1;
        // Driver identity is already authoritative when a report is decoded,
        // unlike Bluetooth SDP product_id which may still be zero. Comparing
        // the bound decoder keeps DS4 and other Sony layouts out without
        // expanding or changing input_event_t.
        // Provenance comes from the bound decoder, never from SDP identity.
        if (dev && dev->driver == &ds5_bt_driver)
            in.motion_source = SWITCH_MOTION_SOURCE_DUALSENSE;
        else if (dev && dev->driver == &wiimote_bt_driver)
            in.motion_source = SWITCH_MOTION_SOURCE_WII;
        else if (dev && dev->driver == &switch_pro_bt_driver)
            in.motion_source = SWITCH_MOTION_SOURCE_SWITCH1;
        else if (e->motion_from_android_bridge)
            // The Android bridge rides the ordinary generic gamepad driver, so
            // the bound decoder cannot identify it. The generic driver sets this
            // flag only when the source's own descriptor declared the bridge
            // extension, which is what gives this family its own seam row.
            in.motion_source = SWITCH_MOTION_SOURCE_ANDROID;
        else
            in.motion_source = SWITCH_MOTION_SOURCE_GENERIC;
        in.motion_sequence = e->motion_sequence;
        if (in.motion_source == SWITCH_MOTION_SOURCE_DUALSENSE &&
            e->motion_timestamp_valid) {
            in.motion_timestamp = e->motion_timestamp;
            in.motion_timestamp_valid = 1;
        }
        ns2_motion_seam_apply(in.motion_source, e->accel, e->gyro,
                              in.accel, in.gyro);
    }
#ifdef NS2_DS5_AUDIO
    in.headset_state = e->headset_state;
#endif

    // Publish identity before input so core0 can make source-specific policy decisions on the
    // new state (notably native Pro2 motion ownership) without one report of stale identity.
    if (dev)
        set_global_device(slot, dev->name, dev->vendor_id, dev->product_id);

    if (e->type == INPUT_TYPE_MOUSE) {
        in.has_mouse = 1;
        in.mouse_delta_x = e->delta_x;
        in.mouse_delta_y = e->delta_y;
        in.mouse_delta_wheel = e->delta_wheel;
        accumulate_global_mouse_input(slot, &in);
    } else {
        set_global_gamepad_input(slot, &in);
    }
    set_global_raw_buttons(slot, b);  // b includes analog L2/R2, for the live view
    uint8_t wake_source = e->dev_addr < NS2_WAKE_SESSION_SOURCES ? e->dev_addr : 0;
    if (!wake_session_active[wake_source]) {
        ns2_wake_controller_session_started(wake_source);
        wake_session_active[wake_source] = true;
    }
    // A neutral reconnect after the dock's sleep power-cycle is not user
    // intent. Only an actual controller button report may arm automatic wake.
    // Some controller protocols briefly route startup reports while changing
    // input modes. Keep their gameplay state live, but repeatedly discard wake
    // intent until the driver declares the stream stable.
    if (e->suppress_wake_input) {
        ns2_wake_controller_rebaseline(wake_source);
    } else {
        ns2_wake_note_controller_input(wake_source, b != 0, platform_time_ms());
    }
}

// Controller dropped -> publish a neutral (centered, no buttons) state.
void router_device_disconnected(uint8_t dev_addr, int8_t instance) {
    router_device_disconnected_with_generation(dev_addr, instance, 0u);
}

void router_device_disconnected_with_generation(uint8_t dev_addr,
                                                int8_t instance,
                                                uint32_t connection_generation) {
    // Driver disconnect callbacks are also reused while bthid swaps parser
    // implementations on one physical link.  bthid brackets those callbacks
    // with an explicit rebind lifecycle; only an actual transport teardown may
    // revoke the active source here.
    if (bthid_rebind_in_progress())
        return;

    bool was_active = ns2_active_input_disconnected_generation(
        dev_addr, instance, connection_generation);
    // Inactive disconnects are intentionally invisible to the console seam.
    // In particular, do not clear a slot that a recycled connection index may
    // now represent.
    if (!was_active)
        return;
    uint8_t wake_source = dev_addr < NS2_WAKE_SESSION_SOURCES ? dev_addr : 0;
    if (wake_session_active[wake_source]) {
        wake_session_active[wake_source] = false;
        ns2_wake_controller_session_ended(wake_source);
    }
}

// Raw HID report passthrough for config mode's debug view (overrides bthid.c's weak
// default). Lets us reverse-engineer inputs a driver doesn't parse yet (Elite paddles).
void bthid_on_raw_report(uint8_t conn_index, uint32_t connection_generation,
                         const uint8_t *data, uint16_t len) {
    ns2_active_input_note_connection(conn_index);
    if (ns2_active_input_connection_is_active_generation(
            conn_index, connection_generation))
        set_global_raw_report(0, data, len);
}

void bthid_on_hid_rebind(uint8_t conn_index) {
    ns2_active_input_rebind(conn_index);
}

// Strong override of the generic driver's weak motion-demand hook. A source that
// can idle its sensors (the Android companion handheld) asks the adapter whether
// the console is actually consuming motion; streaming gyro into a report that
// carries none would drain a phone battery for nothing. On NS2_PRO this is the
// console's real negotiated IMU state; other build axes keep the permissive
// default because they have no equivalent negotiation to consult.
bool bthid_host_wants_motion(void) {
#ifdef NS2_PRO
    return ns2_motion_negotiated();
#else
    return true;
#endif
}

// Strong override of bthid.c's weak battery hook. BAS notifications can arrive
// while the controller is otherwise idle, so republish its cached input state
// to make the new level visible without waiting for a button report.
void bthid_on_battery_update(input_event_t *event) {
    if (!event) return;
    // A BAS update republishes cached state but is not a new mouse packet.
    // Prevent it from replaying the last relative movement.
    int16_t dx = event->delta_x;
    int16_t dy = event->delta_y;
    int8_t wheel = event->delta_wheel;
    if (event->type == INPUT_TYPE_MOUSE) {
        event->delta_x = 0;
        event->delta_y = 0;
        event->delta_wheel = 0;
    }
    router_submit_input(event);
    event->delta_x = dx;
    event->delta_y = dy;
    event->delta_wheel = wheel;
}

// -------------------------------------------------------------------------
// Feedback: drivers poll feedback_get_state() to learn what rumble to send to
// the physical controller. Bridge it to the console-decoded per-motor amplitudes
// the USB core publishes via report_set_rumble() — forwarded independently (not
// duplicated) so drivers with true per-motor output (DualSense, Xbox) preserve
// stereo separation instead of both motors buzzing identically.
// -------------------------------------------------------------------------
static feedback_state_t s_fb[NS2_SLOTS];
// Last report.c rumble generation this consumer has actually observed -- kept separate from
// feedback_state_t (a vendored joypad-os struct, not something this seam should extend) rather
// than added as a new field there. See report_get_rumble_gen()'s own comment
// (include/report.h) for why generation-based change detection replaces plain value comparison:
// Confirmed 2026-07-14 that comparing raw left/right values alone cannot tell "nothing happened"
// apart from "it changed and changed back before this was last polled" -- both look identical
// once the value returns to what was last seen, which could silently drop a real stop transition
// on a downstream driver with a long hardware sustain (e.g. Xbox's ~10-minute pulse_sustain_10ms/
// loop_count in bthid_gamepad.c).
static uint32_t s_rumble_gen_seen[NS2_SLOTS];
static uint32_t s_player_led_gen_seen[NS2_SLOTS];

feedback_state_t *feedback_get_state(uint8_t player_index) {
    uint8_t p = (player_index < NS2_SLOTS) ? player_index : 0;
    uint8_t left, right;
    uint32_t gen;
    report_get_rumble_gen(p, &left, &right, &gen);
    if (gen != s_rumble_gen_seen[p]) {
        s_fb[p].rumble.left = left;
        s_fb[p].rumble.right = right;
        s_fb[p].rumble_dirty = true;
        s_rumble_gen_seen[p] = gen;
    }
    uint8_t player_wire;
    report_get_player_leds(p, &player_wire, &gen);
    if (gen != s_player_led_gen_seen[p]) {
        uint8_t player_number;
        if (ns2_player_led_decode(player_wire, &player_number)) {
            // feedback.pattern uses one bit per player, unlike Nintendo's
            // cumulative 1/3/7/F wire convention.
            s_fb[p].led.pattern = (uint8_t)(1u << (player_number - 1u));
            s_fb[p].led_dirty = true;
        }
        s_player_led_gen_seen[p] = gen;
    }
    // Match supported physical RGB lights to the appearance advertised by the
    // active output personality. Player-indicator patterns remain independent.
    uint8_t rgb[3];
#ifdef NS2_PRO
    if (g_usb_personality == USB_PERSONALITY_JOYCON2_L)
        config_get_joycon2_accent(false, rgb);
    else if (g_usb_personality == USB_PERSONALITY_JOYCON2_R)
        config_get_joycon2_accent(true, rgb);
    else
        config_get_body_color(rgb);  // Pro2 plus GC/config fallback
#else
    config_get_body_color(rgb);
#endif
    if (rgb[0] != s_fb[p].led.r || rgb[1] != s_fb[p].led.g || rgb[2] != s_fb[p].led.b) {
        s_fb[p].led.r = rgb[0];
        s_fb[p].led.g = rgb[1];
        s_fb[p].led.b = rgb[2];
        s_fb[p].led_dirty = true;
    }
    return &s_fb[p];
}

void feedback_clear_dirty(uint8_t player_index) {
    if (player_index >= NS2_SLOTS) return;
    s_fb[player_index].rumble_dirty = false;
    s_fb[player_index].led_dirty = false;
    s_fb[player_index].triggers_dirty = false;
}

// Bluetooth bond storage is handled by BTstack's own TLV store; nothing to flush.
void flash_on_bt_disconnect(void) {}

// -------------------------------------------------------------------------
// Minimal stand-ins for joypad-os framework symbols the vendored drivers still
// reference but PicoSwitch2 does not use (single controller; no player manager).
// -------------------------------------------------------------------------
int find_player_index(int dev_addr, int instance) {
    (void)instance;
    // Drivers use this hook before polling rumble/LED state.  Returning -1
    // for an inactive source gates the drivers that honor the framework's
    // negative result, while preserving slot-0 behavior for the selected
    // source.  A few legacy vendor init paths still fall back to slot 0; those
    // remain an explicit follow-up until feedback becomes source-aware.
    if (dev_addr < 0 || !ns2_active_input_connection_is_active((uint8_t)dev_addr))
        return -1;
    // Real, previously-latent bug (found 2026-07-12 tracing a hardware-reported rumble
    // regression): this used to pass `dev_addr` (actually the caller's BTstack connection
    // index — see every call site, all pass `event.dev_addr`/`conn_index`, never a real BT
    // address) straight through whenever it happened to be < NS2_SLOTS (4, from
    // SWITCH_PRO_MAX_CONTROLLERS, unconditional regardless of NS2_PRO). BLE connections are
    // safe by accident (their conn_index is offset by BLE_CONN_INDEX_OFFSET, always >=
    // NS2_SLOTS, so they always fell through to the 0 fallback) — but a *Classic* BT device
    // landing in connection slot 1, 2, or 3 (btstack_host.c's find_free_classic_connection(),
    // which is a real 4-slot allocator, not something this seam controls) got back that same
    // nonzero index as its "player index," and every driver's feedback_get_state()/
    // feedback_clear_dirty() call used it verbatim — while `ns2_hid_out_report()` (this
    // project's single-controller NS2 milestone) always publishes rumble to hardcoded slot 0.
    // Any Classic device outside slot 0 was reading/clearing a feedback slot that never
    // received anything: dirty always false, rumble always (0,0), silently. This affected
    // every driver identically (Sony/Microsoft/Nintendo/generic/Stadia all call this the same
    // way) — a real, evidenced explanation for a rumble regression reported across multiple
    // independent controller families, and one that predates and is unrelated to the L/R
    // stereo-rumble refactor (a mono value published to slot 0 would have been just as
    // invisible to a driver reading a different slot). This project has one output identity
    // (single-controller milestone, matching this function's own pre-existing comment) — so
    // always resolving to slot 0 is not a workaround, it's what this stand-in already claimed
    // to do and didn't.
    (void)dev_addr;
    return 0;
}

void remove_players_by_address(int dev_addr, int instance) {
    (void)dev_addr;
    (void)instance;
}

// Called by btstack_host on a fatal BT error — no automatic reboot for now.
void platform_reboot(void) {}

#include <pico/time.h>
// Milliseconds since boot (platform HAL — used by the Sony drivers' timers).
uint32_t platform_time_ms(void) { return to_ms_since_boot(get_absolute_time()); }

// Player-indicator LED patterns (index 0 = none; values per feedback.h
// FEEDBACK_LED_PLAYER*). Sony drivers use this only for player indicators; RGB
// lightbars independently follow the active personality's configured appearance.
const uint8_t PLAYER_LEDS[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x09, 0x0A, 0x0C, 0x0F, 0x0F, 0x0F};

// Microseconds since boot (platform HAL — Nintendo/Wiimote driver timers).
uint32_t platform_time_us(void) { return (uint32_t)to_us_since_boot(get_absolute_time()); }

// We only bridge gamepads to the Switch, so there is no keyboard->button mapping.
uint32_t keymap_keys_to_buttons(const uint8_t *keys, uint8_t nkeys, uint8_t modifier) {
    (void)keys; (void)nkeys; (void)modifier;
    return 0;
}

// Per-device persisted settings (e.g. Wiimote calibration). Not stored on our side —
// report "none" so drivers use defaults; save is a no-op.
bool flash_load(flash_t *settings) { (void)settings; return false; }
void flash_save(const flash_t *settings) { (void)settings; }

// The Phase-0 build compiles only the generic driver, but bthid.c's DualShock
// report-ID reclassification references the Sony driver structs. Provide empty
// stand-ins (never activated — no DualShock connects without the vendor drivers).
// Defining NS2_BT_ALL_DRIVERS compiles the real drivers and drops these.
#if !defined(NS2_BT_ALL_DRIVERS) && !defined(NS2_BT_SONY)
#include "bt/bthid/bthid.h"  // bthid_driver_t
const bthid_driver_t ds3_bt_driver = {0};
const bthid_driver_t ds4_bt_driver = {0};
const bthid_driver_t ds5_bt_driver = {0};
#endif
