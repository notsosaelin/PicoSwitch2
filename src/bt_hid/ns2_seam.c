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
#include "switch_pro.h"                           // switch_pro_input_t, SWITCH_MASK_*, pack_stick
#include "bt/bthid/bthid.h"                       // bthid_get_device() — connected controller identity
#include "config.h"                               // config_get_lightbar() / config_get_ns2_map()
#include "ns2_remap.h"                            // NS2_SRC_COUNT / NS2_DST_* (per-device remap)
#ifdef NS2_PRO
#include "usb.h"                                  // g_usb_personality (read-only cross-core check,
                                                   // GameCube-mode analog-fold suppression only)
#endif

#define NS2_SLOTS SWITCH_PRO_MAX_CONTROLLERS

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

// Remap source order: index -> JP button. MUST match NS2_DEFAULT_MAP in config.c and
// the web UI's source indices. (analog L2/R2 are folded into JP_BUTTON_L2/R2 below.)
static const uint32_t SRC_TO_JP[NS2_SRC_COUNT] = {
    JP_BUTTON_B1, JP_BUTTON_B2, JP_BUTTON_B3, JP_BUTTON_B4,
    JP_BUTTON_L1, JP_BUTTON_R1, JP_BUTTON_L2, JP_BUTTON_R2,
    JP_BUTTON_S1, JP_BUTTON_S2, JP_BUTTON_L3, JP_BUTTON_R3,
    JP_BUTTON_DU, JP_BUTTON_DD, JP_BUTTON_DL, JP_BUTTON_DR,
    JP_BUTTON_A1, JP_BUTTON_A2, JP_BUTTON_A3, JP_BUTTON_A4,
    JP_BUTTON_L4, JP_BUTTON_R4, JP_BUTTON_A5, JP_BUTTON_L5, JP_BUTTON_R5,
};

// Apply one remap destination to the Pro Controller output. `joycon2_active` reinterprets
// NS2_DST_GL/NS2_DST_GR as SL/SR (see include/switch_pro.h's SWITCH_EXTRA_SL/SR comment and
// docs/switch2-joycon2/mapping.md): GL/GR mean "grip button", and a lone Joy-Con2 physically has
// no grips to read, so the same generic-controller source buttons the per-family map already
// assigns to GL/GR (typically paddle/extra buttons, unused on a standard pad) instead drive the
// real SL/SR rail buttons a lone Joy-Con2 does have. This does not change Pro2/GameCube mode --
// both keep reading GL/GR unchanged, since the reinterpretation is gated on joycon2_active alone.
static void ns2_apply_dst(uint8_t dst, switch_pro_input_t *in, bool joycon2_active) {
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
        case NS2_DST_GL:      in->extra |= joycon2_active ? SWITCH_EXTRA_SL : SWITCH_EXTRA_GL; break;
        case NS2_DST_GR:      in->extra |= joycon2_active ? SWITCH_EXTRA_SR : SWITCH_EXTRA_GR; break;
        case NS2_DST_C:       in->extra |= SWITCH_EXTRA_C; break;
        default: break;  // NS2_DST_NONE
    }
}

// Controller family for remap selection (matches the web UI + config family order).
static uint8_t ns2_family(uint8_t dev_addr) {
    const bthid_device_t *dev = bthid_get_device(dev_addr);
    if (!dev) return 3;  // Generic
    uint16_t vid = dev->vendor_id;
    const char *n = dev->name;
    bool has = n && n[0];
    if (vid == 0x054C || (has && (strstr(n, "DualSense") || strstr(n, "DualShock")))) return 0;  // Sony
    if (vid == 0x045E || (has && (strstr(n, "Xbox") || strstr(n, "Elite"))))          return 1;  // Xbox
    if (vid == 0x057E || (has && (strstr(n, "Pro Controller") || strstr(n, "Joy-Con")))) return 2;  // Nintendo
    return 3;  // Generic
}

// -------------------------------------------------------------------------
// Router: the ONE call every input driver makes. Translate the unified event
// into the Pro Controller wire format (via the per-family remap) and publish it.
// -------------------------------------------------------------------------
void router_submit_input(const input_event_t *e) {
    if (!e) return;
    switch_pro_input_t in;
    memset(&in, 0, sizeof(in));

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
    bool joycon2_active = (g_usb_personality == USB_PERSONALITY_JOYCON2_L ||
                            g_usb_personality == USB_PERSONALITY_JOYCON2_R);
#else
    bool gc_active = false;
    bool joycon2_active = false;
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
    if (e->gc_native_z) in.gc_extra |= GC_MASK_Z;
    if (e->gc_l_detent) in.gc_extra |= GC_MASK_L_DETENT;
    if (e->gc_r_detent) in.gc_extra |= GC_MASK_R_DETENT;

    // Generic (non-GameCube-native) controllers used in GC mode: Confirmed 2026-07-13 by direct
    // hardware feedback (Xbox/DualSense tested in GC mode) that the Pro2-style pairing is wrong
    // for this personality -- it's the reverse. Shoulder buttons (LB/RB) become ZL and Z (Z
    // displays as "ZR" on the console's own Test Input screen, see switch_gc_encode.c), and the
    // analog triggers (LT/RT) become the GC-native L/R: the continuous value is already
    // unconditionally forwarded above, so only the digital detent click needs synthesizing here,
    // from a high-press threshold approximating the real trigger's end-of-travel detent (a
    // Hypothesis threshold, not hardware-derived -- no genuine GC trigger-to-detent curve data
    // exists for a substitute controller's analog range). Gated on !gc_has_native_layout so the
    // 8BitDo NGC Modkit's own real, already-validated per-button signals (usage 9/10/11) are
    // never second-guessed by this synthesized approximation; harmless overlap for R1 -> Z is
    // possible in principle (the Modkit's own Z usage already sets JP_BUTTON_R1 too, per
    // mapping.md) but this block never runs for it at all since gc_has_native_layout is true.
    if (gc_active && !e->gc_has_native_layout) {
        if (b & JP_BUTTON_L1) in.buttons[2] |= SWITCH_MASK_ZL;
        if (b & JP_BUTTON_R1) in.gc_extra |= GC_MASK_Z;
        if (e->analog[ANALOG_L2] > 224) in.gc_extra |= GC_MASK_L_DETENT;
        if (e->analog[ANALOG_R2] > 224) in.gc_extra |= GC_MASK_R_DETENT;
        // Real bug found via hardware feedback (2026-07-13): some pads (DualSense, several
        // 8BitDo tables -- not Xbox, whose own button map has no L2/R2 destination at all) have
        // a genuine native digital click bit for the trigger, separate from its analog axis and
        // from the seam's analog fold above (which is already suppressed for gc_active). That
        // native click bit still flowed through the per-family remap loop below into its
        // Pro2-appropriate legacy destination (NS2_DST_ZL/NS2_DST_ZR), stacking a second path on
        // top of this block's own shoulder->ZL/Z mapping. Only the left side was visible --
        // SWITCH_MASK_ZL has a live bit in the GC encoder, SWITCH_MASK_ZR does not -- which
        // produced exactly the reported asymmetry (an early spurious "ZL" partway through the
        // left trigger's travel, right trigger unaffected). Clear all four physical sources this
        // block already owns before the family loop runs, so nothing routes them a second time.
        b &= ~(JP_BUTTON_L1 | JP_BUTTON_R1 | JP_BUTTON_L2 | JP_BUTTON_R2);
    }

    // Apply this controller family's remap: each pressed source -> its assigned output.
    // The default map (NS2_DEFAULT_MAP) reproduces the built-in mapping exactly, so an
    // unconfigured device behaves as before; the config UI overrides per family.
    uint8_t slot = ns2_slot(e->dev_addr);
    uint8_t map[NS2_SRC_COUNT];
    config_get_ns2_map(ns2_family(e->dev_addr), map);
    for (int src = 0; src < NS2_SRC_COUNT; src++) {
        if (b & SRC_TO_JP[src])
            ns2_apply_dst(map[src], &in, joycon2_active);
    }

    // Sticks: 0-255 -> 12-bit; Y inverted (Switch is up-positive, HID is up=0).
    switch_pro_pack_stick(ns2_to12(e->analog[ANALOG_LX]),
                          (uint16_t)(4095 - ns2_to12(e->analog[ANALOG_LY])), in.left_stick);
    switch_pro_pack_stick(ns2_to12(e->analog[ANALOG_RX]),
                          (uint16_t)(4095 - ns2_to12(e->analog[ANALOG_RY])), in.right_stick);

    // IMU passthrough — DualSense->Switch axis transform + scaling.
    // accel /2 matches the genuine PC2 range (Experiment A). Gyro is passed ~1:1: the DualSense
    // raw gyro is already close to the Switch int16 gyro scale. The old /64 collapsed it ~60x
    // (Experiment A: genuine gyro peaked 7401 LSB, ours only 122) -> imperceptible in Steam.
    // (Report 0x05 consumes these directly; report 0x09's int32 phase/Q16.16 rewrite is separate.)
    //
    // Axis permutation — CORRECTED 2026-07-10 from a genuine-controller report-0x05 capture
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
    // own would flip the transform's handedness (determinant -1, a mirror — physically
    // impossible for a rigid IMU remount); the sign on the new Y row is flipped to restore a
    // proper rotation (determinant +1). Roll's sign is therefore inferred from that constraint,
    // not independently measured — the capture's third segment (roll) was not clean enough to
    // read a sign off directly. 🔵 Unverified on hardware; see docs/switch2/report-0x09-motion.md.
    if (e->has_motion) {
        in.has_motion = 1;
        in.accel[0] = ns2_clamp16(-e->accel[0] / 2);
        in.accel[1] = ns2_clamp16( e->accel[2] / 2);
        in.accel[2] = ns2_clamp16( e->accel[1] / 2);
        in.gyro[0]  = ns2_clamp16(-e->gyro[0]);
        in.gyro[1]  = ns2_clamp16( e->gyro[2]);
        in.gyro[2]  = ns2_clamp16( e->gyro[1]);
    }

    set_global_gamepad_input(slot, &in);
    set_global_raw_buttons(slot, b);  // b includes analog L2/R2, for the live view
    // Publish the connected controller's identity for config mode's "input type" panel.
    const bthid_device_t *dev = bthid_get_device(e->dev_addr);
    if (dev)
        set_global_device(slot, dev->name, dev->vendor_id, dev->product_id);
}

// Controller dropped -> publish a neutral (centered, no buttons) state.
void router_device_disconnected(uint8_t dev_addr, int8_t instance) {
    (void)instance;
    switch_pro_input_t in;
    memset(&in, 0, sizeof(in));
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in.left_stick);
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in.right_stick);
    set_global_gamepad_input(ns2_slot(dev_addr), &in);
}

// Raw HID report passthrough for config mode's debug view (overrides bthid.c's weak
// default). Lets us reverse-engineer inputs a driver doesn't parse yet (Elite paddles).
void bthid_on_raw_report(uint8_t conn_index, const uint8_t *data, uint16_t len) {
    set_global_raw_report(ns2_slot(conn_index), data, len);
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
    // Lightbar colour from config (single connected controller = slot 0). Drivers with an
    // RGB LED (DualSense/DualShock) apply fb->led when it's dirty; mark dirty on change.
    uint8_t rgb[3];
    config_get_lightbar(0, rgb);
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
// FEEDBACK_LED_PLAYER*). The Sony drivers read this to drive the pad LED/lightbar.
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
