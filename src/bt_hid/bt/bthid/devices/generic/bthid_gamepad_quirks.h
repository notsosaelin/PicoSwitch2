// bthid_gamepad_quirks.h - Shared type for per-controller quirk profiles used by the generic
// Bluetooth gamepad driver (bthid_gamepad.c).
//
// Naming rule for every gamepad_quirk_t defined across quirks/*: name the quirk after the exact
// model when identity is confirmed by PID (e.g. "xbox_elite2", "bitdo_ultimate_mg"); name it
// after the mechanism when it's a deliberate cross-model fallback with no confirmed exact PID
// list (e.g. "bitdo_paddle", the generic "Ultimate/Pro 2, etc." usage-3/6 paddle convention).
// A future Xbox Elite 1 or 3 gets its own quirk file/name -- never folded into "xbox" or
// "elite2". See STATUS.md's 2026-07-15 entry for the full rationale behind this split (this
// driver had grown to 1016 lines of accumulating per-controller special cases before it).
// Current architecture and the mandatory hardware gate are documented in
// docs/bluetooth/generic-gamepad-quirks.md.
#ifndef BTHID_GAMEPAD_QUIRKS_H
#define BTHID_GAMEPAD_QUIRKS_H

#include <stdint.h>
#include <stdbool.h>
#include "core/input_event.h"
#include "bthid_android_bridge.h"

// ============================================================================
// REPORT MAP TYPES (mirrors USB hid_gamepad.c dinput_usage_t)
// ============================================================================
// Moved here (not kept private to bthid_gamepad.c) because a few quirks -- e.g. the 8BitDo NGC
// Modkit -- need to read specific button-usage bits directly (buttonLoc[]) rather than only
// through the standard usage-number table, so their extract_extra() needs the full descriptor-
// derived field map, not just the raw report bytes.

#define BLE_MAX_BUTTONS 16

typedef struct {
    uint8_t byteIndex;
    uint16_t bitMask;
    uint32_t max;
} ble_usage_loc_t;

typedef struct ble_report_map_s ble_report_map_t;

// One controller family/model's behavior beyond what the standard HID-descriptor-driven parse
// already produces (axis extraction, hat->dpad, the plain usage-number button loop -- all of
// that stays in bthid_gamepad.c's core engine, shared by every quirk).
typedef struct {
    const char *name;  // e.g. "xbox_elite2" -- used in logs and the "btid desc" debug JSON dump

    // Usage-number -> JP_BUTTON_* table, used directly when select_button_map is NULL.
    const uint32_t *button_map;
    uint8_t button_map_size;

    // Optional override for families whose button table depends on something other than fixed
    // identity: Xbox (has_sim_triggers distinguishes BLE's gap-pattern table from Classic's
    // sequential one) and the 8BitDo paddle-equipped models (buttonCnt distinguishes
    // paddle-equipped from not). NULL for quirks with one single fixed table.
    void (*select_button_map)(uint8_t button_count, bool has_sim_triggers,
                              const uint32_t **out_map, uint8_t *out_size);

    // True if this device's L2/R2 "trigger" axes are really digital shoulder buttons with no
    // true analog travel (8BitDo M30) -- the core engine zeroes the synthesized analog value so
    // the digital buttons stay independently remappable.
    bool digital_shoulder_triggers;

    // Optional per-report extraction beyond the standard usage-number button loop: paddles at a
    // fixed byte offset, GC-native Z/detent bits, an extra byte outside the HID button bitfield.
    // Called after the core engine has already populated event->buttons/analog[]/button_count,
    // so it may freely OR additional bits into event->buttons. NULL if the quirk needs nothing
    // beyond the standard usage-number table.
    void (*extract_extra)(const ble_report_map_t *map, const uint8_t *data, uint16_t len,
                           input_event_t *event);

    // Optional device-specific rumble output report. NULL = rumble silently unsupported (the
    // honest default for generic HID devices without a hardware-evidenced output protocol).
    //
    // A sender is authorized only after the resolved device VID matches
    // rumble_vendor_id. Zero deliberately disables output even if a callback
    // was accidentally populated, preserving the name-only Xbox safety gate.
    uint16_t rumble_vendor_id;
    bool (*send_rumble)(uint8_t conn_index, uint8_t left, uint8_t right);
} gamepad_quirk_t;

static inline bool gamepad_quirk_can_send_rumble(const gamepad_quirk_t *quirk,
                                                  uint16_t vendor_id)
{
    return quirk && quirk->send_rumble &&
           quirk->rumble_vendor_id != 0 &&
           quirk->rumble_vendor_id == vendor_id;
}

struct ble_report_map_s {
    ble_usage_loc_t xLoc, yLoc, zLoc, rzLoc, rxLoc, ryLoc;
    ble_usage_loc_t hatLoc;
    uint8_t hat_min;            // Logical Minimum of hat (0 or 1)
    ble_usage_loc_t buttonLoc[BLE_MAX_BUTTONS];
    uint8_t buttonCnt;
    uint8_t report_id;          // Expected gamepad input report ID (0 = none)
    uint16_t input_report_len;  // Minimum complete report length, including report ID when present
    bool has_sim_triggers;      // true if triggers use Simulation Controls (Xbox-style)
    // Descriptor-declared PicoSwitch2 Android bridge extension (motion/battery in,
    // rumble/player-LED out). Zeroed for every ordinary controller; populated only
    // when the device's own descriptor declares the vendor block. See
    // bthid_android_bridge.h for why this is a declared capability, not a quirk.
    android_bridge_ext_t bridge;
    const gamepad_quirk_t *quirk;  // resolved via gamepad_quirks_identify(); NULL only before
                                    // the first descriptor parse (bthid_gamepad_set_descriptor()
                                    // sets it unconditionally as its last step, never leaving it
                                    // unset once has_report_map is true).
};

// Resolve which quirk profile applies, most-specific-first (see bthid_gamepad_quirks.c for the
// exact priority order and why it matters -- e.g. the NGC Modkit and the generic 8BitDo paddle
// fallback both match VID 0x2DC8 with 16 buttons, so PID-specific checks must run first). Called
// once right after HID descriptor parsing (buttonCnt already known) and again whenever VID/PID
// resolves later over SDP (which can complete after the descriptor already parsed) -- same
// dual-call-site pattern bthid_gamepad_set_descriptor()/bthid_gamepad_update_vid() already used
// before this refactor. `name` may be NULL/empty if not yet resolved. Never returns NULL --
// falls back to the generic quirk (plain sequential button table, no extras, no rumble).
const gamepad_quirk_t *gamepad_quirks_identify(uint16_t vendor_id, uint16_t product_id,
                                                const char *name, uint8_t button_count);

// True when `quirk` is the unmatched fallback rather than a recognized controller
// family. This is the project's established answer to "has this peer been
// identified as a supported controller?", and it stays that way: it is derived
// from the one ordered match table above, not from a second opinion about the
// HID descriptor.
//
// Used to keep keyboard/mouse descriptor classification away from peers the
// quirk table already claims -- see bthid.c's descriptor handling.
bool gamepad_quirks_is_generic(const gamepad_quirk_t *quirk);

#endif  // BTHID_GAMEPAD_QUIRKS_H
