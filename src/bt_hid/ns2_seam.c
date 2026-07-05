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

// -------------------------------------------------------------------------
// Router: the ONE call every input driver makes. Translate the unified event
// into the Pro Controller wire format and publish it on the seam. (GL/GR/C via
// JP_BUTTON_L4/R4/A2 land in a later phase once switch_pro_input_t gains an
// `extra` field; the buttons/sticks parity is here now.)
// -------------------------------------------------------------------------
void router_submit_input(const input_event_t *e) {
    if (!e) return;
    switch_pro_input_t in;
    memset(&in, 0, sizeof(in));
    const uint32_t b = e->buttons;

    // Face + shoulders (JP is normalized to physical position; Xbox labels):
    //   B1/B2/B3/B4 (A/B/X/Y phys) -> Switch B/A/Y/X ; L1/R1 -> L/R ; L2/R2 -> ZL/ZR
    if (b & JP_BUTTON_B1) in.buttons[0] |= SWITCH_MASK_B;
    if (b & JP_BUTTON_B2) in.buttons[0] |= SWITCH_MASK_A;
    if (b & JP_BUTTON_B3) in.buttons[0] |= SWITCH_MASK_Y;
    if (b & JP_BUTTON_B4) in.buttons[0] |= SWITCH_MASK_X;
    if (b & JP_BUTTON_R1) in.buttons[0] |= SWITCH_MASK_R;
    if (b & JP_BUTTON_L1) in.buttons[2] |= SWITCH_MASK_L;
    // Switch ZL/ZR are digital, but Xbox/DualSense triggers are analog and often set
    // only analog[L2/R2] (not the digital JP bit). Fire on either (thresholded), like
    // bluepad32's `brake/throttle > 64` did.
    if ((b & JP_BUTTON_L2) || e->analog[ANALOG_L2] > 64) in.buttons[2] |= SWITCH_MASK_ZL;
    if ((b & JP_BUTTON_R2) || e->analog[ANALOG_R2] > 64) in.buttons[0] |= SWITCH_MASK_ZR;
    if (b & JP_BUTTON_S1) in.buttons[1] |= SWITCH_MASK_MINUS;
    if (b & JP_BUTTON_S2) in.buttons[1] |= SWITCH_MASK_PLUS;
    if (b & JP_BUTTON_L3) in.buttons[1] |= SWITCH_MASK_L3;
    if (b & JP_BUTTON_R3) in.buttons[1] |= SWITCH_MASK_R3;
    if (b & JP_BUTTON_A1) in.buttons[1] |= SWITCH_MASK_HOME;
    if (b & JP_BUTTON_A2) in.buttons[1] |= SWITCH_MASK_CAPTURE;
    if (b & JP_BUTTON_DU) in.buttons[2] |= SWITCH_MASK_DPAD_UP;
    if (b & JP_BUTTON_DD) in.buttons[2] |= SWITCH_MASK_DPAD_DOWN;
    if (b & JP_BUTTON_DL) in.buttons[2] |= SWITCH_MASK_DPAD_LEFT;
    if (b & JP_BUTTON_DR) in.buttons[2] |= SWITCH_MASK_DPAD_RIGHT;

    // Switch 2 grips: Xbox Elite paddles / DualSense Edge back buttons (L4/R4) -> GL/GR.
    // C (chat) has no source on non-Switch pads (a real Pro Controller 2 passes it through).
    if (b & JP_BUTTON_L4) in.extra |= SWITCH_EXTRA_GL;
    if (b & JP_BUTTON_R4) in.extra |= SWITCH_EXTRA_GR;
    // C (chat): the Switch 2 controllers expose it as A3 (switch2_ble driver). NOTE: the
    // DualSense's Mute also maps to A3, so on a DualSense this currently drives C too —
    // remappable once config mode lands.
    if (b & JP_BUTTON_A3) in.extra |= SWITCH_EXTRA_C;

    // Sticks: 0-255 -> 12-bit; Y inverted (Switch is up-positive, HID is up=0).
    switch_pro_pack_stick(ns2_to12(e->analog[ANALOG_LX]),
                          (uint16_t)(4095 - ns2_to12(e->analog[ANALOG_LY])), in.left_stick);
    switch_pro_pack_stick(ns2_to12(e->analog[ANALOG_RX]),
                          (uint16_t)(4095 - ns2_to12(e->analog[ANALOG_RY])), in.right_stick);

    // IMU passthrough — same DualSense->Switch axis transform + scaling the bluepad32
    // path used (accel/2, gyro/64). Report 0x05 emits this today; report 0x09's 40-byte
    // motion packing is still unknown, so the console gets no gyro yet (tracked separately).
    if (e->has_motion) {
        in.accel[0] = ns2_clamp16(-e->accel[2] / 2);
        in.accel[1] = ns2_clamp16(-e->accel[0] / 2);
        in.accel[2] = ns2_clamp16( e->accel[1] / 2);
        in.gyro[0]  = ns2_clamp16(-e->gyro[2] / 64);
        in.gyro[1]  = ns2_clamp16(-e->gyro[0] / 64);
        in.gyro[2]  = ns2_clamp16( e->gyro[1] / 64);
    }

    set_global_gamepad_input(ns2_slot(e->dev_addr), &in);
    set_global_raw_buttons(ns2_slot(e->dev_addr), e->buttons);  // config live-view
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
