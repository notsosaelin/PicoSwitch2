/*
 * Joy-Con 2 USB emulation. See include/switch_joycon2.h for the module contract and evidence
 * tier of each entry point. Only compiled under -DNS2_PRO, mirroring switch_gc.c's own scoping.
 */
#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "tusb.h"

#include "report.h"       // report_set_rumble, get_global_gamepad_input
#include "switch_joycon2.h"
#include "switch_joycon2_encode.h"
#include "ns2_pairing_crypto.h"
#include "ns2_joycon2_identity.h"
#include "config.h"
#include "switch_pro.h"
#include "usb.h"

#ifdef NS2_PRO

static joycon2_side_t s_side = JOYCON2_SIDE_LEFT;

void switch_joycon2_set_side(joycon2_side_t side) { s_side = side; }
joycon2_side_t switch_joycon2_get_side(void) { return s_side; }

//--------------------------------------------------------------------+
// Descriptors -- Confirmed byte-exact, live USBPcap capture 2026-07-14 (both device and
// configuration descriptors), cross-validated byte-for-byte against
// ndeadly/switch2_controller_research's own independently-captured descriptors.md. See
// docs/switch2-joycon2/protocol.md "USB descriptors" / "USB identity" for the full citation and
// raw capture files.
//--------------------------------------------------------------------+

static const uint8_t switch_joycon2_device_desc_l[] = {
    0x12,        // bLength
    0x01,        // bDescriptorType (Device)
    0x00, 0x02,  // bcdUSB 2.00
    0xEF,        // bDeviceClass (Misc, IAD composite)
    0x02,        // bDeviceSubClass
    0x01,        // bDeviceProtocol
    0x40,        // bMaxPacketSize0 64
    0x7E, 0x05,  // idVendor 0x057E (Nintendo)
    0x67, 0x20,  // idProduct 0x2067 (Joy-Con 2 Left)
    0x10, 0x01,  // bcdDevice 1.10 -- DELIBERATELY NOT the real captured value (1.00, bytes
                 // 00,01). Confirmed 2026-07-14 by direct hardware evidence, real-time: this
                 // project's first hardware test connected the Pico's Joy-Con2(L) personality to
                 // the SAME machine that has genuine Joy-Con 2 L/R hardware already connected
                 // (used earlier the same session for the SPI dump/USB captures this whole
                 // personality is built from) -- it enumerated under "Other devices" with Code 28
                 // ("no compatible drivers"), the exact symptom Pro2's and GameCube's own
                 // bcdDevice fixes both already solved. Root cause (established there, carried
                 // over here by direct analogy, not yet independently re-verified after this
                 // fix): Windows keys its WinUSB driver-binding cache on VID+PID+bcdDevice: using
                 // the real captured bcdDevice made this device indistinguishable from the
                 // genuine unit's own cached binding. Fixed the same way both prior personalities
                 // were: a plausible-looking but deliberately different minor version.
    0x01,        // iManufacturer
    0x02,        // iProduct
    0x03,        // iSerialNumber
    0x01,        // bNumConfigurations
};

static const uint8_t switch_joycon2_device_desc_r[] = {
    0x12, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40,
    0x7E, 0x05,  // idVendor 0x057E
    0x66, 0x20,  // idProduct 0x2066 (Joy-Con 2 Right)
    0x10, 0x01,  // bcdDevice 1.10 -- same deliberate deviation as Left, same reasoning (see
                 // that descriptor's own comment above; Right was not itself hardware-tested
                 // this pass, but shares the identical real captured bcdDevice and collision risk).
    0x01, 0x02, 0x03, 0x01,
};

// Configuration descriptor: byte-for-byte identical between Left and Right (only the device
// descriptor's PID differs) -- Confirmed, same capture as above.
#define SWITCH_JOYCON2_CONFIG_LEN 80
static const uint8_t switch_joycon2_config_desc[] = {
    0x09, 0x02, (SWITCH_JOYCON2_CONFIG_LEN & 0xFF), (SWITCH_JOYCON2_CONFIG_LEN >> 8),
    0x02,        // bNumInterfaces
    0x01,        // bConfigurationValue
    0x04,        // iConfiguration (string index 4 -- text not captured)
    0xC0,        // bmAttributes: self-powered, no remote wakeup
    0xFA,        // bMaxPower 500mA
    // IAD + Interface 0: HID
    0x08, 0x0B, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x05,  // iInterface=5 ("If_Hid", Confirmed)
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x64, 0x00,  // HID desc, bcdHID 1.11, report len 100
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x04,  // EP 0x81 interrupt IN,  64B, bInterval 4
    0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x04,  // EP 0x01 interrupt OUT, 64B, bInterval 4
    // IAD + Interface 1: vendor bulk
    0x08, 0x0B, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x01, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x06,  // iInterface=6 ("Joy-Con 2 (L/R)")
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,  // EP 0x02 bulk OUT, 64B
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,  // EP 0x82 bulk IN,  64B
};
_Static_assert(sizeof(switch_joycon2_config_desc) == SWITCH_JOYCON2_CONFIG_LEN,
               "Joy-Con 2 configuration descriptor size must match wTotalLength");

// HID report descriptor (100 bytes) -- Confirmed, live capture across a physical replug
// (docs/experiments/joycon2-captures/genuine-controller-full-enumeration-replug-2026-07-14.pcap),
// cross-validated against ndeadly's own independently-captured descriptor. Left and Right differ
// in exactly one byte: the Report ID tag for the structured "extended" input report (7 vs 8).
static const uint8_t switch_joycon2_report_desc_l[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x05,        //   Report ID (5)
    0x05, 0xFF,        //   Usage Page (Reserved 0xFF)
    0x09, 0x01,        //   Usage (0x01)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x95, 0x3F,        //   Report Count (63)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x85, 0x07,        //   Report ID (7) -- Left
    0x09, 0x01,        //   Usage (0x01)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x10,        //   Usage Maximum (0x10)
    0x25, 0x01,        //   Logical Maximum (1)
    0x95, 0x10,        //   Report Count (16)
    0x75, 0x01,        //   Report Size (1)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0xFF,        //   Usage Page (Reserved 0xFF)
    0x09, 0x01,        //   Usage (0x01)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x26, 0xFF, 0x0F,  //     Logical Maximum (4095)
    0x95, 0x02,        //     Report Count (2)
    0x75, 0x0C,        //     Report Size (12)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0xC0,              //   End Collection
    0x05, 0xFF,        //   Usage Page (Reserved 0xFF)
    0x09, 0x02,        //   Usage (0x02)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x95, 0x37,        //   Report Count (55)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (0x01)
    0x95, 0x3F,        //   Report Count (63)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0xC0,              // End Collection
};
_Static_assert(sizeof(switch_joycon2_report_desc_l) == 100,
               "Joy-Con 2 HID report descriptor must be 100 bytes (Confirmed wDescriptorLength)");

// Identical to the Left descriptor except byte 24 (Report ID 7 -> 8).
static uint8_t switch_joycon2_report_desc_r[sizeof(switch_joycon2_report_desc_l)];
static bool switch_joycon2_report_desc_r_built = false;

static const uint8_t *switch_joycon2_report_desc_right(void) {
    if (!switch_joycon2_report_desc_r_built) {
        memcpy(switch_joycon2_report_desc_r, switch_joycon2_report_desc_l,
               sizeof(switch_joycon2_report_desc_l));
        switch_joycon2_report_desc_r[24] = 0x08;  // Report ID 8 -- Right (Confirmed, matches
                                                   // ndeadly's own independently-captured R
                                                   // descriptor exactly)
        switch_joycon2_report_desc_r_built = true;
    }
    return switch_joycon2_report_desc_r;
}

// Strings -- Manufacturer/Serial Confirmed (ndeadly's descriptors.md, cross-checked structurally
// against this project's own captured index values); Product and IF1 iInterface text Confirmed
// directly from this project's own live capture for Left, and from ndeadly's descriptors.md for
// Right (not independently re-captured by this project for Right, per the project owner's
// direction that the Left capture already settled the pattern). "If_Hid" Confirmed live for
// Left, identical string GameCube also uses for its own HID interface.
static const char *switch_joycon2_strings_l[] = {
    (const char[]){0x09, 0x04},          // 0: language id (en-US)
    "Nintendo",                          // 1: manufacturer -- Confirmed
    "Joy-Con 2 (L)",                     // 2: product -- Confirmed
    "00",                                // 3: serial -- Confirmed (literal, not a real per-unit serial)
    NULL,                                // 4: iConfiguration -- not captured; stalls this index
    "If_Hid",                            // 5: iInterface (IF0, HID) -- Confirmed live
    "Joy-Con 2 (L)",                     // 6: iInterface (IF1, vendor) -- Confirmed live
};

static const char *switch_joycon2_strings_r[] = {
    (const char[]){0x09, 0x04},
    "Nintendo",
    "Joy-Con 2 (R)",
    "00",
    NULL,
    "If_Hid",
    "Joy-Con 2 (R)",
};

const uint8_t *switch_joycon2_device_descriptor(void) {
    return s_side == JOYCON2_SIDE_LEFT ? switch_joycon2_device_desc_l : switch_joycon2_device_desc_r;
}
const uint8_t *switch_joycon2_config_descriptor(void) { return switch_joycon2_config_desc; }
const uint8_t *switch_joycon2_hid_report_descriptor(void) {
    return s_side == JOYCON2_SIDE_LEFT ? switch_joycon2_report_desc_l : switch_joycon2_report_desc_right();
}
const char **switch_joycon2_string_table(size_t *count) {
    *count = sizeof(switch_joycon2_strings_l) / sizeof(switch_joycon2_strings_l[0]);
    return (const char **)(s_side == JOYCON2_SIDE_LEFT ? switch_joycon2_strings_l : switch_joycon2_strings_r);
}

//--------------------------------------------------------------------+
// Microsoft OS 1.0 WinUSB auto-bind descriptors, matching the other vendor-interface
// personalities. Windows-specific questions are tracked in docs/switch2-joycon2/open-questions.md.
//--------------------------------------------------------------------+

#define JOYCON2_MS_OS_VENDOR_CODE 0x21  // distinct from Pro2's/GameCube's own values -- these
                                        // only need to be internally self-consistent per
                                        // personality, never simultaneously active, see
                                        // switch_gc.c's identical reasoning.

static const uint16_t switch_joycon2_ms_os_str[] = {
    0x0312, 'M', 'S', 'F', 'T', '1', '0', '0', JOYCON2_MS_OS_VENDOR_CODE};

const uint16_t *switch_joycon2_ms_os_string_descriptor(void) {
    return switch_joycon2_ms_os_str;
}

static const uint8_t switch_joycon2_ms_compat_id[] = {
    0x28, 0x00, 0x00, 0x00,              // dwLength = 40
    0x00, 0x01,                          // bcdVersion 1.00
    0x04, 0x00,                          // wIndex = 0x0004 (Compatible ID)
    0x01,                                // bCount = 1 function section
    0, 0, 0, 0, 0, 0, 0,                 // reserved[7]
    0x01, 0x01,                          // bFirstInterfaceNumber = 1, reserved = 1
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,  // compatibleID
    0, 0, 0, 0, 0, 0, 0, 0,              // subCompatibleID
    0, 0, 0, 0, 0, 0,                    // reserved[6]
};
_Static_assert(sizeof(switch_joycon2_ms_compat_id) == 40, "MS compat ID descriptor must be 40 bytes");

// Extended Properties OS feature descriptor: register the WinUSB device-interface GUID for IF1.
// The Compatible ID descriptor above is sufficient to load WinUSB, but it does not create the
// discoverable device interface that libusb (and therefore SDL/Steam) needs in order to open IF1.
// Windows requests this descriptor with the same vendor code and wIndex=0x0005. The GUID is the
// value observed on the working Joy-Con 2 (R) WinUSB node on 2026-07-16; one device-family GUID is
// intentionally shared by both sides.
static const uint8_t switch_joycon2_ms_ext_props[] = {
    // Header (10 bytes)
    0x8E, 0x00, 0x00, 0x00,              // dwLength = 142
    0x00, 0x01,                          // bcdVersion = 1.00
    0x05, 0x00,                          // wIndex = 0x0005 (Extended Properties)
    0x01, 0x00,                          // wCount = 1 custom property

    // Custom property section (132 bytes)
    0x84, 0x00, 0x00, 0x00,              // dwSize = 132
    0x01, 0x00, 0x00, 0x00,              // dwPropertyDataType = REG_SZ
    0x28, 0x00,                          // wPropertyNameLength = 40 bytes
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 0, 0,
    0x4E, 0x00, 0x00, 0x00,              // dwPropertyDataLength = 78 bytes
    '{', 0, '6', 0, 'F', 0, '1', 0, '3', 0, '7', 0, '2', 0, '5', 0, 'E', 0,
    '-', 0, 'E', 0, 'F', 0, '0', 0, 'E', 0,
    '-', 0, '4', 0, 'F', 0, 'D', 0, '3', 0,
    '-', 0, 'A', 0, 'E', 0, '5', 0, 'F', 0,
    '-', 0, 'B', 0, '2', 0, 'D', 0, 'E', 0, '9', 0, '8', 0, '9', 0, 'E', 0,
    'C', 0, '8', 0, '2', 0, '5', 0, '}', 0, 0, 0,
};
_Static_assert(sizeof(switch_joycon2_ms_ext_props) == 142,
               "MS extended properties descriptor must be 142 bytes");

//--------------------------------------------------------------------+
// Input report construction -- Stage C. Field layout Confirmed (see switch_joycon2_encode.c's
// own citations); the actual encoders are pure/host-testable, this is just the runtime glue.
//--------------------------------------------------------------------+

static uint8_t s_report_counter = 0;
static uint32_t s_report05_counter = 0;
static uint8_t s_selected_report_id = 0;  // 0 = none selected yet (matches switch_gc.c's
                                           // GC_REPORT_ID_NONE convention; 0 is never a valid
                                           // Joy-Con 2 input report ID)
#define JOYCON2_DEFAULT_FEATURE_MASK 0x37u  // buttons, sticks, IMU, mouse, rumble
static uint8_t s_feature_mask = JOYCON2_DEFAULT_FEATURE_MASK;
static uint8_t s_enabled_features = 0;
static uint16_t s_mouse_motion_timing = 0;
static int8_t s_mouse_scroll_direction = 0;
static uint32_t s_mouse_scroll_until_ms = 0;

static void switch_joycon2_reset_mouse_runtime(void) {
    s_mouse_motion_timing = 0;
    s_mouse_scroll_direction = 0;
    s_mouse_scroll_until_ms = 0;
}

static void switch_joycon2_update_mouse_scroll(switch_pro_input_t *in, uint32_t now_ms) {
    if (!in->mouse_enabled || !in->has_mouse) {
        // Disconnect and source changes must end a held wheel deflection
        // immediately; otherwise a stale mouse could keep navigating after a
        // gamepad takes the slot.
        switch_joycon2_reset_mouse_runtime();
        in->mouse_scroll = 0;
        return;
    }

    if (in->mouse_delta_wheel != 0) {
        int8_t direction = in->mouse_delta_wheel > 0 ? 1 : -1;
        uint32_t notches = in->mouse_delta_wheel > 0
            ? (uint32_t)in->mouse_delta_wheel
            : (uint32_t)(-(int16_t)in->mouse_delta_wheel);
        uint32_t duration_ms = notches * 40u;
        if (duration_ms > 400u) duration_ms = 400u;

        // Repeated notches in the same direction extend the current pulse.
        // Reversing direction starts a new pulse immediately.
        uint32_t base = now_ms;
        if (direction == s_mouse_scroll_direction &&
            (int32_t)(s_mouse_scroll_until_ms - now_ms) > 0) {
            base = s_mouse_scroll_until_ms;
        }
        s_mouse_scroll_direction = direction;
        s_mouse_scroll_until_ms = base + duration_ms;
    }

    if ((int32_t)(s_mouse_scroll_until_ms - now_ms) > 0)
        in->mouse_scroll = s_mouse_scroll_direction;
    else {
        s_mouse_scroll_direction = 0;
        s_mouse_scroll_until_ms = 0;
        in->mouse_scroll = 0;
    }
}

static void switch_joycon2_build_report(uint8_t *p) {
    switch_pro_input_t in;
    // Consume relative mouse deltas on every extended report, even while the
    // feature is disabled, so enabling cannot release stale motion as a burst.
    take_global_gamepad_input(0, &in);
    in.mouse_enabled = (s_enabled_features & 0x10) != 0;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    switch_joycon2_update_mouse_scroll(&in, now_ms);
    if (in.mouse_enabled && in.has_mouse) {
        // The native motion clock advances at 800 Hz; extended reports are
        // nominally 250 Hz, so three ticks per emitted report matches the
        // working reference closely and gives the console a stable sideways
        // posture signal for mouse-mode activation.
        s_mouse_motion_timing = (uint16_t)(s_mouse_motion_timing + 3u);
        if (s_mouse_motion_timing == 0) s_mouse_motion_timing = 3;
        in.mouse_motion_timing = s_mouse_motion_timing;
    } else {
        in.mouse_motion_timing = 0;
    }
    switch_joycon2_encode_report(&in, get_global_raw_buttons(0), s_side, s_report_counter++, p);
}

static void switch_joycon2_build_report05(uint8_t *p) {
    switch_pro_input_t in;
    // Absolute mouse report-0x05 is not implemented yet; still consume relative
    // events so a later switch to 0x07/0x08 cannot replay stale movement.
    take_global_gamepad_input(0, &in);
    switch_joycon2_encode_report05(&in, get_global_raw_buttons(0), s_side,
                                    s_report05_counter++, p);
}

//--------------------------------------------------------------------+
// EP0 vendor control -- Nintendo Switch 2 console identity handshake. Both side personalities now
// pass real-console enumeration and streaming; opaque field semantics remain documented outside
// source in docs/switch2-joycon2/open-questions.md.
//--------------------------------------------------------------------+

// Identity block (64 B), returned for both bRequest=3 and SPI reads at 0x13000. Structurally
// modeled on this project's own genuine-unit SPI dump analysis
// (docs/experiments/joycon2-spi-dump-analysis-2026-07-14.md) -- header, per-model type code
// ("HB"=Left/"HC"=Right, Confirmed from real flash at 0x13002), the 12-byte serial field at
// 0x13004-0x1300F (Confirmed "W" + 11 digits, same shape as Pro2/GC's own), 2 reserved zero bytes,
// VID/PID (Confirmed at 0x13012-0x13015), and the fixed "01 08"-shaped bytes observed immediately
// after PID on the real dump. Per this project's established exclusion policy (matching
// switch_gc.c's own fictitious serial), the serial field is a FICTITIOUS value, not either real
// dumped unit's actual serial -- structurally plausible but deliberately wrong so it can never
// collide with real hardware. Body/button/grip use genuine values; the highlight at 0x1301F is
// filled from the per-side user configuration when this personality starts.
//
// Fixed 2026-07-14: the identity template previously used only 9 digits after 'W' (10-byte serial) instead
// of the Confirmed 11-digit/12-byte shape -- 2 bytes short, silently shifting VID, PID, and every
// byte after them 2 positions earlier than the real dump's actual layout (offset 16/18 instead of
// the Confirmed 18/20). The project owner reported Joy-Con 2 (L)/(R) never showing the "Paired"
// notification a genuine console gives Pro2/GameCube -- unlike the earlier vendor-bulk-command gap
// (case 0x11/0x18, already fixed), which only affects the *streaming* phase after pairing, a
// misaligned identity block corrupts the very data (VID/PID) the console's initial recognition
// handshake reads, a much more plausible explanation for never reaching "Paired" at all.
static uint8_t switch_joycon2_ctrl_identity_active[64];

static void switch_joycon2_build_identity(void) {
    bool right = s_side == JOYCON2_SIDE_RIGHT;
    uint8_t accent[3];
    config_get_joycon2_accent(right, accent);
    ns2_joycon2_build_identity(right, accent, switch_joycon2_ctrl_identity_active);
}

static const uint8_t *switch_joycon2_ctrl_identity(void) {
    return switch_joycon2_ctrl_identity_active;
}

// Both version-bearing surfaces are rebuilt together on personality reset.
// Current values/type bytes are from a live genuine Joy-Con 2 query via the
// UART↔BLE bridge, not inherited from the older reference capture.
static const uint8_t switch_joycon2_unit_id[6] = {
    0x02, 0xBB, 0x5E, 0xAB, 0xA9, 0x3C};
static uint8_t switch_joycon2_ctrl_info[NS2_JOYCON2_EP0_INFO_LEN];
static uint8_t switch_joycon2_firmware_info[NS2_JOYCON2_COMMAND_INFO_LEN];

static const uint8_t JOYCON2_DEVICE_KEY_B1[16] = {
    0x5C, 0xF6, 0xEE, 0x79, 0x2C, 0xDF, 0x05, 0xE1,
    0xBA, 0x2B, 0x63, 0x25, 0xC4, 0x1A, 0x5F, 0x10};  // shared working family key; see open questions
static uint8_t s_joycon2_ltk[16];

// Factory/SPI memory read emulation, mirroring switch_gc_mem_read()'s approach but using
// Joy-Con 2's own Confirmed factory-data findings: only ONE stick-calibration slot is ever
// populated on real hardware (Confirmed, joycon2-spi-dump-analysis-2026-07-14.md §3.8 -- a lone
// Joy-Con has exactly one physical stick), so the second slot deliberately reads back 0xFF
// (unprogrammed) rather than a synthetic value, unlike GameCube's own two-slot synthetic
// duplication. Stick calibration itself is synthetic (reused from switch_pro2.c's own proven
// working shape), NOT either real dumped unit's actual per-unit calibration.
static const uint8_t switch_joycon2_synthetic_cal_blk[40] = {
    0x01, 0xAD, 0xD9, 0x9A, 0x55, 0x56, 0x65, 0xA0, 0x00, 0x0A, 0xA0, 0x00, 0x0A, 0xE2,
    0x20, 0x0E, 0xE2, 0x20, 0x0E, 0x9A, 0xAD, 0xD9, 0x9A, 0xAD, 0xD9, 0x0A, 0xA5, 0x50,
    0x0A, 0xA5, 0x50, 0x2F, 0xF6, 0x62, 0x2F, 0xF6, 0x62, 0x0A, 0xFF, 0xFF};

static void switch_joycon2_mem_read(uint32_t addr, uint8_t len, uint8_t *out) {
    const uint8_t *identity = switch_joycon2_ctrl_identity();
    for (uint8_t i = 0; i < len; i++) {
        uint32_t a = addr + i;
        if (a >= 0x13000 && a < 0x13000 + 64) {
            out[i] = identity[a - 0x13000];
        } else if (a >= 0x13080 && a < 0x13080 + 40) {
            out[i] = switch_joycon2_synthetic_cal_blk[a - 0x13080];  // the one real stick's cal
        } else if (a >= 0x130C0 && a < 0x130C0 + 40) {
            out[i] = 0xFF;  // second slot -- Confirmed unprogrammed on real hardware, don't
                             // synthesize a value here (see this function's own comment)
        } else if (a >= 0x13100 && a < 0x13100 + 24) {
            out[i] = 0x00;  // motion calibration bias -- safe "no bias" default (0.0f pattern)
        } else if (a == 0x1FA000) {
            out[i] = 0x00;  // Bluetooth pairing entry count = 0 (no bonds over USB)
        } else {
            out[i] = 0xFF;  // uninitialised/erased flash
        }
    }
}

static uint32_t s_bulk_cmd_count = 0;

static void switch_joycon2_vendor_dispatch(const uint8_t *c, uint32_t n) {
    if (n < 8) return;
    uint8_t id = c[0], transport = c[2], sub = c[3];
    s_bulk_cmd_count++;

    static uint32_t last_unknown_log_ms = 0;

    uint8_t r[64];
    memset(r, 0, sizeof(r));
    r[0] = id;
    r[1] = 0x01;
    r[2] = transport;
    r[3] = sub;
    r[4] = 0x00;
    r[5] = 0xF8;
    uint8_t *d = &r[8];
    uint16_t dl = 0;  // default: bare 8-byte ACK -- never silent, matches switch_gc.c's own
                      // `default:` case.

    switch (id) {
    case 0x03:
        if (sub == 0x0D || sub == 0x03) {  // Initialise USB / Enable USB HID Reports
            d[0] = 0x01;
            dl = 4;
        } else if (sub == 0x0A) {  // Select Input Report -- accepts 0x05 or this side's own
                                    // extended report ID (7 Left / 8 Right); anything else is
                                    // ACKed but does not arm streaming, matching switch_gc.c's
                                    // documented "invalid report IDs are ignored" semantics.
            uint8_t extended_id = (s_side == JOYCON2_SIDE_LEFT) ? 0x07 : 0x08;
            if (n > 8 && (c[8] == 0x05 || c[8] == extended_id)) {
                s_selected_report_id = c[8];
            }
        }
        break;
    case 0x02: {  // SPI/flash memory
        uint32_t addr = (uint32_t)c[12] | ((uint32_t)c[13] << 8) |
                        ((uint32_t)c[14] << 16) | ((uint32_t)c[15] << 24);
        if (sub == 0x04) {  // memory read, variable length
            uint8_t len = c[8];
            if (len > 0x28) len = 0x28;  // r[] is 64 bytes total; 8 (header) + 4 (echo) + 0x28 fits
            d[0] = len;
            d[4] = c[12]; d[5] = c[13]; d[6] = c[14]; d[7] = c[15];
            switch_joycon2_mem_read(addr, len, &d[8]);
            dl = (uint16_t)(8 + len);
        } else if (sub == 0x01) {  // read a fixed 0x28-byte block
            d[0] = 0x28;
            d[4] = c[12]; d[5] = c[13]; d[6] = c[14]; d[7] = c[15];
            switch_joycon2_mem_read(addr, 0x28, &d[8]);
            dl = 8 + 0x28;
        } else if (sub == 0x05) {  // memory write -> ack (never persisted)
            d[4] = c[12]; d[5] = c[13]; d[6] = c[14]; d[7] = c[15];
            dl = 8;
        }
        break;
    }
    case 0x0C:  // feature select -- structurally mirrors switch_gc.c/switch_pro2.c exactly;
                // Joy-Con 2 uses the 10 78 status and level 0x03 for its
                // sensor-backed capabilities (including the optical mouse).
        r[4] = 0x10;
        r[5] = 0x78;
        if (sub == 0x01) {
            uint8_t f = (n > 8) ? c[8] : 0;
            d[4] = (f & 0x01) ? 0x07 : 0x00;
            d[5] = (f & 0x02) ? 0x07 : 0x00;
            d[6] = (f & 0x04) ? 0x03 : 0x00;
            d[7] = (f & 0x80) ? 0x03 : 0x00;
            d[8] = (f & 0x10) ? 0x03 : 0x00;
            d[9] = (f & 0x20) ? 0x03 : 0x00;
            dl = 12;
        } else if (sub == 0x06) {
            d[4] = (n > 12) ? c[12] : 0;
            dl = 40;
        } else {
            uint8_t requested = (n > 8) ? c[8] : 0;
            if (sub == 0x02) {
                s_feature_mask = requested;
            } else if (sub == 0x03) {
                s_feature_mask &= (uint8_t)~requested;
                s_enabled_features &= (uint8_t)~requested;
                if (requested & 0x10) switch_joycon2_reset_mouse_runtime();
            } else if (sub == 0x04) {
                s_enabled_features |= requested & s_feature_mask;
            } else if (sub == 0x05) {
                s_enabled_features &= (uint8_t)~(requested & s_feature_mask);
                if (requested & 0x10) switch_joycon2_reset_mouse_runtime();
            }
            dl = 4;
        }
        break;
    case 0x15:  // Bluetooth-pairing-shaped commands over USB -- real AES-128 key derivation,
                // byte-for-byte the same algorithm switch_pro2.c's/switch_gc.c's own
                // hardware-validated pairing uses (ns2_pairing_crypto.h).
        if (sub == 0x01) {
            memcpy(d, (const uint8_t[]){0x01, 0x08, 0x01,
                   switch_joycon2_ctrl_info[10], switch_joycon2_ctrl_info[11], switch_joycon2_ctrl_info[12],
                   switch_joycon2_ctrl_info[13], switch_joycon2_ctrl_info[14], switch_joycon2_ctrl_info[15]}, 9);
            dl = 9;
        } else if (sub == 0x02) {
            d[0] = 0x01;
            ns2_pairing_challenge(s_joycon2_ltk, &c[9], &d[1]);
            dl = 17;
        } else if (sub == 0x03) {
            d[0] = 0x01;
            dl = 1;
        } else if (sub == 0x04) {
            ns2_pairing_derive_ltk(&c[9], JOYCON2_DEVICE_KEY_B1, s_joycon2_ltk);
            d[0] = 0x01;
            memcpy(&d[1], JOYCON2_DEVICE_KEY_B1, 16);
            dl = 17;
        }
        break;
    case 0x16:  // unknown, 24 zero bytes (matches switch_gc.c's/switch_pro2.c's own equivalent)
        dl = 24;
        break;
    case 0x07:  // first-init command
        d[0] = 0x00;
        dl = 1;
        break;
    case 0x09:  // console-assigned player LED bitfield
        if (n > 8) report_set_player_leds(0, c[8]);
        dl = 0;
        break;
    case 0x08:  // Charging Grip commands -- Confirmed to exist and documented
                // (ndeadly's commands.md "Command 0x08"), but not implemented beyond a bare ACK
                // this pass: this project has no Charging Grip hardware of its own to source
                // GL/GR from, and the grip itself is a passive USB hub (Confirmed via live
                // capture, see docs/switch2-joycon2/protocol.md), not something this dongle emulates.
        dl = 0;
        break;
    case 0x10:  // firmware 2.1.4, side-specific type, BT 12.0.0, no DSP
        memcpy(d, switch_joycon2_firmware_info, sizeof(switch_joycon2_firmware_info));
        dl = sizeof(switch_joycon2_firmware_info);
        break;
    case 0x0B:  // battery -- structurally mirrors switch_gc.c/switch_pro2.c; values not
                // independently confirmed for Joy-Con 2.
        if (sub == 0x03) { memcpy(d, (const uint8_t[]){0xA5, 0x0E, 0x00, 0x00}, 4); dl = 4; }
        else if (sub == 0x04) { memcpy(d, (const uint8_t[]){0x34, 0x00, 0x83, 0x00}, 4); dl = 4; }
        break;
    case 0x01:  // NFC -- Confirmed neither side emulates real NFC hardware (Left genuinely has
                // none; Right does but this project doesn't emulate it). Reuses switch_gc.c's
                // own bare-ack shape (dir=0x04) for the same command family.
        r[1] = 0x04;
        dl = 0;
        break;
    // Structured family replies required by the real-console path. Joy-Con-specific meanings of
    // the opaque constants are not assigned here; see docs/switch2-joycon2/open-questions.md.
    case 0x11:
        if (sub == 0x01) { d[0] = 0x03; dl = 4; }  // USB form (0x03; the BLE form is 0x01)
        else if (sub == 0x03) {
            memcpy(d, (const uint8_t[]){
                0x01, 0xC0, 0x03, 0x00, 0x00, 0xE7, 0xD0, 0x1C, 0x3B, 0x79, 0x22, 0xA0, 0x3A,
                0x0A, 0xE8, 0x9C, 0x42, 0x58, 0xA0, 0x0B, 0x42, 0x0A, 0xE8, 0x9C, 0x41, 0x58,
                0xA0, 0x0B, 0x41}, 29);
            dl = 29;
        }
        break;
    case 0x18:  // structurally mirrors switch_gc.c/switch_pro2.c; values not independently
                // confirmed for Joy-Con 2.
        if (sub == 0x01) { memcpy(d, (const uint8_t[]){0,0,0x40,0xF0,0,0,0x60,0}, 8); dl = 8; }
        else if (sub == 0x03) { d[0] = (n > 8) ? c[8] : 0; dl = 1; }
        break;
    default:  // vibration, and anything else not specifically handled -> bare ACK. Never
              // silent, matching switch_gc.c's own `default:` case.
        dl = 0;
        break;
    }

    if (id != 0x03 && id != 0x02 && id != 0x0C && id != 0x15 && id != 0x16 &&
        id != 0x07 && id != 0x09 && id != 0x08 && id != 0x10 && id != 0x0B && id != 0x01 &&
        id != 0x11 && id != 0x18) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - last_unknown_log_ms > 1000) {
            printf("[JOYCON2] vendor bulk cmd id=0x%02x sub=0x%02x len=%lu answered with bare ACK\n",
                   id, sub, (unsigned long)n);
            last_unknown_log_ms = now_ms;
        }
    }

    tud_vendor_write(r, (uint16_t)(8 + dl));
    tud_vendor_write_flush();
}

//--------------------------------------------------------------------+
// Output report 0x01 (rumble/LED). Until a side-specific decode is captured, any nonzero packed
// LRA field becomes a fixed moderate motor request with a watchdog. See
// docs/switch2-joycon2/open-questions.md.
//--------------------------------------------------------------------+

#define JOYCON2_RUMBLE_ON_AMPLITUDE 0xB0
#define JOYCON2_RUMBLE_WATCHDOG_MS 500
static uint32_t s_rumble_last_nonzero_ms = 0;
static bool s_rumble_watchdog_armed = false;

void switch_joycon2_hid_out_report(uint8_t report_id, const uint8_t *data, uint16_t len) {
    if (report_id == 0 && len == 0) {
        report_set_rumble(0, 0, 0);
        s_rumble_watchdog_armed = false;
        return;
    }
    if (report_id != 0x01) return;

    bool any_nonzero = false;
    for (uint16_t i = 0; i < len && i < 16; i++) {
        if (data[i] != 0) { any_nonzero = true; break; }
    }
    if (!any_nonzero) {
        report_set_rumble(0, 0, 0);
        s_rumble_watchdog_armed = false;
        return;
    }
    report_set_rumble(0, JOYCON2_RUMBLE_ON_AMPLITUDE, JOYCON2_RUMBLE_ON_AMPLITUDE);
    s_rumble_last_nonzero_ms = to_ms_since_boot(get_absolute_time());
    s_rumble_watchdog_armed = true;
}

bool switch_joycon2_vendor_control_xfer(uint8_t rhport, uint8_t stage, const void *request_v) {
    tusb_control_request_t const *request = (tusb_control_request_t const *)request_v;
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bRequest == JOYCON2_MS_OS_VENDOR_CODE && request->wIndex == 0x0004) {
        return tud_control_xfer(rhport, request, (void *)switch_joycon2_ms_compat_id,
                                sizeof(switch_joycon2_ms_compat_id));
    }
    if (request->bRequest == JOYCON2_MS_OS_VENDOR_CODE && request->wIndex == 0x0005) {
        return tud_control_xfer(rhport, request, (void *)switch_joycon2_ms_ext_props,
                                sizeof(switch_joycon2_ms_ext_props));
    }

    switch (request->bRequest) {
        case 0x03: {  // identity block (64 B)
            const uint8_t *identity = switch_joycon2_ctrl_identity();
            uint16_t len = request->wLength < 64 ? request->wLength : 64;
            return tud_control_xfer(rhport, request, (void *)identity, len);
        }
        case 0x02: {  // firmware/version info (16 B)
            uint16_t len = request->wLength < sizeof(switch_joycon2_ctrl_info)
                               ? request->wLength : (uint16_t)sizeof(switch_joycon2_ctrl_info);
            return tud_control_xfer(rhport, request, (void *)switch_joycon2_ctrl_info, len);
        }
        case 0x04:  // OUT, no data -> ACK
            return tud_control_status(rhport, request);
    }

    return false;
}

//--------------------------------------------------------------------+
// Lifecycle
//--------------------------------------------------------------------+

void switch_joycon2_init(void) {
    switch_joycon2_build_identity();
    s_report_counter = 0;
    s_report05_counter = 0;
    s_selected_report_id = 0;
    s_feature_mask = JOYCON2_DEFAULT_FEATURE_MASK;
    s_enabled_features = 0;
    switch_joycon2_reset_mouse_runtime();
    s_bulk_cmd_count = 0;
    memset(s_joycon2_ltk, 0, sizeof(s_joycon2_ltk));
    report_set_rumble(0, 0, 0);
    s_rumble_watchdog_armed = false;
}

void switch_joycon2_reset(void) {
    ns2_joycon2_build_ep0_info(switch_joycon2_unit_id, switch_joycon2_ctrl_info);
    ns2_joycon2_build_command_info(s_side == JOYCON2_SIDE_RIGHT,
                                   switch_joycon2_firmware_info);
    switch_joycon2_init();
}

void switch_joycon2_mount(void) {
    s_report_counter = 0;
    s_report05_counter = 0;
    s_selected_report_id = 0;
    s_feature_mask = JOYCON2_DEFAULT_FEATURE_MASK;
    s_enabled_features = 0;
    switch_joycon2_reset_mouse_runtime();
    s_bulk_cmd_count = 0;
    memset(s_joycon2_ltk, 0, sizeof(s_joycon2_ltk));
    report_set_rumble(0, 0, 0);
    s_rumble_watchdog_armed = false;
}

void switch_joycon2_task(void) {
    if (tud_vendor_available()) {
        uint8_t cmd[64];
        uint32_t n = tud_vendor_read(cmd, sizeof(cmd));
        switch_joycon2_vendor_dispatch(cmd, n);
    }
    if (s_rumble_watchdog_armed &&
        (uint32_t)(to_ms_since_boot(get_absolute_time()) - s_rumble_last_nonzero_ms) > JOYCON2_RUMBLE_WATCHDOG_MS) {
        report_set_rumble(0, 0, 0);
        s_rumble_watchdog_armed = false;
    }

    uint8_t extended_id = (s_side == JOYCON2_SIDE_LEFT) ? 0x07 : 0x08;
    if (tud_hid_n_ready(0)) {
        uint8_t report[63];
        if (s_selected_report_id == 0x05) {
            switch_joycon2_build_report05(report);
            tud_hid_n_report(0, 0x05, report, sizeof(report));
        } else if (s_selected_report_id == extended_id) {
            switch_joycon2_build_report(report);
            tud_hid_n_report(0, extended_id, report, sizeof(report));
        }
        // No report selected yet: stay silent, matches switch_gc.c's own identical behavior.
    }
}

#endif  // NS2_PRO
