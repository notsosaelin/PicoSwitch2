#ifndef _SWITCH_PRO_H_
#define _SWITCH_PRO_H_

#include <stdint.h>
#include <stdbool.h>

#include "controller_headset.h"

// Number of emulated Pro Controllers (USB HID interfaces). Single knob, set via
// -D SWITCH_PRO_MAX_CONTROLLERS in CMake; must equal CFG_TUD_HID and the number
// of interfaces in the USB configuration descriptor. Max 4 (Switch supports 4).
#ifndef SWITCH_PRO_MAX_CONTROLLERS
#define SWITCH_PRO_MAX_CONTROLLERS 4
#endif

// --- Pro Controller 3-byte button report layout ---
// byte 0 (buttons[0]): right-side buttons + R/ZR
#define SWITCH_MASK_Y (1U << 0)
#define SWITCH_MASK_X (1U << 1)
#define SWITCH_MASK_B (1U << 2)
#define SWITCH_MASK_A (1U << 3)
#define SWITCH_MASK_R (1U << 6)
#define SWITCH_MASK_ZR (1U << 7)
// byte 1 (buttons[1]): shared/middle buttons
#define SWITCH_MASK_MINUS (1U << 0)
#define SWITCH_MASK_PLUS (1U << 1)
#define SWITCH_MASK_R3 (1U << 2)
#define SWITCH_MASK_L3 (1U << 3)
#define SWITCH_MASK_HOME (1U << 4)
#define SWITCH_MASK_CAPTURE (1U << 5)
// byte 2 (buttons[2]): left-side buttons + dpad + L/ZL
#define SWITCH_MASK_DPAD_DOWN (1U << 0)
#define SWITCH_MASK_DPAD_UP (1U << 1)
#define SWITCH_MASK_DPAD_RIGHT (1U << 2)
#define SWITCH_MASK_DPAD_LEFT (1U << 3)
#define SWITCH_MASK_L (1U << 6)
#define SWITCH_MASK_ZL (1U << 7)

// --- Switch 2 extra buttons (report 0x09 byte 2: C=0x10, GL=0x08, GR=0x04) ---
// Carried separately in switch_pro_input_t.extra; 0 on Switch-1 controllers and on
// non-Switch pads that don't expose grips (a real Pro Controller 2 passes them through).
#define SWITCH_EXTRA_C (1U << 0)   // C (chat)
#define SWITCH_EXTRA_GL (1U << 1)  // GL (left grip)
#define SWITCH_EXTRA_GR (1U << 2)  // GR (right grip)

// --- NSO GameCube-only semantic bits (switch_pro_input_t.gc_extra) ---
// These do NOT reuse SWITCH_MASK_R/SWITCH_MASK_ZR/SWITCH_MASK_L or SWITCH_EXTRA_GL/GR --
// native GameCube Z and the independent L/R trigger detents are physically distinct
// controls from Pro2's own R/ZR/L/GL/GR concepts and must never be silently mapped
// through them (see docs/switch2-gc/mapping.md "Internal normalized model requirements").
// Populated only for input devices with confirmed evidence for these controls (currently
// the 8BitDo NGC Modkit and Retro Fighters BattlerGC Pro); 0 for every other device.
// Pro2/Switch 1 encoders never read this field.
#define GC_MASK_Z (1U << 0)          // native GameCube Z (distinct from Z/ZR routing elsewhere)
#define GC_MASK_L_DETENT (1U << 1)   // independent left analog-trigger mechanical detent
#define GC_MASK_R_DETENT (1U << 2)   // independent right analog-trigger mechanical detent
#define GC_MASK_ZL (1U << 3)         // native GameCube ZL shoulder

// Switch analog sticks are 12-bit (0x000..0xFFF), centered at 0x800.
#define SWITCH_STICK_MIN 0x000
#define SWITCH_STICK_MID 0x800
#define SWITCH_STICK_MAX 0xFFF

// Per-controller input state produced by the Bluetooth core (core1) and
// consumed by the USB core (core0). Sticks are pre-packed into the Pro
// Controller's 12-bit-in-3-bytes wire format. IMU values are pre-scaled to the
// Pro Controller's raw 16-bit units (filled in Phase 2; 0 = no motion).
typedef struct {
    uint8_t buttons[3];      // SWITCH_MASK_* bitfields, indexed [0..2]
    uint8_t extra;           // SWITCH_EXTRA_* — Switch 2 extra buttons (C / GL / GR)
    uint8_t left_stick[3];   // packed 12-bit lx/ly
    uint8_t right_stick[3];  // packed 12-bit rx/ry
    int16_t accel[3];        // x, y, z (raw Pro Controller accelerometer units)
    int16_t gyro[3];         // x, y, z (raw Pro Controller gyroscope units)
    uint8_t has_motion;      // 1 if this controller reports IMU (gate report-0x09 motion)
    uint8_t battery_level;   // normalized 0..100 percentage
    uint8_t battery_valid;   // 1 when the source controller reported a level
    uint8_t battery_charging;// 1 when the source controller reports charging
#ifdef NS2_DS5_AUDIO
    uint8_t headset_state;   // CONTROLLER_HEADSET_* physical jack state
#endif

    // NSO GameCube-only fields (2026-07-13). Always present (not `#ifdef NS2_PRO`) so this
    // struct's layout/size is identical across build configs; Pro2/Switch 1 encoders simply
    // never read them, and their wire output must stay byte-for-byte unchanged. Populated by
    // ns2_seam.c's router_submit_input() from input_event_t's own gc_* fields (see
    // core/input_event.h), which are themselves only set by drivers with confirmed evidence
    // for a device's real GameCube-native controls (currently the 8BitDo NGC Modkit and
    // Retro Fighters BattlerGC Pro).
    uint8_t left_trigger;    // continuous 0..255, GameCube native analog L (raw passthrough,
                             // never thresholded/collapsed to a boolean)
    uint8_t right_trigger;   // continuous 0..255, GameCube native analog R
    uint8_t gc_extra;        // GC_MASK_* -- native Z / independent L,R trigger detents
} switch_pro_input_t;

// Pack two 12-bit stick axes (0..4095) into the 3-byte Pro Controller format.
static inline void switch_pro_pack_stick(uint16_t x, uint16_t y, uint8_t out[3]) {
    out[0] = (uint8_t)(x & 0xFF);
    out[1] = (uint8_t)(((y & 0x0F) << 4) | ((x >> 8) & 0x0F));
    out[2] = (uint8_t)((y >> 4) & 0xFF);
}

// Initialize per-interface protocol state (called once, on the USB core).
void switch_pro_init(void);

// Feed an output report received from the console (called from
// tud_hid_set_report_cb on the USB core). buf[0] is the report id.
void switch_pro_receive(uint8_t instance, const uint8_t *buf, uint16_t len);

// Build the next 64-byte input report for an interface into out (must be >= 64
// bytes). Returns the number of bytes to send (always 64). Reads the latest
// shared input for that instance. Called on the USB core.
uint16_t switch_pro_generate_report(uint8_t instance, uint8_t *out);

#endif  // _SWITCH_PRO_H_
