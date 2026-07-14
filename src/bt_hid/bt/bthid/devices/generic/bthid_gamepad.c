// bthid_gamepad.c - Generic Bluetooth Gamepad Driver
// Handles basic HID gamepads over Bluetooth
// This is a fallback driver for gamepads without a specific driver
//
// For BLE devices with HID descriptors, uses the same HID report parser
// as the USB path (hid_parser.c) to dynamically extract field locations.
// Falls back to hardcoded 6-byte layout for Classic BT devices without descriptors.

#include "bthid_gamepad.h"
#include "bt/bthid/bthid.h"
#include "bt/transport/bt_transport.h"
#include "core/input_event.h"
#include "core/router/router.h"
#include "core/buttons.h"
#include "core/services/players/manager.h"
#include "core/services/players/feedback.h"
#include "usb/usbh/hid/devices/generic/hid_parser.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// REPORT MAP TYPES (mirrors USB hid_gamepad.c dinput_usage_t)
// ============================================================================

#define BLE_MAX_BUTTONS 16

typedef struct {
    uint8_t byteIndex;
    uint16_t bitMask;
    uint32_t max;
} ble_usage_loc_t;

typedef struct {
    ble_usage_loc_t xLoc, yLoc, zLoc, rzLoc, rxLoc, ryLoc;
    ble_usage_loc_t hatLoc;
    uint8_t hat_min;            // Logical Minimum of hat (0 or 1)
    ble_usage_loc_t buttonLoc[BLE_MAX_BUTTONS];
    uint8_t buttonCnt;
    uint8_t report_id;          // Expected gamepad input report ID (0 = none)
    bool has_sim_triggers;      // true if triggers use Simulation Controls (Xbox-style)
    bool is_xbox;               // true if Microsoft VID (0x045E) — affects button map
    bool is_8bitdo;             // true if 8BitDo VID (0x2DC8) — paddle button order
    bool is_elite2;             // true if Xbox Elite Series 2 — 4 back paddles in last report byte
    bool digital_shoulder_triggers; // controller has no real analog triggers; its
                                // L2/R2 "trigger" axes are just the digital shoulder
                                // buttons (e.g. 8BitDo M30). Suppress analog L2/R2 so
                                // the digital buttons remain remappable.
    bool is_ngc_modkit;         // true if 8BitDo NGC Modkit specifically (PID 0x286A, not
                                // just the shared 0x2DC8 8BitDo VID) -- a GameCube-shell
                                // modkit with a different physical layout than 8BitDo's
                                // paddle controllers (Ultimate/Pro 2), which BITDO_BUTTON_MAP
                                // below is actually for. See NGC_MODKIT_BUTTON_MAP's own
                                // comment for the mapping rationale, confirmed 2026-07-12
                                // via live hardware capture.
} ble_report_map_t;

// ============================================================================
// DRIVER DATA
// ============================================================================

typedef struct {
    input_event_t event;        // Current input state
    bool initialized;
    bool has_report_map;        // true if HID descriptor was parsed
    ble_report_map_t map;       // cached field locations from descriptor
    uint8_t rumble_left;        // Last sent rumble values (for change detection)
    uint8_t rumble_right;
} bthid_gamepad_data_t;

static bthid_gamepad_data_t gamepad_data[BTHID_MAX_DEVICES];

// ============================================================================
// HAT SWITCH LOOKUP (same as USB hid_gamepad.c)
// ============================================================================
// hat format: 8 = released, 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
// Returns packed dpad bits: bit0=up, bit1=right, bit2=down, bit3=left

static const uint8_t HAT_SWITCH_TO_DIRECTION_BUTTONS[] = {
    0b0001, 0b0011, 0b0010, 0b0110, 0b0100, 0b1100, 0b1000, 0b1001, 0b0000
};

// ============================================================================
// BUTTON USAGE MAPPING TABLES
// ============================================================================

// Xbox BT HID: buttons 1-15 with gaps at 3,6,9,10
// A=1, B=2, X=4, Y=5, LB=7, RB=8, View=11, Menu=12, Xbox=13, L3=14, R3=15
static const uint32_t XBOX_BUTTON_MAP[17] = {
    0,                  // usage 0: invalid
    JP_BUTTON_B1,       // usage 1: A
    JP_BUTTON_B2,       // usage 2: B
    0,                  // usage 3: (pad)
    JP_BUTTON_B3,       // usage 4: X
    JP_BUTTON_B4,       // usage 5: Y
    0,                  // usage 6: (pad)
    JP_BUTTON_L1,       // usage 7: LB
    JP_BUTTON_R1,       // usage 8: RB
    0,                  // usage 9: (pad)
    0,                  // usage 10: (pad)
    JP_BUTTON_S1,       // usage 11: View
    JP_BUTTON_S2,       // usage 12: Menu
    JP_BUTTON_A1,       // usage 13: Xbox
    JP_BUTTON_L3,       // usage 14: L3
    JP_BUTTON_R3,       // usage 15: R3
    JP_BUTTON_A2,       // usage 16: Share (Series X/S)
};

// Xbox Classic BT: sequential buttons 1-15 (no gaps like BLE), different order from generic
static const uint32_t XBOX_SEQ_BUTTON_MAP[16] = {
    0,                  // usage 0: invalid
    JP_BUTTON_B1,       // usage 1: A
    JP_BUTTON_B2,       // usage 2: B
    JP_BUTTON_B3,       // usage 3: X
    JP_BUTTON_B4,       // usage 4: Y
    JP_BUTTON_L1,       // usage 5: LB
    JP_BUTTON_R1,       // usage 6: RB
    JP_BUTTON_S1,       // usage 7: Back/View
    JP_BUTTON_S2,       // usage 8: Start/Menu
    JP_BUTTON_L3,       // usage 9: L3
    JP_BUTTON_R3,       // usage 10: R3
    JP_BUTTON_A1,       // usage 11: Guide
    0,                  // usage 12
    0,                  // usage 13
    0,                  // usage 14
    JP_BUTTON_A2,       // usage 15: Share (Series X/S)
};

// Standard sequential HID gamepads: shoulders before triggers
// Used by most generic controllers
static const uint32_t SEQ_BUTTON_MAP[16] = {
    0,                  // usage 0: invalid
    JP_BUTTON_B1,       // usage 1: face 1 (A/Cross)
    JP_BUTTON_B2,       // usage 2: face 2 (B/Circle)
    JP_BUTTON_B3,       // usage 3: face 3 (X/Square)
    JP_BUTTON_B4,       // usage 4: face 4 (Y/Triangle)
    JP_BUTTON_L1,       // usage 5: left shoulder
    JP_BUTTON_R1,       // usage 6: right shoulder
    JP_BUTTON_L2,       // usage 7: left trigger (digital)
    JP_BUTTON_R2,       // usage 8: right trigger (digital)
    JP_BUTTON_S1,       // usage 9: select/back
    JP_BUTTON_S2,       // usage 10: start/menu
    JP_BUTTON_L3,       // usage 11: left stick
    JP_BUTTON_R3,       // usage 12: right stick
    JP_BUTTON_A1,       // usage 13: guide/home
    JP_BUTTON_A2,       // usage 14: capture/share
    JP_BUTTON_A3,       // usage 15: assistant/mute
};

// 8BitDo controllers with back paddles (Ultimate, Pro 2, etc.)
// VID 0x2DC8. Descriptor order inserts R4 at 3 and L4 at 6:
//   A B R4 X Y L4 LB RB LT RT Select Start L3 R3 Home Capture
static const uint32_t BITDO_BUTTON_MAP[17] = {
    0,                  // usage 0: invalid
    JP_BUTTON_B1,       // usage 1: A
    JP_BUTTON_B2,       // usage 2: B
    JP_BUTTON_R4,       // usage 3: R4 (back paddle)
    JP_BUTTON_B3,       // usage 4: X
    JP_BUTTON_B4,       // usage 5: Y
    JP_BUTTON_L4,       // usage 6: L4 (back paddle)
    JP_BUTTON_L1,       // usage 7: LB
    JP_BUTTON_R1,       // usage 8: RB
    JP_BUTTON_L2,       // usage 9: LT (digital)
    JP_BUTTON_R2,       // usage 10: RT (digital)
    JP_BUTTON_S1,       // usage 11: Select
    JP_BUTTON_S2,       // usage 12: Start
    JP_BUTTON_A1,       // usage 13: Home
    JP_BUTTON_L3,       // usage 14: L3
    JP_BUTTON_R3,       // usage 15: R3
    JP_BUTTON_A2,       // usage 16: Capture
};

// 8BitDo NGC Modkit (VID 0x2DC8, PID 0x286A specifically -- NOT the same physical layout as
// the paddle-equipped 8BitDo Ultimate/Pro 2 that BITDO_BUTTON_MAP above is for). A GameCube-
// shell modkit; button_cnt also happens to be 16 like the paddle controllers, which is exactly
// why this needs its own PID-specific table rather than falling into BITDO_BUTTON_MAP's
// is_8bitdo+buttonCnt>14 rule -- confirmed 2026-07-12 via live hardware capture (see
// docs/bluetooth/8bitdo-ngc-diy-profile.md and
// docs/experiments/gate2-identity-log-hardware-captures-2026-07-12.md for the full raw report
// evidence, including two design iterations this table went through before the owner confirmed
// the final mapping on real hardware -- see that doc for what was tried and rejected, and why).
//
// Face buttons are DIRECT, not rotated: this is a GameCube-shaped controller, so physical
// A/B/X/Y should land on Switch A/B/X/Y directly -- unlike Xbox/PlayStation pads, which use the
// standard rotated convention (physical "A" position -> Switch B slot, etc.) baked into
// JP_BUTTON_B1..B4's own default remap. Getting the *labels* right here means picking the
// JP_BUTTON_* source whose DEFAULT destination is the matching letter (see NS2_DEFAULT_MAP in
// config.c): JP_BUTTON_B2->NS2 A, JP_BUTTON_B1->NS2 B, JP_BUTTON_B4->NS2 X, JP_BUTTON_B3->NS2 Y.
//
// Usages 3, 6, 13, 16 are not wired to any physical control on this unit (interactively
// confirmed -- pressed every remaining button on the controller, nothing else fires).
//
// Usages 7/8 (partial-travel echo bits, fire well before the real click -- see the R2/L2
// investigation in the capture doc) and usages 9/10 (the TRUE mechanical click) are ALL
// suppressed here (mapped to 0). This was NOT the original design -- an earlier iteration
// mapped 9/10 to JP_BUTTON_L1/R1 for discrete "L/R pressed" semantics, but the owner wants the
// simpler, hardware-owner-confirmed behavior instead: the trigger's continuous ANALOG value
// (bytes 6/7, already fed to ANALOG_L2/ANALOG_R2 correctly) is what should drive ZL/ZR, via
// this project's existing generic seam-level fold in ns2_seam.c's router_submit_input()
// (`if (analog[ANALOG_L2] > 64) ... JP_BUTTON_L2`, which NS2_DEFAULT_MAP already routes to
// NS2_DST_ZL/ZR) -- "any real press" registers as ZL/ZR, not just the discrete click. No digital
// bit needs to come from the button-usage table at all for this to work correctly.
//
// Usage 11 (Z) maps to JP_BUTTON_R1 (NOT R2/L2 -- deliberately a different bit than whatever the
// analog fold touches, so Z can never conflict with a simultaneous real trigger press), which
// NS2_DEFAULT_MAP routes to plain NS2_DST_R. Nothing here ever emits JP_BUTTON_L1, so NS2 "L"
// correctly never fires (there's no left-side equivalent of Z on a real GameCube controller).
// NS2 "C" (Gamechat) also never fires -- nothing maps to JP_BUTTON_A3/A5, its only sources.
//
// Usages 14/15 (physical L3/R3 stick clicks) are deliberately repurposed, not passed through
// as L3/R3: a genuine GameCube controller has no clickable sticks at all, so this modkit's
// extra hardware becomes CAPTURE/HOME instead (JP_BUTTON_A2/A1, routed by NS2_DEFAULT_MAP to
// NS2_DST_CAPTURE/NS2_DST_HOME) -- explicit product decision, not a claim about printed labels.
//
// This is Pro-Controller-2-mode-specific: it approximates "trigger touched" as a digital ZL/ZR
// press because that's all today's output supports. The eventual NSO GameCube USB personality
// (Gate 3, not started) will differ: real continuous L/R analog output instead of the ZL/ZR
// approximation, and Z will very likely target ZR (or whatever NSO's own GC-adapter convention
// turns out to be) rather than plain R -- do not assume this table's choices carry over
// unchanged when that work starts; re-derive against NSO's actual behavior.
//
// This table covers the pairing mode captured 2026-07-12 (Classic BT, resolved via SDP as
// 0x2DC8:0x286A). The controller is reported to also support a second "Android/D-Input" BLE
// pairing mode -- unconfirmed whether it uses the same PID or the same report shape; needs its
// own capture before this table can be assumed to cover it. See the profile doc's "Status"
// section.
static const uint32_t NGC_MODKIT_BUTTON_MAP[17] = {
    0,                  // usage 0: invalid
    JP_BUTTON_B2,       // usage 1: A -> NS2 A (direct, not rotated)
    JP_BUTTON_B1,       // usage 2: B -> NS2 B
    0,                  // usage 3: not wired on this unit
    JP_BUTTON_B4,       // usage 4: X -> NS2 X
    JP_BUTTON_B3,       // usage 5: Y -> NS2 Y
    0,                  // usage 6: not wired on this unit
    0,                  // usage 7: L-trigger partial-travel echo -- suppressed
    0,                  // usage 8: R-trigger partial-travel echo -- suppressed
    0,                  // usage 9: L trigger true click -- suppressed; analog fold drives ZL instead
    0,                  // usage 10: R trigger true click -- suppressed; analog fold drives ZR instead
    JP_BUTTON_R1,       // usage 11: Z -> NS2 "R" (not ZR -- must not collide with the analog fold)
    JP_BUTTON_S2,       // usage 12: Start
    0,                  // usage 13: not wired on this unit (no Home/Menu button)
    JP_BUTTON_A2,       // usage 14: physical L3 repurposed -> CAPTURE
    JP_BUTTON_A1,       // usage 15: physical R3 repurposed -> HOME
    0,                  // usage 16: not wired on this unit
};

// ============================================================================
// ANALOG SCALING (same as USB hid_gamepad.c scale_analog_hid_gamepad)
// ============================================================================

static uint8_t scale_analog(uint16_t value, uint32_t max_value)
{
    if (max_value == 0) return 128;
    return (uint8_t)((uint32_t)value * 255 / max_value);
}

// ============================================================================
// HID DESCRIPTOR PARSING
// ============================================================================

// Extract a field value from report data given byte index and bit mask
static uint16_t extract_field(const uint8_t* data, uint16_t len, ble_usage_loc_t* loc)
{
    if (!loc->bitMask || loc->byteIndex >= len) return 0;

    uint16_t raw;
    if (loc->bitMask > 0xFF && (loc->byteIndex + 1) < len) {
        // 16-bit field spanning two bytes (HID reports are little-endian)
        raw = (uint16_t)data[loc->byteIndex] | ((uint16_t)data[loc->byteIndex + 1] << 8);
    } else {
        raw = data[loc->byteIndex];
    }
    return (raw & loc->bitMask) >> __builtin_ctz(loc->bitMask);
}

static bool device_is_m30(const bthid_device_t* device);

void bthid_gamepad_set_descriptor(bthid_device_t* device, const uint8_t* desc, uint16_t desc_len)
{
    bthid_gamepad_data_t* gp = (bthid_gamepad_data_t*)device->driver_data;
    if (!gp) return;

    printf("[BTHID_GAMEPAD] Parsing HID descriptor (%d bytes)\n", desc_len);

    HID_ReportInfo_t* info = NULL;
    uint8_t ret = USB_ProcessHIDReport(0, 0, desc, desc_len, &info);
    if (ret != HID_PARSE_Successful) {
        printf("[BTHID_GAMEPAD] HID parse failed: %d\n", ret);
        return;
    }

    // Clear the map
    memset(&gp->map, 0, sizeof(ble_report_map_t));

    uint8_t btns_count = 0;
    uint8_t idOffset = 0;

    // Pass 1: Find the gamepad report ID (the one containing Generic Desktop X axis)
    // This reliably identifies the gamepad report in multi-report-ID descriptors
    // that may also contain Consumer Control, Keyboard, or other collections.
    uint8_t gamepad_report_id = 0;
    HID_ReportItem_t* scan = info->FirstReportItem;
    while (scan) {
        if (scan->Attributes.Usage.Page == 0x01 && scan->Attributes.Usage.Usage == 0x30) {
            gamepad_report_id = scan->ReportID;
            break;
        }
        scan = scan->Next;
    }

    // Set up report ID offset
    if (gamepad_report_id) {
        idOffset = 8;  // Report ID takes first byte (8 bits)
        gp->map.report_id = gamepad_report_id;
    } else if (info->UsingReportIDs && info->FirstReportItem) {
        // Fallback: use first item's report ID
        idOffset = 8;
        gp->map.report_id = info->FirstReportItem->ReportID;
    }

    // Pass 2: Only process items from the gamepad report
    HID_ReportItem_t* item = info->FirstReportItem;
    while (item) {
        // Skip items from other report IDs
        if (item->ReportID != gamepad_report_id) {
            item = item->Next;
            continue;
        }
        uint8_t bitSize = item->Attributes.BitSize;
        uint8_t bitOffset = item->BitOffset + idOffset;
        uint16_t bitMask = ((0xFFFF >> (16 - bitSize)) << (bitOffset % 8));
        uint8_t byteIndex = bitOffset / 8;

        uint8_t report[1] = {0};
        if (USB_GetHIDReportItemInfo(item->ReportID, report, item)) {
            switch (item->Attributes.Usage.Page) {
                case 0x01:  // Generic Desktop
                    switch (item->Attributes.Usage.Usage) {
                        case 0x30:  // X - Left Analog X
                            gp->map.xLoc.byteIndex = byteIndex;
                            gp->map.xLoc.bitMask = bitMask;
                            gp->map.xLoc.max = item->Attributes.Logical.Maximum;
                            break;
                        case 0x31:  // Y - Left Analog Y
                            gp->map.yLoc.byteIndex = byteIndex;
                            gp->map.yLoc.bitMask = bitMask;
                            gp->map.yLoc.max = item->Attributes.Logical.Maximum;
                            break;
                        case 0x32:  // Z - Right Analog X
                            gp->map.zLoc.byteIndex = byteIndex;
                            gp->map.zLoc.bitMask = bitMask;
                            gp->map.zLoc.max = item->Attributes.Logical.Maximum;
                            break;
                        case 0x35:  // RZ - Right Analog Y
                            gp->map.rzLoc.byteIndex = byteIndex;
                            gp->map.rzLoc.bitMask = bitMask;
                            gp->map.rzLoc.max = item->Attributes.Logical.Maximum;
                            break;
                        case 0x33:  // RX - Left Trigger
                            gp->map.rxLoc.byteIndex = byteIndex;
                            gp->map.rxLoc.bitMask = bitMask;
                            gp->map.rxLoc.max = item->Attributes.Logical.Maximum;
                            break;
                        case 0x34:  // RY - Right Trigger
                            gp->map.ryLoc.byteIndex = byteIndex;
                            gp->map.ryLoc.bitMask = bitMask;
                            gp->map.ryLoc.max = item->Attributes.Logical.Maximum;
                            break;
                        case 0x39:  // Hat switch
                            gp->map.hatLoc.byteIndex = byteIndex;
                            gp->map.hatLoc.bitMask = bitMask;
                            gp->map.hat_min = (uint8_t)item->Attributes.Logical.Minimum;
                            break;
                    }
                    break;
                case 0x02:  // Simulation Controls (Xbox-style triggers)
                    switch (item->Attributes.Usage.Usage) {
                        case 0xC5:  // Brake → Left Trigger
                            gp->map.rxLoc.byteIndex = byteIndex;
                            gp->map.rxLoc.bitMask = bitMask;
                            gp->map.rxLoc.max = item->Attributes.Logical.Maximum;
                            gp->map.has_sim_triggers = true;
                            break;
                        case 0xC4:  // Accelerator → Right Trigger
                            gp->map.ryLoc.byteIndex = byteIndex;
                            gp->map.ryLoc.bitMask = bitMask;
                            gp->map.ryLoc.max = item->Attributes.Logical.Maximum;
                            gp->map.has_sim_triggers = true;
                            break;
                    }
                    break;
                case 0x09: {  // Button
                    uint8_t usage = item->Attributes.Usage.Usage;
                    if (usage >= 1 && usage <= BLE_MAX_BUTTONS) {
                        gp->map.buttonLoc[usage - 1].byteIndex = byteIndex;
                        gp->map.buttonLoc[usage - 1].bitMask = bitMask;
                    }
                    btns_count++;
                    break;
                }
            }
        }
        item = item->Next;
    }

    gp->map.buttonCnt = btns_count;

    // Release parser memory
    USB_FreeReportInfo(info);

    // Auto-detect swapped Z/RZ vs RX/RY axes.
    // Some controllers (8BitDo, Sony) use RX/RY for right stick and Z/RZ for triggers,
    // while others (Xbox, DirectInput) use Z/RZ for right stick and RX/RY for triggers.
    // Detect by comparing axis resolution: sticks match X/Y resolution, triggers are smaller.
    if (!gp->map.has_sim_triggers &&
        gp->map.zLoc.max && gp->map.rzLoc.max &&
        gp->map.rxLoc.max && gp->map.ryLoc.max &&
        gp->map.xLoc.max) {
        // If RX/RY have same resolution as X/Y (stick-like) and Z/RZ are smaller (trigger-like),
        // swap: RX/RY become right stick, Z/RZ become triggers
        bool rx_is_stick = (gp->map.rxLoc.max == gp->map.xLoc.max);
        bool z_is_trigger = (gp->map.zLoc.max < gp->map.xLoc.max);
        if (rx_is_stick && z_is_trigger) {
            printf("[BTHID_GAMEPAD] Swapping Z/RZ<->RX/RY (RX/RY=stick, Z/RZ=trigger)\n");
            ble_usage_loc_t tmp;
            tmp = gp->map.zLoc;  gp->map.zLoc  = gp->map.rxLoc; gp->map.rxLoc = tmp;
            tmp = gp->map.rzLoc; gp->map.rzLoc = gp->map.ryLoc; gp->map.ryLoc = tmp;
        }
    }

    // BLE PnP VID/PID often doesn't resolve, so also match Xbox by name (the driver
    // already relies on the name; button output is correct, which proves is_xbox holds).
    gp->map.is_xbox = (device->vendor_id == 0x045E) ||
                      (device->name[0] && strstr(device->name, "Xbox") != NULL);
    gp->map.is_8bitdo = (device->vendor_id == 0x2DC8);
    gp->map.is_elite2 = (device->vendor_id == 0x045E &&
                         (device->product_id == 0x0B05 || device->product_id == 0x0B22));
    gp->map.is_ngc_modkit = (device->vendor_id == 0x2DC8 && device->product_id == 0x286A);
    gp->map.digital_shoulder_triggers = device_is_m30(device);
    gp->has_report_map = true;
    printf("[BTHID_GAMEPAD] Parsed: %d btns, X@%d Y@%d Z@%d RZ@%d RX@%d RY@%d hat@%d(min=%d) sim=%d xbox=%d 8bitdo=%d\n",
           btns_count,
           gp->map.xLoc.byteIndex, gp->map.yLoc.byteIndex,
           gp->map.zLoc.byteIndex, gp->map.rzLoc.byteIndex,
           gp->map.rxLoc.byteIndex, gp->map.ryLoc.byteIndex,
           gp->map.hatLoc.byteIndex, gp->map.hat_min, gp->map.has_sim_triggers,
           gp->map.is_xbox, gp->map.is_8bitdo);
}

// 8BitDo M30: Genesis/Saturn-style pad with NO analog triggers — its L2/R2 are
// digital shoulder buttons that the HID report also exposes as trigger axes.
// Reporting them as analog L2/R2 makes the trigger output bypass button
// remapping (the analog axis isn't remapped), so users can't reassign L2/R2.
//
// Identify primarily by the device NAME: it's the one identifier stable across
// M30 firmware revisions / power-on modes (which report different BT PIDs), and
// some units never resolve VID/PID at all (matched only by BT class-of-device,
// so vendor_id/product_id stay 0). VID/PID is a secondary match for when the
// SDP Device ID query does succeed.
static bool device_is_m30(const bthid_device_t* device)
{
    // Require BOTH "8BitDo" and "M30" in the name so unrelated devices that just
    // happen to contain "M30" (phones, other pads) don't get their real analog
    // triggers zeroed. The advertised name is "8BitDo M30 gamepad".
    const char* name = device->name;
    if (name[0] && strstr(name, "8BitDo") && strstr(name, "M30")) {
        return true;
    }
    return (device->vendor_id == 0x2DC8 &&
            (device->product_id == 0x0651 ||   // M30 over Bluetooth (SDP Device ID)
             device->product_id == 0x5006));   // M30 USB PID (defensive)
}

void bthid_gamepad_update_vid(bthid_device_t* device)
{
    bthid_gamepad_data_t* gp = (bthid_gamepad_data_t*)device->driver_data;
    if (!gp || !gp->has_report_map) return;

    // BLE PnP VID/PID often doesn't resolve, so also match Xbox by name (the driver
    // already relies on the name; button output is correct, which proves is_xbox holds).
    gp->map.is_xbox = (device->vendor_id == 0x045E) ||
                      (device->name[0] && strstr(device->name, "Xbox") != NULL);
    gp->map.is_8bitdo = (device->vendor_id == 0x2DC8);
    gp->map.is_elite2 = (device->vendor_id == 0x045E &&
                         (device->product_id == 0x0B05 || device->product_id == 0x0B22));
    gp->map.is_ngc_modkit = (device->vendor_id == 0x2DC8 && device->product_id == 0x286A);
    gp->map.digital_shoulder_triggers = device_is_m30(device);
}

// Debug: format the parsed report-field map — see bthid_gamepad.h for why this exists
// (inspecting real descriptor-parse results instead of guessing from input symptoms).
bool bthid_gamepad_dump_map(uint8_t conn_index, char* out, unsigned out_size)
{
    bthid_device_t* device = bthid_get_device(conn_index);
    if (!device || (const bthid_driver_t*)device->driver != &bthid_gamepad_driver) {
        snprintf(out, out_size, "\"not-generic-driver\"");
        return false;
    }
    bthid_gamepad_data_t* gp = (bthid_gamepad_data_t*)device->driver_data;
    if (!gp || !gp->has_report_map) {
        snprintf(out, out_size, "\"no-report-map-parsed-yet\"");
        return false;
    }
    ble_report_map_t* m = &gp->map;
    int j = snprintf(out, out_size,
        "{\"report_id\":%u,\"button_cnt\":%u,\"is_xbox\":%s,\"is_8bitdo\":%s,"
        "\"is_elite2\":%s,\"is_ngc_modkit\":%s,\"has_sim_triggers\":%s,\"digital_shoulder_triggers\":%s,"
        "\"hat\":{\"byte\":%u,\"mask\":\"0x%02X\",\"min\":%u},"
        "\"x\":{\"byte\":%u,\"mask\":\"0x%02X\",\"max\":%lu},"
        "\"y\":{\"byte\":%u,\"mask\":\"0x%02X\",\"max\":%lu},"
        "\"z\":{\"byte\":%u,\"mask\":\"0x%02X\",\"max\":%lu},"
        "\"rz\":{\"byte\":%u,\"mask\":\"0x%02X\",\"max\":%lu},"
        "\"rx\":{\"byte\":%u,\"mask\":\"0x%02X\",\"max\":%lu},"
        "\"ry\":{\"byte\":%u,\"mask\":\"0x%02X\",\"max\":%lu},"
        "\"buttons\":[",
        m->report_id, m->buttonCnt, m->is_xbox ? "true" : "false",
        m->is_8bitdo ? "true" : "false", m->is_elite2 ? "true" : "false",
        m->is_ngc_modkit ? "true" : "false",
        m->has_sim_triggers ? "true" : "false", m->digital_shoulder_triggers ? "true" : "false",
        m->hatLoc.byteIndex, m->hatLoc.bitMask, m->hat_min,
        m->xLoc.byteIndex, m->xLoc.bitMask, (unsigned long)m->xLoc.max,
        m->yLoc.byteIndex, m->yLoc.bitMask, (unsigned long)m->yLoc.max,
        m->zLoc.byteIndex, m->zLoc.bitMask, (unsigned long)m->zLoc.max,
        m->rzLoc.byteIndex, m->rzLoc.bitMask, (unsigned long)m->rzLoc.max,
        m->rxLoc.byteIndex, m->rxLoc.bitMask, (unsigned long)m->rxLoc.max,
        m->ryLoc.byteIndex, m->ryLoc.bitMask, (unsigned long)m->ryLoc.max);
    bool first = true;
    for (int i = 0; i < BLE_MAX_BUTTONS && j < (int)out_size - 40; i++) {
        if (!m->buttonLoc[i].bitMask) continue;
        j += snprintf(out + j, out_size - j,
            "%s{\"usage\":%d,\"byte\":%u,\"mask\":\"0x%02X\"}",
            first ? "" : ",", i + 1, m->buttonLoc[i].byteIndex, m->buttonLoc[i].bitMask);
        first = false;
    }
    snprintf(out + j, out_size - j, "]}");
    return true;
}

// ============================================================================
// DYNAMIC REPORT PROCESSING (from parsed HID descriptor)
// ============================================================================

static void process_report_dynamic(bthid_gamepad_data_t* gp, const uint8_t* data, uint16_t len)
{
    ble_report_map_t* map = &gp->map;
    uint32_t buttons = 0;

    // Extract analog axes
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    uint8_t l2 = 0, r2 = 0;

    if (map->xLoc.max) {
        lx = scale_analog(extract_field(data, len, &map->xLoc), map->xLoc.max);
    }
    if (map->yLoc.max) {
        ly = scale_analog(extract_field(data, len, &map->yLoc), map->yLoc.max);
    }
    if (map->zLoc.max) {
        rx = scale_analog(extract_field(data, len, &map->zLoc), map->zLoc.max);
    }
    if (map->rzLoc.max) {
        ry = scale_analog(extract_field(data, len, &map->rzLoc), map->rzLoc.max);
    }
    if (map->rxLoc.max) {
        l2 = scale_analog(extract_field(data, len, &map->rxLoc), map->rxLoc.max);
    }
    if (map->ryLoc.max) {
        r2 = scale_analog(extract_field(data, len, &map->ryLoc), map->ryLoc.max);
    }

    // Controllers whose "triggers" are really digital shoulder buttons (M30):
    // drop the synthesized analog so L2/R2 come only from the digital buttons,
    // which stay subject to button remapping.
    if (map->digital_shoulder_triggers) {
        l2 = 0;
        r2 = 0;
    }

    // Hat switch -> dpad
    // Table is 0-based: [0]=N, [1]=NE, ..., [7]=NW, [8]=center
    // HID descriptors use either min=0 (0=N) or min=1 (1=N, 0=center)
    if (map->hatLoc.bitMask && map->hatLoc.byteIndex < len) {
        uint8_t hatValue = (uint8_t)extract_field(data, len, &map->hatLoc);
        uint8_t direction;
        if (map->hat_min > 0) {
            // 1-based hat: value 0 and values > max are center
            direction = (hatValue >= map->hat_min && hatValue <= map->hat_min + 7)
                        ? (hatValue - map->hat_min) : 8;
        } else {
            direction = hatValue <= 8 ? hatValue : 8;
        }
        uint8_t dpad = HAT_SWITCH_TO_DIRECTION_BUTTONS[direction];
        if (dpad & 0x01) buttons |= JP_BUTTON_DU;
        if (dpad & 0x02) buttons |= JP_BUTTON_DR;
        if (dpad & 0x04) buttons |= JP_BUTTON_DD;
        if (dpad & 0x08) buttons |= JP_BUTTON_DL;
    }

    // Map buttons by HID usage number using descriptor-derived layout detection
    // Simulation Controls triggers (Brake/Accelerator) = Xbox gap pattern
    // Generic Desktop triggers (Rx/Ry) = sequential button layout
    const uint32_t* btn_map;
    uint8_t btn_map_size;
    if (map->is_xbox && map->has_sim_triggers) {
        // Xbox BLE: gap-pattern buttons with Simulation Controls triggers
        btn_map = XBOX_BUTTON_MAP;
        btn_map_size = sizeof(XBOX_BUTTON_MAP) / sizeof(XBOX_BUTTON_MAP[0]);
    } else if (map->is_xbox) {
        // Xbox Classic BT: sequential buttons (no gaps), different order
        btn_map = XBOX_SEQ_BUTTON_MAP;
        btn_map_size = sizeof(XBOX_SEQ_BUTTON_MAP) / sizeof(XBOX_SEQ_BUTTON_MAP[0]);
    } else if (map->is_ngc_modkit) {
        // 8BitDo NGC Modkit (PID 0x286A): checked BEFORE the generic is_8bitdo+buttonCnt>14
        // rule below on purpose -- this device ALSO has 16 buttons like the paddle
        // controllers that rule is for, which is exactly the bug this PID-specific check
        // exists to avoid repeating. See NGC_MODKIT_BUTTON_MAP's own comment for evidence.
        btn_map = NGC_MODKIT_BUTTON_MAP;
        btn_map_size = sizeof(NGC_MODKIT_BUTTON_MAP) / sizeof(NGC_MODKIT_BUTTON_MAP[0]);
    } else if (map->is_8bitdo && map->buttonCnt > 14) {
        // 8BitDo with paddles (Ultimate, etc.): R4 at usage 3, L4 at usage 6
        // Models without paddles (SN30 Pro, M30) have ≤14 buttons and use SEQ map
        btn_map = BITDO_BUTTON_MAP;
        btn_map_size = sizeof(BITDO_BUTTON_MAP) / sizeof(BITDO_BUTTON_MAP[0]);
    } else {
        btn_map = SEQ_BUTTON_MAP;
        btn_map_size = sizeof(SEQ_BUTTON_MAP) / sizeof(SEQ_BUTTON_MAP[0]);
    }

    // NSO GameCube-native semantic bits (2026-07-13). Only the 8BitDo NGC Modkit has confirmed
    // evidence for these -- see docs/bluetooth/8bitdo-ngc-diy-profile.md "Raw hardware
    // observations": usage 9 (byte9 0x01) and usage 10 (byte9 0x02) are the TRUE mechanical
    // trigger clicks (confirmed to fire only at full press, composing cleanly with no bit
    // aliasing); usage 11 (byte9 0x04) is Z. Usages 7/8 are deliberately NOT used here -- they
    // are a partial-travel echo that fires well before the true click and stays asserted
    // through it (confirmed: "L full/click" shows byte8=0x40 AND byte9=0x01 simultaneously),
    // so using them would double-fire/false-trigger the detent early. These are independent of
    // (and never OR'd into) `buttons`/btn_map -- they reach the router via their own
    // gc_native_z/gc_l_detent/gc_r_detent fields, not the JP_BUTTON_*/NS2_DST_* remap table,
    // since they are fixed evidence-backed physical mappings for this exact PID, not a
    // user-remappable destination.
    bool gc_native_z = false, gc_l_detent = false, gc_r_detent = false;

    uint8_t buttonCount = 0;
    for (int i = 0; i < BLE_MAX_BUTTONS; i++) {
        if (map->buttonLoc[i].bitMask) {
            buttonCount++;
            bool pressed = map->buttonLoc[i].byteIndex < len &&
                           (data[map->buttonLoc[i].byteIndex] & map->buttonLoc[i].bitMask);
            if (pressed) {
                uint8_t usage = i + 1;  // usage number = slot index + 1
                if (usage < btn_map_size) {
                    buttons |= btn_map[usage];
                }
                if (map->is_ngc_modkit) {
                    if (usage == 9) gc_l_detent = true;
                    else if (usage == 10) gc_r_detent = true;
                    else if (usage == 11) gc_native_z = true;
                }
            }
        }
    }

    // Xbox Elite Series 2: the 4 back paddles live in the last report byte (bits 0-3).
    // They report raw ONLY when left UNMAPPED in the active on-board profile (a mapped
    // paddle sends its assigned button instead). Captured on hardware in byte 19 of a
    // 20-byte report: R4=0x01, R5=0x02, L4=0x04, L5=0x08. Left paddles -> GL, right -> GR.
    // (Byte 17 = active profile 0-3; not mapped — it's a mode selector, not a button.)
    // Xbox Elite Series 2: 20-byte report with the 4 paddles in byte 19 (R4=0x01,
    // R5=0x02, L4=0x04, L5=0x08). Detected by "Xbox + 20-byte report" rather than the
    // exact PID (which the BLE PnP query often fails to resolve); regular Xbox pads send
    // 16-byte reports so they never hit this. Paddles report raw only when the active
    // on-board profile leaves them unmapped.
    if ((map->is_elite2 || map->is_xbox) && len >= 20) {
        uint8_t pad = data[19];
        if (pad & 0x04) buttons |= JP_BUTTON_L4;  // upper-left  paddle -> GL
        if (pad & 0x08) buttons |= JP_BUTTON_L5;  // lower-left  paddle -> GL
        if (pad & 0x01) buttons |= JP_BUTTON_R4;  // upper-right paddle -> GR
        if (pad & 0x02) buttons |= JP_BUTTON_R5;  // lower-right paddle -> GR
    } else if (map->is_xbox && len > 0 && (data[len - 1] & 0x01)) {
        // Xbox extra byte: last byte, bit 0 (outside the HID buttons bitfield).
        // Series X/S Share (16-byte report, BLE) -> A2 ; Xbox One Back (Classic) -> S1.
        if (gp->event.transport == INPUT_TRANSPORT_BT_BLE) {
            buttons |= JP_BUTTON_A2;
        } else {
            buttons |= JP_BUTTON_S1;
        }
    }

    gp->event.buttons = buttons;
    gp->event.button_count = buttonCount;
    gp->event.analog[ANALOG_LX] = lx;
    gp->event.analog[ANALOG_LY] = ly;
    gp->event.analog[ANALOG_RX] = rx;
    gp->event.analog[ANALOG_RY] = ry;
    gp->event.analog[ANALOG_L2] = l2;
    gp->event.analog[ANALOG_R2] = r2;
    gp->event.gc_has_native_layout = map->is_ngc_modkit;
    gp->event.gc_native_z = gc_native_z;
    gp->event.gc_l_detent = gc_l_detent;
    gp->event.gc_r_detent = gc_r_detent;
    // NGC Modkit does NOT need suppress_l2r2_analog_fold: an earlier iteration mapped Z to
    // JP_BUTTON_R2 (colliding with the seam's analog-fold, which also drives JP_BUTTON_R2 from
    // ANALOG_R2) and needed this flag to avoid the conflict. The current design maps Z to
    // JP_BUTTON_R1 instead (see NGC_MODKIT_BUTTON_MAP) and deliberately WANTS the analog fold to
    // keep driving ZL/ZR from the real trigger values -- so nothing to suppress here anymore.
    // suppress_l2r2_analog_fold itself (input_event.h) is left in place as available
    // infrastructure for a future device that has the same kind of collision this one no longer does.

    router_submit_input(&gp->event);
}

// ============================================================================
// DRIVER IMPLEMENTATION
// ============================================================================

static bool gamepad_match(const char* device_name, const uint8_t* class_of_device,
                          uint16_t vendor_id, uint16_t product_id, bool is_ble)
{
    (void)device_name;
    (void)vendor_id;   // Generic driver doesn't use VID/PID
    (void)product_id;

    // BLE devices don't have COD — match any BLE HID device as fallback
    if (is_ble) {
        return true;
    }

    if (!class_of_device) {
        return false;
    }

    // Check for Peripheral major class (0x05)
    uint8_t major_class = (class_of_device[1] >> 0) & 0x1F;
    if (major_class != 0x05) {
        return false;
    }

    // Check for gamepad/joystick in minor class
    uint8_t minor_class = (class_of_device[0] >> 2) & 0x3F;
    uint8_t device_subtype = minor_class & 0x0F;

    // 0x01 = Joystick, 0x02 = Gamepad
    if (device_subtype == 0x01 || device_subtype == 0x02) {
        return true;
    }

    return false;
}

static bool gamepad_init(bthid_device_t* device)
{
    printf("[BTHID_GAMEPAD] Init for device: %s\n", device->name);

    // Find free data slot
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (!gamepad_data[i].initialized) {
            // Initialize input event with defaults
            init_input_event(&gamepad_data[i].event);
            gamepad_data[i].initialized = true;
            gamepad_data[i].has_report_map = false;
            memset(&gamepad_data[i].map, 0, sizeof(ble_report_map_t));

            // Set device info
            gamepad_data[i].event.type = INPUT_TYPE_GAMEPAD;
            gamepad_data[i].event.transport = device->is_ble ? INPUT_TRANSPORT_BT_BLE : INPUT_TRANSPORT_BT_CLASSIC;
            gamepad_data[i].event.dev_addr = device->conn_index;  // Use conn_index as address
            gamepad_data[i].event.instance = 0;

            device->driver_data = &gamepad_data[i];
            return true;
        }
    }

    return false;
}

static void gamepad_process_report(bthid_device_t* device, const uint8_t* data, uint16_t len)
{
    bthid_gamepad_data_t* gp = (bthid_gamepad_data_t*)device->driver_data;
    if (!gp) {
        return;
    }

    // Dynamic path: use parsed HID descriptor for field extraction
    if (gp->has_report_map) {
        // Filter by report ID — skip non-gamepad reports (battery, feature, etc.)
        // that would otherwise be parsed as gamepad data with wrong byte layout
        if (gp->map.report_id && len > 0 && data[0] != gp->map.report_id) {
            return;
        }
        // One-time hex dump of first gamepad report for debugging
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            printf("[BTHID_GAMEPAD] Report (%d bytes):", len);
            for (int i = 0; i < len && i < 20; i++) printf(" %02x", data[i]);
            printf("\n");
            printf("[BTHID_GAMEPAD] Map: X@%d/%04x Y@%d/%04x Z@%d/%04x RZ@%d/%04x RX@%d/%04x RY@%d/%04x hat@%d\n",
                   gp->map.xLoc.byteIndex, gp->map.xLoc.bitMask,
                   gp->map.yLoc.byteIndex, gp->map.yLoc.bitMask,
                   gp->map.zLoc.byteIndex, gp->map.zLoc.bitMask,
                   gp->map.rzLoc.byteIndex, gp->map.rzLoc.bitMask,
                   gp->map.rxLoc.byteIndex, gp->map.rxLoc.bitMask,
                   gp->map.ryLoc.byteIndex, gp->map.ryLoc.bitMask,
                   gp->map.hatLoc.byteIndex);
        }
        process_report_dynamic(gp, data, len);
        return;
    }

    // Fallback: hardcoded 6-byte layout for Classic BT without descriptors
    if (len < 4) {
        return;
    }

    uint32_t raw_buttons = 0;
    if (len >= 1) raw_buttons |= data[0];
    if (len >= 2) raw_buttons |= (uint32_t)data[1] << 8;

    uint32_t buttons = 0;

    if (raw_buttons & 0x0001) buttons |= JP_BUTTON_B1;  // A/Cross
    if (raw_buttons & 0x0002) buttons |= JP_BUTTON_B2;  // B/Circle
    if (raw_buttons & 0x0004) buttons |= JP_BUTTON_B3;  // X/Square
    if (raw_buttons & 0x0008) buttons |= JP_BUTTON_B4;  // Y/Triangle
    if (raw_buttons & 0x0010) buttons |= JP_BUTTON_L1;  // LB
    if (raw_buttons & 0x0020) buttons |= JP_BUTTON_R1;  // RB
    if (raw_buttons & 0x0040) buttons |= JP_BUTTON_L2;  // LT (digital)
    if (raw_buttons & 0x0080) buttons |= JP_BUTTON_R2;  // RT (digital)
    if (raw_buttons & 0x0100) buttons |= JP_BUTTON_S1;  // Select/Back
    if (raw_buttons & 0x0200) buttons |= JP_BUTTON_S2;  // Start
    if (raw_buttons & 0x0400) buttons |= JP_BUTTON_L3;  // LS
    if (raw_buttons & 0x0800) buttons |= JP_BUTTON_R3;  // RS
    if (raw_buttons & 0x1000) buttons |= JP_BUTTON_A1;  // Home/Guide

    gp->event.buttons = buttons;

    // Axes (using analog[] array indices from input_event.h)
    if (len >= 3) gp->event.analog[ANALOG_LX] = data[2];   // Left stick X
    if (len >= 4) gp->event.analog[ANALOG_LY] = data[3];   // Left stick Y
    if (len >= 5) gp->event.analog[ANALOG_RX] = data[4];   // Right stick X
    if (len >= 6) gp->event.analog[ANALOG_RY] = data[5];  // Right stick Y

    // Submit to router
    router_submit_input(&gp->event);
}

// Xbox rumble output report constants
#define XBOX_RUMBLE_REPORT_ID   0x03
#define XBOX_RUMBLE_MOTORS      0x03  // Enable strong (bit 1) + weak (bit 0) main motors

// KNOWN GAP, documented not silently left (found 2026-07-12 tracing a hardware-reported
// "generic/XInput-class controller rumble doesn't work" regression): this generic fallback
// driver matches ANY unrecognized BLE HID gamepad, or any Classic device whose Class-of-Device
// says Peripheral/Joystick/Gamepad (see gamepad_match() above) — a wide, format-unknown net.
// It only ever *sends* a rumble output report for vendor_id == 0x045E (the one format it
// knows, verified against real Xbox hardware). For every other vendor ID matched by this
// generic driver, the block below is skipped entirely and rumble_dirty is still cleared at
// the bottom — an honest "we don't know how to rumble this device," not a bug in the sense of
// broken logic, but a real, user-visible capability gap for non-Xbox-vendor "XInput-class"
// pads. Not fixed here: blindly sending the Xbox-format report to an arbitrary device this
// driver matched (which could have a completely different, unknown output-report shape) is a
// real hardware risk that cannot be validated without a physical device and is not
// evidence-backed the way the Xbox-format assumption is for actual Xbox hardware. If a
// specific generic/XInput pad is confirmed (via a hardware capture of its own HID report
// descriptor) to accept this same format, extend the condition below explicitly for that
// device rather than removing the vendor check outright.
static void gamepad_task(bthid_device_t* device)
{
    bthid_gamepad_data_t* gp = (bthid_gamepad_data_t*)device->driver_data;
    if (!gp) return;

    int player_idx = find_player_index(gp->event.dev_addr, gp->event.instance);
    if (player_idx < 0) return;

    feedback_state_t* fb = feedback_get_state(player_idx);
    if (!fb || !fb->rumble_dirty) return;

    uint8_t left = fb->rumble.left;
    uint8_t right = fb->rumble.right;

    if (left != gp->rumble_left || right != gp->rumble_right) {
        // Xbox controllers (VID 0x045E): Report ID 0x03, 8 bytes. Verified byte-for-byte
        // 2026-07-12 against the Linux xpadneo driver (atar-axis/xpadneo, xpadneo.h's
        // xpadneo_rumble_report/xpadneo_rumble_data — the reference Xbox-BLE/BT HID driver):
        // [0]=enable_actuators, [1]=left_trigger_magnitude, [2]=right_trigger_magnitude,
        // [3]=strong_motor, [4]=weak_motor, [5]=pulse_sustain_10ms, [6]=pulse_release_10ms,
        // [7]=loop_count
        //
        // THIS is the actually-reachable Xbox rumble path — xbox_bt.c/xbox_ble.c look like
        // dedicated Xbox drivers but are never registered (bthid_registry.c: "generic driver
        // handles all Xbox"), so every real Xbox controller lands here regardless of
        // transport. An earlier pass in this same investigation fixed the loop_count bug
        // below in xbox_bt.c/xbox_ble.c first, without realizing those files were dead code —
        // the actual regression was still live here the whole time. Both dead files have
        // since been removed; this is now the only Xbox rumble implementation in the tree.
        if (device->vendor_id == 0x045E) {
            bool stopping = (left == 0 && right == 0);
            uint8_t buf[8];
            // Confirmed 2026-07-14 by direct research into a documented, near-identical bug
            // class (atar-axis/xpadneo issue #400, "8BitDo Pro 2 non-stop rumble on connect",
            // fixed in commit 94ad82a): that bug was root-caused to the enable/motor-mask bits
            // staying set on a stop command, with only the magnitude zeroed -- some controllers'
            // firmware do not reliably treat "enabled, magnitude 0" as equivalent to "disabled".
            // This driver had exactly that shape (buf[0] was unconditionally XBOX_RUMBLE_MOTORS
            // regardless of amplitude) before this fix. Disabling the enable bits entirely on a
            // genuine stop is strictly more correct and costs nothing -- apply it regardless of
            // whether it's confirmed to be *this specific* project's trigger.
            buf[0] = stopping ? 0x00 : XBOX_RUMBLE_MOTORS;
            buf[1] = 0;                              // Left trigger magnitude (0: enable bits don't request trigger motors)
            buf[2] = 0;                              // Right trigger magnitude (same)
            buf[3] = ((uint16_t)left * 100) / 255;   // Strong motor (0-100)
            buf[4] = ((uint16_t)right * 100) / 255;  // Weak motor (0-100)
            // pulse_sustain_10ms: Confirmed 2026-07-14 by real hardware feedback to be the actual
            // bug, distinct from the enable-bits fix above. This was 0xFF (max, ~2550ms PER
            // TRIGGER) regardless of amplitude -- fine for a single isolated rumble command, but
            // real gameplay (Smash Bros) sends a rapid stream of brief, deliberately small/
            // textured rumble ticks, never a single one-shot trigger. Each of those legitimate
            // ticks was independently re-arming a ~2.55s hold with no release gap (buf[6]=0), so
            // any two ticks landing within 2.55s of each other (near-guaranteed for "textured"
            // gameplay rumble sent every few tens of ms) smeared into one continuous motor
            // engagement instead of a series of distinct short buzzes -- reported as "a powerful
            // continuous [rumble]" that "only stops if there's a transition screen where normally
            // nothing would send any rumble signal at all" (i.e. only when NO trigger arrives for
            // a full ~2.55s) and persisting briefly even right after pausing (whatever trigger
            // landed in the last ~2.55s before the pause keeps holding). Shortened to 0x05 (50ms)
            // so each individual trigger decays quickly on its own unless genuinely refreshed
            // faster than that by the host -- long enough to feel like a distinct tick, short
            // enough that a stream of separate small ticks reads as a texture, not one sustained
            // buzz. Still 0 on an explicit stop (see `stopping` above).
            buf[5] = stopping ? 0x00 : 0x05;
            buf[6] = 0x00;                           // pulse_release_10ms: no gap between pulses
            // loop_count: xpadneo sets 0xEB (235) to sustain ~10 minutes from one command
            // ("we pulse the motors for 60 minutes as the Windows driver does" —
            // rumble.c's rumble_worker()). This was 0x00 ("repeat: none"), which likely
            // stopped the motor after a single ~2.55s pulse_sustain burst and never
            // resumed, since this driver (correctly) only resends on an amplitude change.
            // Set to 0 on an explicit stop (see `stopping` above) so a stop command can never
            // itself be interpreted as "sustain this (zero) state for up to 10 minutes" --
            // it should mean "off, now," not "off, for a while." Kept at 0xEB for a genuine
            // trigger -- with pulse_sustain now shortened to 0x05, the worst-case total duration
            // if a trigger is somehow never followed by anything else is 235*50ms ~= 11.75s, not
            // ~10 minutes -- a much safer bound while this is still not fully confirmed.
            buf[7] = stopping ? 0x00 : 0xEB;
            bthid_send_output_report(device->conn_index, XBOX_RUMBLE_REPORT_ID, buf, sizeof(buf));
        }

        gp->rumble_left = left;
        gp->rumble_right = right;
    }

    feedback_clear_dirty(player_idx);
}

static void gamepad_disconnect(bthid_device_t* device)
{
    printf("[BTHID_GAMEPAD] Disconnect: %s\n", device->name);

    bthid_gamepad_data_t* gp = (bthid_gamepad_data_t*)device->driver_data;
    if (gp) {
        // Clear router state first (sends zeroed input report)
        router_device_disconnected(gp->event.dev_addr, gp->event.instance);
        // Remove player assignment
        remove_players_by_address(gp->event.dev_addr, gp->event.instance);

        init_input_event(&gp->event);
        gp->has_report_map = false;
        gp->initialized = false;
    }
}

// ============================================================================
// DRIVER STRUCT
// ============================================================================

const bthid_driver_t bthid_gamepad_driver = {
    .name = "Generic BT Gamepad",
    .transports = BTHID_TRANSPORT_BOTH,  // universal fallback, explicit for clarity
    .match = gamepad_match,
    .init = gamepad_init,
    .process_report = gamepad_process_report,
    .task = gamepad_task,
    .disconnect = gamepad_disconnect,
};

void bthid_gamepad_register(void)
{
    bthid_register_driver(&bthid_gamepad_driver);
}
