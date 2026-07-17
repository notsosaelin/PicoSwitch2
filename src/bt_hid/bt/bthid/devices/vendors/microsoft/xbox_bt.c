// xbox_bt.c - Xbox Bluetooth Controller Driver
// Handles Xbox One/Series controllers over Bluetooth
//
// Reference: Xbox controllers use standard HID over Bluetooth
// Report format is similar to USB but with BT HID report structure

#include "xbox_bt.h"
#include "xbox_bt_report.h"
#include "xbox_rumble.h"
#include "bt/bthid/bthid.h"
#include "bt/transport/bt_transport.h"
#include "core/input_event.h"
#include "core/router/router.h"
#include "core/buttons.h"
#include "core/services/players/manager.h"
#include "core/services/players/feedback.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// XBOX BT CONSTANTS
// ============================================================================

// Share button: NOT in the buttons bitfield — sent as a separate byte (data[16])
// after the standard 16-byte report on Xbox Series X/S controllers
#define XBOX_BT_SHARE           0x01

// Alternative format button masks — older controllers with D-pad in button field
#define XBOX_BT_ALT_DPAD_UP         0x0001
#define XBOX_BT_ALT_DPAD_DOWN       0x0002
#define XBOX_BT_ALT_DPAD_LEFT       0x0004
#define XBOX_BT_ALT_DPAD_RIGHT      0x0008
#define XBOX_BT_ALT_START           0x0010
#define XBOX_BT_ALT_BACK            0x0020
#define XBOX_BT_ALT_LEFT_THUMB      0x0040
#define XBOX_BT_ALT_RIGHT_THUMB     0x0080
#define XBOX_BT_ALT_LEFT_SHOULDER   0x0100
#define XBOX_BT_ALT_RIGHT_SHOULDER  0x0200
#define XBOX_BT_ALT_GUIDE           0x0400
#define XBOX_BT_ALT_A               0x1000
#define XBOX_BT_ALT_B               0x2000
#define XBOX_BT_ALT_X               0x4000
#define XBOX_BT_ALT_Y               0x8000

// ============================================================================
// XBOX BT REPORT STRUCTURE
// ============================================================================

// Xbox BT HID input report (standard gamepad format)
typedef struct __attribute__((packed)) {
    uint8_t report_id;          // Report ID

    uint16_t lx;                // Left stick X (0 to 65535)
    uint16_t ly;                // Left stick Y (0 to 65535)
    uint16_t rx;                // Right stick X (0 to 65535)
    uint16_t ry;                // Right stick Y (0 to 65535)

    uint16_t lt;                // Left trigger (0-1023)
    uint16_t rt;                // Right trigger (0-1023)

    uint8_t dpad;               // D-pad as hat (1=N..8=NW, 0=center)

    uint16_t buttons;           // Button bitfield
} xbox_bt_input_report_t;

// Alternative format some controllers use
typedef struct __attribute__((packed)) {
    uint8_t report_id;

    uint16_t buttons;           // Button bitfield

    uint8_t lt;                 // Left trigger (0-255)
    uint8_t rt;                 // Right trigger (0-255)

    int16_t lx;                 // Left stick X
    int16_t ly;                 // Left stick Y
    int16_t rx;                 // Right stick X
    int16_t ry;                 // Right stick Y
} xbox_bt_input_alt_t;

// ============================================================================
// DRIVER DATA
// ============================================================================

typedef struct {
    input_event_t event;
    bool initialized;
    uint8_t rumble_left;   // Last sent rumble values (for change detection)
    uint8_t rumble_right;
} xbox_bt_data_t;

static xbox_bt_data_t xbox_data[BTHID_MAX_DEVICES];

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Scale 16-bit signed stick value to 8-bit unsigned (1-255, 128 center).
// Retained for the older alternative report format only; the standard report
// is unsigned and decoded by xbox_bt_decode_standard_report().
static uint8_t scale_stick_16to8(int16_t val)
{
    // Scale from [-32768, 32767] to [1, 255]
    int32_t scaled = ((int32_t)val + 32768) / 256;
    if (scaled <= 0) return 1;
    if (scaled >= 255) return 255;
    return (uint8_t)scaled;
}

// ============================================================================
// DRIVER IMPLEMENTATION
// ============================================================================

static bool xbox_match(const char* device_name, const uint8_t* class_of_device,
                       uint16_t vendor_id, uint16_t product_id, bool is_ble)
{
    (void)class_of_device;
    (void)is_ble;

    // Xbox Elite Series 2 has a non-standard HID report layout —
    // let the generic gamepad driver handle it via HID descriptor parsing
    if (vendor_id == 0x045E && (product_id == 0x0B05 || product_id == 0x0B22)) {
        return false;
    }

    // VID match - Microsoft vendor ID = 0x045E
    // Many Xbox controller PIDs exist, so just match VID
    if (vendor_id == 0x045E) {
        return true;
    }

    // Name-based match (fallback)
    if (device_name) {
        if (strstr(device_name, "Xbox Wireless Controller") != NULL) {
            return true;
        }
        if (strstr(device_name, "Xbox Elite") != NULL) {
            return true;
        }
        if (strstr(device_name, "Xbox Adaptive") != NULL) {
            return true;
        }
        if (strstr(device_name, "Microsoft") != NULL &&
            strstr(device_name, "Controller") != NULL) {
            return true;
        }
    }

    return false;
}

static bool xbox_init(bthid_device_t* device)
{
    printf("[XBOX_BT] Init for device: %s\n", device->name);

    // Find free data slot
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (!xbox_data[i].initialized) {
            init_input_event(&xbox_data[i].event);
            xbox_data[i].initialized = true;

            xbox_data[i].event.type = INPUT_TYPE_GAMEPAD;
            xbox_data[i].event.transport = INPUT_TRANSPORT_BT_CLASSIC;
            xbox_data[i].event.dev_addr = device->conn_index;
            xbox_data[i].event.instance = 0;
            xbox_data[i].event.button_count = 10;
            xbox_data[i].event.layout = LAYOUT_MODERN_4FACE;
            xbox_data[i].rumble_left = 0;
            xbox_data[i].rumble_right = 0;

            device->driver_data = &xbox_data[i];

            return true;
        }
    }

    return false;
}

static void xbox_process_report(bthid_device_t* device, const uint8_t* data, uint16_t len)
{
    xbox_bt_data_t* xbox = (xbox_bt_data_t*)device->driver_data;
    if (!xbox || len < 2) return;

    // Only parse report ID 0x01 (standard gamepad input)
    uint8_t report_id = data[0];
    if (report_id != 0x01) {
        return;
    }

    uint32_t buttons = 0x00000000;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    uint8_t lt = 0, rt = 0;

    // Xbox BT controllers can send different report formats
    // Try to detect based on report length and parse accordingly

    if (len >= sizeof(xbox_bt_input_report_t)) {
        xbox_bt_decoded_report_t decoded;
        if (!xbox_bt_decode_standard_report(data, len, &decoded)) {
            return;
        }
        buttons = decoded.buttons;
        lx = decoded.lx;
        ly = decoded.ly;
        rx = decoded.rx;
        ry = decoded.ry;
        lt = decoded.lt;
        rt = decoded.rt;
        xbox->event.gc_has_native_layout = false;
        xbox->event.gc_native_zl = false;
        xbox->event.gc_native_z = false;
        xbox->event.gc_l_detent = false;
        xbox->event.gc_r_detent = false;

        // Share button: Xbox Series X/S sends it as an extra byte after the
        // standard 16-byte report (byte 16, bit 0) — not in the buttons bitfield
        if (len > 16 && (data[16] & XBOX_BT_SHARE)) buttons |= JP_BUTTON_A2;

    } else if (len >= sizeof(xbox_bt_input_alt_t)) {
        xbox->event.gc_has_native_layout = false;
        xbox->event.gc_native_zl = false;
        xbox->event.gc_native_z = false;
        xbox->event.gc_l_detent = false;
        xbox->event.gc_r_detent = false;
        // Alternative format (older controllers or different firmware)
        const xbox_bt_input_alt_t* rpt = (const xbox_bt_input_alt_t*)data;

        // Parse sticks
        lx = scale_stick_16to8(rpt->lx);
        ly = scale_stick_16to8(-rpt->ly);
        rx = scale_stick_16to8(rpt->rx);
        ry = scale_stick_16to8(-rpt->ry);

        // Parse triggers (already 8-bit)
        lt = rpt->lt;
        rt = rpt->rt;

        // Parse buttons (button bitfield in different position)
        uint16_t btn = rpt->buttons;

        // D-pad is in button bitfield for this format
        if (btn & XBOX_BT_ALT_DPAD_UP)        buttons |= JP_BUTTON_DU;
        if (btn & XBOX_BT_ALT_DPAD_DOWN)      buttons |= JP_BUTTON_DD;
        if (btn & XBOX_BT_ALT_DPAD_LEFT)      buttons |= JP_BUTTON_DL;
        if (btn & XBOX_BT_ALT_DPAD_RIGHT)     buttons |= JP_BUTTON_DR;
        if (btn & XBOX_BT_ALT_A)              buttons |= JP_BUTTON_B1;
        if (btn & XBOX_BT_ALT_B)              buttons |= JP_BUTTON_B2;
        if (btn & XBOX_BT_ALT_X)              buttons |= JP_BUTTON_B3;
        if (btn & XBOX_BT_ALT_Y)              buttons |= JP_BUTTON_B4;
        if (btn & XBOX_BT_ALT_LEFT_SHOULDER)  buttons |= JP_BUTTON_L1;
        if (btn & XBOX_BT_ALT_RIGHT_SHOULDER) buttons |= JP_BUTTON_R1;
        if (lt > 10)                           buttons |= JP_BUTTON_L2;
        if (rt > 10)                           buttons |= JP_BUTTON_R2;
        if (btn & XBOX_BT_ALT_BACK)           buttons |= JP_BUTTON_S1;
        if (btn & XBOX_BT_ALT_START)          buttons |= JP_BUTTON_S2;
        if (btn & XBOX_BT_ALT_LEFT_THUMB)     buttons |= JP_BUTTON_L3;
        if (btn & XBOX_BT_ALT_RIGHT_THUMB)    buttons |= JP_BUTTON_R3;
        if (btn & XBOX_BT_ALT_GUIDE)          buttons |= JP_BUTTON_A1;

    } else {
        // Unknown format, skip
        return;
    }

    // Update event
    xbox->event.buttons = buttons;
    xbox->event.analog[ANALOG_LX] = lx;
    xbox->event.analog[ANALOG_LY] = ly;
    xbox->event.analog[ANALOG_RX] = rx;
    xbox->event.analog[ANALOG_RY] = ry;
    xbox->event.analog[ANALOG_L2] = lt;
    xbox->event.analog[ANALOG_R2] = rt;

    // Submit to router
    router_submit_input(&xbox->event);
}

// Rumble output report constants — same HID report descriptor as Xbox BLE (same
// hardware/firmware family, see the file header comment), so the same report ID/layout
// applies over Classic BT. Report ID 0x03, 8 bytes, verified byte-for-byte 2026-07-12
// against the Linux xpadneo driver (atar-axis/xpadneo, xpadneo.h's
// xpadneo_rumble_report/xpadneo_rumble_data — the reference Xbox-BLE/BT HID driver):
// [0]=enable_actuators, [1]=left_trigger_magnitude, [2]=right_trigger_magnitude,
// [3]=strong_motor, [4]=weak_motor, [5]=pulse_sustain_10ms, [6]=pulse_release_10ms,
// [7]=loop_count. Magnitude range is 0-100 per the HID descriptor.
static void xbox_task(bthid_device_t* device)
{
    // Found 2026-07-12 tracing a hardware-reported "Xbox rumble doesn't work" regression:
    // this was a complete no-op — the comment below claimed rumble was "handled through HID
    // output reports when needed," but nothing in this file ever called feedback_get_state()
    // or sent an output report. xbox_ble.c (the BLE variant of this same driver family) already
    // has this working; mirrored here for Classic BT using the same report format.
    xbox_bt_data_t* xbox = (xbox_bt_data_t*)device->driver_data;
    if (!xbox) return;

    int player_idx = find_player_index(xbox->event.dev_addr, xbox->event.instance);
    if (player_idx < 0) return;

    feedback_state_t* fb = feedback_get_state(player_idx);
    if (!fb) return;

    if (fb->rumble_dirty) {
        uint8_t left = fb->rumble.left;
        uint8_t right = fb->rumble.right;
        if (left != xbox->rumble_left || right != xbox->rumble_right) {
            uint8_t buf[XBOX_RUMBLE_DATA_LEN];
            xbox_rumble_build_payload(left, right, buf);
            if (!bthid_send_output_report(device->conn_index, XBOX_RUMBLE_REPORT_ID,
                                           buf, sizeof(buf))) {
                // Keep rumble_dirty set and the last-sent cache unchanged. In particular,
                // a failed GameCube OFF/STOP must be retried instead of remembered as sent.
                return;
            }
            xbox->rumble_left = left;
            xbox->rumble_right = right;
        }
        feedback_clear_dirty(player_idx);
    }
}

static void xbox_disconnect(bthid_device_t* device)
{
    printf("[XBOX_BT] Disconnect: %s\n", device->name);

    xbox_bt_data_t* xbox = (xbox_bt_data_t*)device->driver_data;
    if (xbox) {
        // Clear router state first (sends zeroed input report)
        router_device_disconnected(xbox->event.dev_addr, xbox->event.instance);
        // Remove player assignment
        remove_players_by_address(xbox->event.dev_addr, xbox->event.instance);

        init_input_event(&xbox->event);
        xbox->initialized = false;
    }
}

// ============================================================================
// DRIVER STRUCT
// ============================================================================

const bthid_driver_t xbox_bt_driver = {
    .name = "Xbox Wireless Controller",
    // Classic BT half of the same physical Xbox controller family as xbox_ble.c —
    // see that file's .transports comment for why this is now declared explicitly
    // instead of relying only on registration order.
    .transports = BTHID_TRANSPORT_CLASSIC,
    .match = xbox_match,
    .init = xbox_init,
    .process_report = xbox_process_report,
    .task = xbox_task,
    .disconnect = xbox_disconnect,
};

void xbox_bt_register(void)
{
    bthid_register_driver(&xbox_bt_driver);
}
