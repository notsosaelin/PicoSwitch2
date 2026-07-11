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

#define NS2_SLOTS SWITCH_PRO_MAX_CONTROLLERS

// bthid conn_index (0..N) arrives as dev_addr; map it to an output slot.
static inline uint8_t ns2_slot(uint8_t dev_addr) {
    return (dev_addr < NS2_SLOTS) ? dev_addr : 0;
}

// 0-255 (center 128) -> 12-bit (center ~2048).
static inline uint16_t ns2_to12(uint8_t v) {
    return (uint16_t)(((uint32_t)v * 4095u) / 255u);
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

// Apply one remap destination to the Pro Controller output.
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
    uint32_t b = e->buttons;
    if (e->analog[ANALOG_L2] > 64) b |= JP_BUTTON_L2;
    if (e->analog[ANALOG_R2] > 64) b |= JP_BUTTON_R2;

    // Apply this controller family's remap: each pressed source -> its assigned output.
    // The default map (NS2_DEFAULT_MAP) reproduces the built-in mapping exactly, so an
    // unconfigured device behaves as before; the config UI overrides per family.
    uint8_t slot = ns2_slot(e->dev_addr);
    uint8_t map[NS2_SRC_COUNT];
    config_get_ns2_map(ns2_family(e->dev_addr), map);
    for (int src = 0; src < NS2_SRC_COUNT; src++) {
        if (b & SRC_TO_JP[src])
            ns2_apply_dst(map[src], &in);
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
// the physical controller. Bridge it to the console-decoded amplitude the USB
// core publishes via report_set_rumble().
// -------------------------------------------------------------------------
static feedback_state_t s_fb[NS2_SLOTS];

feedback_state_t *feedback_get_state(uint8_t player_index) {
    uint8_t p = (player_index < NS2_SLOTS) ? player_index : 0;
    uint8_t amp = report_get_rumble(p);
    if (amp != s_fb[p].rumble.left) {
        s_fb[p].rumble.left = amp;
        s_fb[p].rumble.right = amp;
        s_fb[p].rumble_dirty = true;
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
    return (dev_addr >= 0 && dev_addr < NS2_SLOTS) ? dev_addr : 0;
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
