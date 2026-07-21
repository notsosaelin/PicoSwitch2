/*
 * NSO GameCube Controller (VID 057E / PID 2073) USB emulation. See
 * include/switch_gc.h for the module contract and exact Stage boundary of
 * each entry point. Only compiled into the link when -DNS2_PRO is set,
 * mirroring switch_pro2.c's own scoping -- GameCube is a runtime-selectable
 * personality within the NS2_PRO build, not a separate build axis.
 */
#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "tusb.h"

#include "ns2_gc_identity.h"
#include "ns2_protocol_trace.h"
#include "report.h"       // report_set_rumble, get_global_gamepad_input
#include "switch_gc_rumble_decode.h"
#include "switch_gc.h"
#include "switch_gc_encode.h"  // switch_gc_encode_report (pure, host-testable encoder)
#include "ns2_pairing_crypto.h"  // shared AES-128/LTK-derivation pairing crypto
#include "switch_gc_report_select.h"  // switch_gc_select_report (pure, host-testable)
#include "switch_pro.h"    // switch_pro_input_t, SWITCH_MASK_*/SWITCH_EXTRA_*
#include "usb.h"  // g_usb_personality (dispatch owner; not read directly here)

#ifdef NS2_PRO

// NS2_DIAG: GameCube USB handshake progress, blinked on the LED by ns2_bt_host.c (core1) -- see
// switch_gc.h's own comment for the stage numbers and why this is a separate variable from
// switch_pro2.c's g_ns2_stage. Always defined (cheap, a single byte) even when NS2_DIAG is off;
// only the blink code that reads it is gated by NS2_DIAG.
volatile uint8_t g_gc_stage = 0;

// NS2_DIAG: the raw report-ID byte the console requested via 0x03/sub 0x0A, valid only when
// g_gc_stage == 21 (reached the select-report step, but the requested ID wasn't 0x05/0x0A).
// Blinked as a two-digit tens/ones burst by ns2_bt_host.c rather than a flat flash count, so an
// arbitrary byte value (not just a small stage number) can still be read off the LED directly --
// see that file's own comment for the exact encoding.
volatile uint8_t g_gc_bad_report_id = 0;

//--------------------------------------------------------------------+
// Descriptors -- Confirmed byte-exact, cross-validated against TWO
// independent genuine controllers (this project's own unit via elevated
// USBPcap --inject-descriptors capture, and a second unit via ndeadly's
// rumble-procon-gccon.pcapng.gz capture -- both agree byte-for-byte). See
// docs/switch2-gc/protocol.md "USB identity" / "USB descriptor topology" and
// docs/experiments/nso-gc-captures/genuine-controller-descriptors-2026-07-13.pcap.
//--------------------------------------------------------------------+

static const uint8_t switch_gc_device_desc[] = {
    0x12,        // bLength
    0x01,        // bDescriptorType (Device)
    0x00, 0x02,  // bcdUSB 2.00
    0xEF,        // bDeviceClass (Misc, IAD composite)
    0x02,        // bDeviceSubClass
    0x01,        // bDeviceProtocol
    0x40,        // bMaxPacketSize0 64
    0x7E, 0x05,  // idVendor 0x057E (Nintendo)
    0x73, 0x20,  // idProduct 0x2073 (NSO GameCube Controller)
    0x11, 0x01,  // bcdDevice 1.11 -- DELIBERATELY NOT the real captured 01,01 (BCD 1.01).
                 // Superseded 2026-07-13 by direct hardware evidence: the project owner's
                 // first hardware test, run with a genuine GameCube controller simultaneously
                 // connected for comparison, found Windows Device Manager showing Code 28,
                 // intermittent "If_Hid" misrecognition, and Steam losing its cached mapping
                 // for the GENUINE controller too -- classic symptoms of a WinUSB driver-cache
                 // collision. Windows keys its WinUSB binding cache on VID+PID+bcdDevice (not a
                 // true unique serial -- this device's serial string is literally "00", same as
                 // the genuine unit's). Using the exact real bcdDevice made the Pico and a real
                 // controller indistinguishable to that cache. This is the IDENTICAL problem
                 // switch_pro2.c's own ns2_device_desc already solved the same way: its bcdDevice
                 // is deliberately 2.10, not the real 2.00, for exactly this reason (see that
                 // file's own comment) -- confirmed empirically there that the Switch 2 console
                 // does not gate on bcdDevice, only Windows' PC-side driver cache cares. That
                 // finding is carried over by analogy here (Strong, not independently re-verified
                 // for GameCube specifically against a real console -- flag for Stage G). Original
                 // captured bytes (01,01 = BCD 1.01, not "2.01" -- ndeadly's own descriptors.md
                 // comment has the arithmetic wrong) preserved here in history/evidence docs, not
                 // reused verbatim in firmware, per this exact precedent.
    0x01,        // iManufacturer
    0x02,        // iProduct
    0x03,        // iSerialNumber
    0x01,        // bNumConfigurations
};

static const char *switch_gc_strings[] = {
    (const char[]){0x09, 0x04},          // 0: language id (en-US)
    "Nintendo",                          // 1: manufacturer -- Confirmed
    "Nintendo GameCube Controller",      // 2: product -- Confirmed
    "00",                                // 3: serial -- Confirmed (literal, not a real per-unit serial)
    NULL,                                // 4: iConfiguration -- not captured; stalls this index (see tud_descriptor_string_cb)
    "If_Hid",                            // 5: iInterface (IF0, HID) -- Confirmed live via pywinusb this session
    NULL,                                // 6: iInterface (IF1, vendor) -- not captured; stalls this index
};

// HID report descriptor (97 bytes). Confirmed 2026-07-13: a live USBPcap
// replug capture on this project's own genuine unit caught a standalone
// GET_DESCRIPTOR(HID Report) transaction and recovered these 97 bytes
// byte-exact (previously only Strong -- transcribed from ndeadly's
// descriptors.md, never independently captured). See docs/switch2-gc/
// protocol.md "HID report descriptor" and docs/experiments/nso-gc-captures/
// genuine-controller-hid-report-descriptor-2026-07-13.pcap.
static const uint8_t switch_gc_report_desc[] = {
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
    0x85, 0x0A,        //   Report ID (10)
    0x09, 0x01,        //   Usage (0x01)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x15,        //   Usage Maximum (0x15) -> 21 buttons
    0x25, 0x01,        //   Logical Maximum (1)
    0x95, 0x15,        //   Report Count (21)
    0x75, 0x01,        //   Report Size (1)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x81, 0x03,        //   Input (Const) -> 3-bit pad
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x33,        //     Usage (Rx)
    0x09, 0x35,        //     Usage (Rz)
    0x26, 0xFF, 0x0F,  //     Logical Maximum (4095)
    0x95, 0x04,        //     Report Count (4)
    0x75, 0x0C,        //     Report Size (12)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0xC0,              //   End Collection
    0x05, 0xFF,        //   Usage Page (Reserved 0xFF)
    0x09, 0x02,        //   Usage (0x02)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x95, 0x34,        //   Report Count (52)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x85, 0x03,        //   Report ID (3)
    0x09, 0x01,        //   Usage (0x01)
    0x95, 0x3F,        //   Report Count (63)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0xC0,              // End Collection
};
_Static_assert(sizeof(switch_gc_report_desc) == 97,
               "GameCube HID report descriptor must be 97 bytes (Confirmed wDescriptorLength)");

// Configuration descriptor: exact byte-for-byte capture, 2 interfaces (IF0
// HID, IF1 vendor bulk), matching Pro Controller 2's own Option-B interface
// SHAPE (2 IAD-grouped interfaces, same endpoint address numbering) but with
// GameCube's own distinct field values (iConfiguration=4, bInterval=4 on the
// HID endpoints vs Pro2's bInterval=1, etc.) -- see the byte-level citation in
// docs/switch2-gc/protocol.md "USB descriptor topology" for the full decode.
#define SWITCH_GC_CONFIG_LEN 80
static const uint8_t switch_gc_config_desc[] = {
    0x09, 0x02, (SWITCH_GC_CONFIG_LEN & 0xFF), (SWITCH_GC_CONFIG_LEN >> 8),
    0x02,        // bNumInterfaces
    0x01,        // bConfigurationValue
    0x04,        // iConfiguration (string index 4 -- text not captured, see switch_gc_strings)
    0xC0,        // bmAttributes: self-powered, NO remote wakeup (Confirmed; same value as this
                 // project's own Pro2 descriptor -- the notable comparison is against
                 // SoulCalDan's unrelated WiiU-adapter descriptor, which uses 0xE0/remote-wakeup)
    0xFA,        // bMaxPower 500mA
    // IAD + Interface 0: HID
    0x08, 0x0B, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x05,  // iInterface=5 ("If_Hid", Confirmed)
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x61, 0x00,  // HID desc, bcdHID 1.11, report len 97
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x04,  // EP 0x81 interrupt IN,  64B, bInterval 4
    0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x04,  // EP 0x01 interrupt OUT, 64B, bInterval 4
    // IAD + Interface 1: vendor bulk
    0x08, 0x0B, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x01, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x06,  // iInterface=6 (text not captured)
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,  // EP 0x02 bulk OUT, 64B
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,  // EP 0x82 bulk IN,  64B
};
_Static_assert(sizeof(switch_gc_config_desc) == SWITCH_GC_CONFIG_LEN,
               "GameCube configuration descriptor size must match wTotalLength");

const uint8_t *switch_gc_device_descriptor(void) { return switch_gc_device_desc; }
const uint8_t *switch_gc_config_descriptor(void) { return switch_gc_config_desc; }
const uint8_t *switch_gc_hid_report_descriptor(void) { return switch_gc_report_desc; }
const char **switch_gc_string_table(size_t *count) {
    *count = sizeof(switch_gc_strings) / sizeof(switch_gc_strings[0]);
    return switch_gc_strings;
}

//--------------------------------------------------------------------+
// Microsoft OS 1.0 descriptors -- auto-bind WinUSB to the vendor interface
// (IF1), mirroring switch_pro2.c's own identical mechanism for its vendor
// interface. Added 2026-07-13 after hardware testing found GameCube mode
// showed Windows Device Manager error code 28 ("no compatible drivers") on
// the whole composite device, while a genuine controller enumerates cleanly
// (Universal Serial Bus Devices, no driver error) -- strong evidence the real
// NSO GameCube Controller ships this exact mechanism too, matching how the
// real Switch 2 Pro Controller 2 does (see switch_pro2.c's own comment: its
// retail unit's vendor interface exposes `USB\MS_COMP_WINUSB`). This is a
// standard, safe, well-understood Windows-only enumeration mechanism -- not
// the Nintendo-specific EP0 identity handshake (bRequest 0x02/0x03), which
// stays Stage D. NSO-GC.md's own Stage B scope explicitly allows implementing
// an EP0 vendor command in this stage "if Windows enumeration requires it" --
// this hardware result is exactly that evidence.
//
// The vendor code (0x20) is a value THIS firmware chooses and advertises to
// Windows via the 0xEE string below; Windows then echoes it back as bRequest
// on the follow-up Extended Compat ID fetch. It does not need to match
// whatever byte the genuine controller's own firmware happens to use
// internally -- only to be internally self-consistent, which it is. Reusing
// Pro2's same numeric value is a naming convenience, not a correctness
// requirement (the two personalities are never simultaneously active).
//--------------------------------------------------------------------+

#define GC_MS_OS_VENDOR_CODE 0x20

// OS string descriptor (index 0xEE): "MSFT100" + the vendor request code.
static const uint16_t switch_gc_ms_os_str[] = {
    0x0312, 'M', 'S', 'F', 'T', '1', '0', '0', GC_MS_OS_VENDOR_CODE};

const uint16_t *switch_gc_ms_os_string_descriptor(void) {
    return switch_gc_ms_os_str;
}

// Extended Compat ID OS feature descriptor: WINUSB for interface 1 (vendor).
static const uint8_t switch_gc_ms_compat_id[] = {
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
_Static_assert(sizeof(switch_gc_ms_compat_id) == 40, "MS compat ID descriptor must be 40 bytes");

//--------------------------------------------------------------------+
// Stage C -- Input report 0x0A construction. Layout Confirmed (field-by-field
// live decode, docs/switch2-gc/protocol.md "Input Report 0x0A"); the button
// bitfield itself is Strong only (no live button-press sample yet). Mirrors
// switch_pro2.c's own ns2_build_report() shape/conventions exactly -- same
// shared switch_pro_input_t source, same packed-12-bit stick passthrough,
// same tud_hid_n_report(instance, report_id, buf, 63) convention (TinyUSB
// prepends the report ID byte; 63 is the data length only).
//
// The actual field encoding lives in switch_gc_encode.c (pure, host-testable,
// zero pico-sdk dependency, mirroring usb_mode_cycle.c's own pattern) -- see
// tools/test_switch_gc_report.c for its golden tests. Everything below is
// just the thin glue to real cross-core/runtime state.
//--------------------------------------------------------------------+

// Personality-scoped report counter -- resets to 0 whenever this personality becomes (or
// re-becomes) active, per its own lifecycle hooks below, rather than living forever as an
// unreset function-static (a fresh session on real hardware should start its own counter from
// 0, matching switch_pro2.c's ns2_build_report()'s equivalent, which is also reset implicitly
// on every fresh boot since Pro2 is the sole personality there).
static uint8_t s_report_counter = 0;

void switch_gc_build_report(uint8_t *p) {
    switch_pro_input_t in;
    get_global_gamepad_input(0, &in);
    switch_gc_encode_report(&in, s_report_counter++, p);
}

//--------------------------------------------------------------------+
// Runtime lifecycle -- Stage B: safe neutral behavior only. No report
// construction, no command dispatch, no factory-data emulation. Those are
// Stage C/D/E per NSO-GC.md's explicit scope boundary for this pass.
//--------------------------------------------------------------------+

// Stage D, MINIMUM evidence-backed gate only (2026-07-13). Session-scoped: reset by
// switch_gc_init()/switch_gc_reset()/switch_gc_mount() (personality entry, mount,
// disconnect/re-enumeration, switch-away) so a fresh session never inherits a prior one's
// armed state. GC_REPORT_ID_NONE = unarmed/no report selected; 0x05 or 0x0A once a real live
// hardware capture (2026-07-13) proved the host can validly request either -- see
// switch_gc_encode_report05()'s own comment for why 0x05 turned out to be required, not just
// 0x0A. The arm/ignore/switch decision itself lives in the pure, host-tested
// switch_gc_select_report() (switch_gc_report_select.h/.c).
static uint8_t s_selected_report_id = GC_REPORT_ID_NONE;
static uint32_t s_report05_counter = 0;  // independent 32-bit counter, report 0x05's own width

// Native GC output is binary ERM state, not Pro2 HD-rumble amplitude. The decoder accepts the two
// captured state-byte forms. Stretch ON into a bounded pulse because Bluetooth output cannot
// reliably reproduce millisecond-scale edges; OFF/ZLP do not refresh it and STOP ends it now.
// Evidence and unresolved semantics: docs/switch2-gc/open-questions.md.
#define GC_RUMBLE_ON_AMPLITUDE 0xB0
#define GC_RUMBLE_PULSE_MS 40
static uint32_t s_rumble_pulse_until_ms = 0;
static bool s_rumble_pulse_armed = false;

// Identity + info blocks the console fetches over EP0 vendor control BEFORE it will touch the
// bulk command channel -- the SAME gate `switch_pro2.c`'s `ns2_vendor_control_xfer()` already
// solved for Pro Controller 2 (see that function's own comment): "stalling these was why the
// console configured us then went silent... the Switch requests are console-specific; Windows/
// Steam never send them (no PC impact)." Found 2026-07-13 to be the exact reason GC mode wasn't
// recognized on a real Switch 2 despite enumerating and streaming fine on Windows/Steam --
// `switch_gc_vendor_control_xfer()` (below) previously only handled the WinUSB MS-OS request and
// stalled everything else, including these three. Also reused by switch_gc_mem_read() below
// (0x02/sub 0x04 memory reads at SPI address 0x13000 structurally are this same identity data).
//
// Byte layout Confirmed from this project's own captured GC bRequest=3/2 responses (see
// docs/switch2-gc/protocol.md "EP0 vendor control requests", cross-validated against two
// independent genuine units): `01 00`, 16-byte serial field (14 ASCII + 2 nul), VID `7E 05`,
// PID `73 20`, `01 04 01` (a fixed type/mode constant -- GC's real captured value, NOT Pro2's
// `01 06 01`), then 0xFF fill to 64 bytes -- GC's real capture has no color-byte fields there,
// unlike Pro2's identity block. Per NSO-GC.md's explicit exclusion rule, the serial is a
// FICTITIOUS value, not either real captured unit's actual serial (`HHW50001061937` /
// `HHW50001551810`) -- matches the confirmed `HHW5000` prefix pattern for plausibility, deliberately
// wrong suffix so it can never collide with or be mistaken for real per-unit hardware.
static const uint8_t switch_gc_ctrl_identity[64] = {
    0x01, 0x00,                                                          // fixed header
    'H', 'H', 'W', '5', '0', '0', '0', '9', '9', '9', '9', '9', '9', '9', 0x00, 0x00,  // serial (fictitious)
    0x7E, 0x05,                                                          // VID 0x057E
    0x73, 0x20,                                                          // PID 0x2073
    0x01, 0x04, 0x01,                                                    // type/mode (Confirmed, GC's own real value)
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,          // 0xFF fill to 64 bytes
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
_Static_assert(sizeof(switch_gc_ctrl_identity) == 64, "GC EP0 identity block must be 64 bytes");

// Synthetic per-unit ID retained from the validated EP0 reply. Version bytes are
// assembled beside command 0x10 from the shared current GC identity builder.
static const uint8_t SWITCH_GC_UNIT_ID[6] = {0x02, 0xBB, 0x5E, 0xAB, 0xA9, 0x3C};
static uint8_t switch_gc_ctrl_info[NS2_GC_EP0_INFO_LEN];

// USB pairing crypto. The public device key is shared with the working Pro2 path; a GC-specific
// capture has not distinguished it. The derived LTK is session-scoped and reset on init/mount.
static const uint8_t GC_DEVICE_KEY_B1[16] = {
    0x5C, 0xF6, 0xEE, 0x79, 0x2C, 0xDF, 0x05, 0xE1,
    0xBA, 0x2B, 0x63, 0x25, 0xC4, 0x1A, 0x5F, 0x10};
static uint8_t s_gc_ltk[16];

// Synthetic calibration record with the same working shape as Pro2; never copy private per-unit
// calibration from a dump. See docs/experiments/nso-gc-spi-dump-analysis-2026-07-13.md.
static const uint8_t switch_gc_synthetic_cal_blk[40] = {
    0x01, 0xAD, 0xD9, 0x9A, 0x55, 0x56, 0x65, 0xA0, 0x00, 0x0A, 0xA0, 0x00, 0x0A, 0xE2,
    0x20, 0x0E, 0xE2, 0x20, 0x0E, 0x9A, 0xAD, 0xD9, 0x9A, 0xAD, 0xD9, 0x0A, 0xA5, 0x50,
    0x0A, 0xA5, 0x50, 0x2F, 0xF6, 0x62, 0x2F, 0xF6, 0x62, 0x0A, 0xFF, 0xFF};

// GC's own small SPI/factory read emulation, mirroring switch_pro2.c's ns2_mem_read() approach.
// Only address windows with actual evidence, or a deliberately-flagged synthetic stand-in, are
// populated; everything else reads back 0xFF, matching the "uninitialised flash reads as 0xFF"
// convention already established for Pro2 AND independently confirmed in GC's own SPI dump
// analysis (docs/experiments/nso-gc-spi-dump-analysis-2026-07-13.md: 0x13060 and 0x1FC040 both
// read 0xFF on a real unit despite being in the documented read list -- left as 0xFF here too,
// deliberately NOT synthesized, since that's the evidence-backed real value for those two).
static void switch_gc_mem_read(uint32_t addr, uint8_t len, uint8_t *out) {
    for (uint8_t i = 0; i < len; i++) {
        uint32_t a = addr + i;
        if (a >= 0x13000 && a < 0x13000 + sizeof(switch_gc_ctrl_identity)) {
            // Same identity bytes already served over EP0 bRequest=3 -- internally consistent,
            // and this address is exactly where that data structurally lives (Confirmed:
            // bRequest=3 reads from SPI 0x13000, see protocol.md "EP0 vendor control requests").
            out[i] = switch_gc_ctrl_identity[a - 0x13000];
        } else if (a >= 0x13080 && a < 0x13080 + 40) {
            out[i] = switch_gc_synthetic_cal_blk[a - 0x13080];  // synthetic, see comment above
        } else if (a >= 0x130C0 && a < 0x130C0 + 40) {
            out[i] = switch_gc_synthetic_cal_blk[a - 0x130C0];  // same synthetic block, second copy
        } else if (a >= 0x13100 && a < 0x13100 + 24) {
            out[i] = 0x00;  // motion calibration bias -- safe "no bias" default (all-zero is a
                             // valid float 0.0f pattern, unlike 0xFF which reads as garbage/NaN)
        } else if (a >= 0x13140 && a < 0x13140 + 9) {
            out[i] = 0x00;  // same reasoning, smaller unidentified block immediately after
        } else if (a == 0x1FA000) {
            out[i] = 0x00;  // Bluetooth pairing entry count = 0 (no bonds over USB) -- same
                             // convention as switch_pro2.c's ns2_mem_read() for the same address.
        } else {
            out[i] = 0xFF;  // uninitialised/erased flash, matches GC's own SPI dump evidence
        }
    }
}

// Vendor bulk command dispatch. Every request receives at least the standard 8-byte response
// envelope; required families add their captured or family-shaped payload. No private per-unit
// serial, bond, or calibration bytes are copied from reference hardware. Protocol history and
// remaining opaque fields live in docs/switch2-gc/{protocol,open-questions}.md.
// NS2_DIAG: total bulk vendor commands received this session -- see switch_gc_vendor_dispatch()'s
// own comment for why stage 5 alone (fires on ANY command) wasn't enough signal: it can't tell
// "only one command ever arrived, then the console gave up" from "many commands arrived but the
// final select-report step specifically never happened". Reset alongside the other session state
// in switch_gc_init()/mount().
static uint32_t s_bulk_cmd_count = 0;

static void switch_gc_vendor_dispatch(const uint8_t *c, uint32_t n) {
    if (n < 8) return;
    uint8_t id = c[0], transport = c[2], sub = c[3];
    ns2_protocol_trace_record(time_us_32(), (uint8_t)g_usb_personality,
                              NS2_TRACE_BULK_COMMAND,
                              NS2_TRACE_CONSOLE_TO_DEVICE, id, sub, c, n);
    // NS2_DIAG: stage = 5 + (count of distinct bulk commands received so far, capped at 9), so
    // the LED flash count directly answers "how many commands did the console send before it
    // stopped" -- much more diagnostic than a flat "at least one arrived" signal. Capped to keep
    // the blink cycle (each flash adds ~1.5s) countable in practice. Only updates while a report
    // hasn't been selected yet (see the sub==0x0A handler below for the distinct full-success
    // sentinel) so a late-arriving housekeeping command after real success doesn't regress it.
    s_bulk_cmd_count++;
    if (s_selected_report_id == GC_REPORT_ID_NONE) {
        uint8_t capped = (uint8_t)(s_bulk_cmd_count > 9 ? 9 : s_bulk_cmd_count);
        uint8_t candidate = (uint8_t)(5 + capped);
        if (candidate > g_gc_stage) g_gc_stage = candidate;
        // NS2_DIAG: stage 20 -- the console has sent SOME 0x03-family command (0x0D Init USB,
        // 0x03 Enable USB HID Reports, or 0x0A Select Input Report -- this stage does NOT
        // distinguish which). Stage 21 is the sharper follow-up question, checked separately
        // below at the actual sub==0x0A dispatch: has it specifically tried Select Input Report,
        // as opposed to only ever sending 0x0D/0x03? A console stuck at 20 forever (never
        // reaching 21) means it never attempts the select step at all -- a different, earlier
        // problem than reaching 21 but requesting a report ID this dispatcher doesn't recognize.
        if (id == 0x03 && g_gc_stage < 20) g_gc_stage = 20;
    }

    // Bounded diagnostic (rate-limited to 1/sec) for whatever this dispatch still doesn't
    // recognize below, so the first real hardware run against an actual console produces useful
    // evidence without flooding the log.
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
    uint16_t dl = 0;  // default: bare 8-byte ACK, matching switch_pro2.c's own `default:` case --
                      // the key fix this pass: never silent, always at least an envelope.

    switch (id) {
    case 0x03:
        if (sub == 0x0D || sub == 0x03) {
            // Initialise USB / Enable USB HID Reports: Confirmed 4-byte tail `01 00 00 00`
            // observed directly for 0x0D in the real capture; 0x03 mirrors it (not
            // independently observed for GC, but documented and structurally identical in
            // ndeadly's commands.md).
            d[0] = 0x01;
            dl = 4;
        } else if (sub == 0x0A) {
            // Select Input Report: Confirmed real response is exactly 8 bytes, no tail (dl
            // stays 0), for both supported report IDs (0x05 Confirmed via live Steam capture,
            // 0x0A Confirmed via this project's earlier documented capture). Any other requested
            // value is ACKed (matches ndeadly's documented "invalid report IDs are ignored"
            // semantics -- the request still gets a response) but does not arm streaming.
            // NS2_DIAG: stage 21 -- the console has specifically attempted Select Input Report
            // (as opposed to only ever sending other 0x03-family commands, stage 20). Set BEFORE
            // checking whether the requested ID was recognized, so reaching 21 without ever
            // reaching 255 means "it tried, but requested an ID this dispatcher doesn't accept."
            if (g_gc_stage < 21) g_gc_stage = 21;
            s_selected_report_id = switch_gc_select_report(s_selected_report_id, c, n);
            if (s_selected_report_id == GC_REPORT_ID_NONE && n > 8) {
                g_gc_bad_report_id = c[8];  // NS2_DIAG: exact unrecognized report-ID byte requested
            }
            if (s_selected_report_id != GC_REPORT_ID_NONE) {
                // NS2_DIAG: full success sentinel (255, not a count) -- see ns2_bt_host.c's
                // blink logic, which special-cases this as a solid-on LED rather than trying to
                // count 255 flashes. Deliberately NOT part of the 5+count scale above so it can
                // never be confused with "9 or more commands received but still not armed".
                g_gc_stage = 255;
            }
        }
        break;
    case 0x02: {  // SPI/flash memory -- see switch_gc_mem_read()'s own comment for what's real
        uint32_t addr = (uint32_t)c[12] | ((uint32_t)c[13] << 8) |
                        ((uint32_t)c[14] << 16) | ((uint32_t)c[15] << 24);
        if (sub == 0x04) {  // memory read, variable length
            uint8_t len = c[8];
            if (len > 0x28) len = 0x28;  // r[] is 64 bytes total; 8 (header) + 4 (echo) + 0x28 fits
            d[0] = len;
            d[4] = c[12]; d[5] = c[13]; d[6] = c[14]; d[7] = c[15];
            switch_gc_mem_read(addr, len, &d[8]);
            dl = (uint16_t)(8 + len);
        } else if (sub == 0x01) {  // read a fixed 0x28-byte block (Pro2's own equivalent reads
                                    // 0x40, but this dispatcher's r[64] buffer can't hold an
                                    // 8+8+0x40 reply -- 0x28 is the largest that fits safely)
            d[0] = 0x28;
            d[4] = c[12]; d[5] = c[13]; d[6] = c[14]; d[7] = c[15];
            switch_gc_mem_read(addr, 0x28, &d[8]);
            dl = 8 + 0x28;
        } else if (sub == 0x05) {  // memory write -> ack (never persisted)
            d[4] = c[12]; d[5] = c[13]; d[6] = c[14]; d[7] = c[15];
            dl = 8;
        }
        break;
    }
    case 0x0C:  // feature select; GC's captured feature mask is 0x27
        if (sub == 0x01) {  // echo per-bit family capability levels for the requested mask
            uint8_t f = (n > 8) ? c[8] : 0;
            d[4] = (f & 0x01) ? 0x07 : 0x00;
            d[5] = (f & 0x02) ? 0x07 : 0x00;
            d[6] = (f & 0x04) ? 0x01 : 0x00;
            d[7] = (f & 0x80) ? 0x01 : 0x00;
            d[8] = (f & 0x10) ? 0x01 : 0x00;
            d[9] = (f & 0x20) ? 0x03 : 0x00;
            dl = 12;
        } else if (sub == 0x06) {  // configure features -> echo the requested feature id
            d[4] = (n > 12) ? c[12] : 0;
            dl = 40;
        } else {
            dl = 4;  // set/clear/enable/disable mask -> bare structural ack
        }
        break;
    case 0x15:  // Bluetooth-pairing-shaped commands, run over USB. CORRECTED 2026-07-13: this
                // project's own reference research (ndeadly) explicitly documents "the Switch 2
                // console requires successful pairing... [which] can be performed via both
                // Bluetooth and USB connections" -- a wired connection is NOT exempt, contrary
                // to this comment's own previous assumption (which shipped placeholder bytes
                // here and is the likely reason a real console's handshake stalled after this
                // exact command family in hardware testing, per the LED handshake-progress
                // diagnostic reaching "0x03/0x0A attempted" only after 9+ commands including
                // this one). Now performs REAL AES-128 key derivation via the shared,
                // host-tested ns2_pairing_crypto.h -- byte-for-byte the same algorithm
                // switch_pro2.c's own already-hardware-validated pairing uses.
        if (sub == 0x01) {  // exchange addresses -> reuse GC's own EP0 info-block tail bytes for
                             // self-consistency, mirroring switch_pro2.c's identical pattern
                             // (its own address-exchange reply reuses its own info-block tail).
            memcpy(d, (const uint8_t[]){0x01, 0x04, 0x01,
                   switch_gc_ctrl_info[10], switch_gc_ctrl_info[11], switch_gc_ctrl_info[12],
                   switch_gc_ctrl_info[13], switch_gc_ctrl_info[14], switch_gc_ctrl_info[15]}, 9);
            dl = 9;
        } else if (sub == 0x02) {  // confirm LTK: B2(wire) = AES128(LTK, reverse(A2))
            d[0] = 0x01;
            ns2_pairing_challenge(s_gc_ltk, &c[9], &d[1]);  // A2 = c[9..24]
            dl = 17;
        } else if (sub == 0x03) {  // finalise
            d[0] = 0x01;
            dl = 1;
        } else if (sub == 0x04) {  // derive LTK from A1 and return the shared working public B1
            ns2_pairing_derive_ltk(&c[9], GC_DEVICE_KEY_B1, s_gc_ltk);  // A1 = c[9..24]
            d[0] = 0x01;
            memcpy(&d[1], GC_DEVICE_KEY_B1, 16);
            dl = 17;
        }
        break;
    case 0x16:  // unknown, 24 zero bytes (matches switch_pro2.c's own equivalent)
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
    // Everything below added 2026-07-13 after real hardware testing showed pairing now succeeds
    // (console shows a "Paired" GameCube icon) but the handshake still stalls before Select Input
    // Report, with Pro2 mode working perfectly on the same console/hardware. Systematic diff
    // against switch_pro2.c's own complete, hardware-validated ns2_dispatch(): these command
    // families were previously falling through to GC's generic bare-ACK default (0 bytes) instead
    // of the specific structured replies Pro2 actually sends. A console asking for a 12-byte
    // firmware-info block or a 29-byte block (0x11/sub 0x03) and getting 0 bytes back is a much
    // larger deviation than a wrong calibration value, and is now the leading suspect.
    case 0x10:  // genuine NSO GC firmware identity (live-query confirmed 2026-07-21)
        ns2_gc_build_command_info(d);
        dl = NS2_GC_COMMAND_INFO_LEN;
        break;
    case 0x0B:  // battery -- structurally mirrors switch_pro2.c; values not independently
                // confirmed for GC, kept only for envelope-shape/length consistency.
        if (sub == 0x03) { memcpy(d, (const uint8_t[]){0xA5, 0x0E, 0x00, 0x00}, 4); dl = 4; }
        else if (sub == 0x04) { memcpy(d, (const uint8_t[]){0x34, 0x00, 0x83, 0x00}, 4); dl = 4; }
        break;
    case 0x11:  // opaque family response retained for console interoperability
        if (sub == 0x01) { d[0] = 0x03; dl = 4; }  // USB form (0x03; the BLE form is 0x01)
        else if (sub == 0x03) {
            memcpy(d, (const uint8_t[]){
                0x01, 0xC0, 0x03, 0x00, 0x00, 0xE7, 0xD0, 0x1C, 0x3B, 0x79, 0x22, 0xA0, 0x3A,
                0x0A, 0xE8, 0x9C, 0x42, 0x58, 0xA0, 0x0B, 0x42, 0x0A, 0xE8, 0x9C, 0x41, 0x58,
                0xA0, 0x0B, 0x41}, 29);
            dl = 29;
        }
        break;
    case 0x01:  // NFC -- GC has no NFC hardware, but Pro2's own capture-confirmed bare-ack shape
                // (dir=0x04 instead of the default 0x01) is reused here since ndeadly's own docs
                // frame this as common command-family behavior, not Pro2-specific.
        r[1] = 0x04;
        dl = 0;
        break;
    case 0x18:  // structurally mirrors switch_pro2.c; values not independently confirmed for GC.
        if (sub == 0x01) { memcpy(d, (const uint8_t[]){0,0,0x40,0xF0,0,0,0x60,0}, 8); dl = 8; }
        else if (sub == 0x03) { d[0] = (n > 8) ? c[8] : 0; dl = 1; }
        break;
    default:  // 0x0A vibration, 0x08 and anything else -> bare ACK. This is the key fix from the
              // previous pass: NEVER silent, even for commands this dispatcher doesn't
              // semantically understand -- matches switch_pro2.c's own `default:` case exactly.
        dl = 0;
        break;
    }

    // Bounded diagnostic (rate-limited to 1/sec): log any command ID this dispatcher doesn't
    // have a specific case for, even though it still gets a bare ACK via `default:` above -- so
    // the first real hardware run against an actual console produces useful evidence about what
    // else it sends, without flooding the log every session.
    if (id != 0x03 && id != 0x02 && id != 0x0C && id != 0x15 && id != 0x16 &&
        id != 0x07 && id != 0x09 && id != 0x10 && id != 0x0B && id != 0x11 && id != 0x01 &&
        id != 0x18) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - last_unknown_log_ms > 1000) {
            printf("[GC] vendor bulk cmd id=0x%02x sub=0x%02x len=%lu answered with bare ACK\n",
                   id, sub, (unsigned long)n);
            last_unknown_log_ms = now_ms;
        }
    }

    uint16_t response_length = (uint16_t)(8 + dl);
    ns2_protocol_trace_record(time_us_32(), (uint8_t)g_usb_personality,
                              NS2_TRACE_BULK_RESPONSE,
                              NS2_TRACE_DEVICE_TO_CONSOLE, id, sub, r,
                              response_length);
    tud_vendor_write(r, response_length);
    tud_vendor_write_flush();
}

void switch_gc_init(void) {
    // Reset both report counters so a fresh session starts from 0 rather than carrying over
    // whatever count a previous activation of this personality reached, and re-arm the Stage D
    // gate closed -- a fresh session must not inherit a prior one's selected-report-id state.
    s_report_counter = 0;
    s_report05_counter = 0;
    s_selected_report_id = GC_REPORT_ID_NONE;
    s_bulk_cmd_count = 0;
    memset(s_gc_ltk, 0, sizeof(s_gc_ltk));  // no stale key survives into a fresh session
    g_gc_stage = 0;  // NS2_DIAG: fresh personality entry, no USB activity observed yet
    report_set_rumble(0, 0, 0);
    s_rumble_pulse_armed = false;
}

void switch_gc_reset(void) {
    switch_gc_init();  // identical to init(); may diverge once D/E add more runtime state.
}

void switch_gc_mount(void) {
    // Mirrors ns2_mount()'s role (invalidate host-session-scoped state on a fresh
    // SET_CONFIGURATION). A real re-enumeration is exactly the kind of session boundary the
    // counters and selected-report-id state should not survive across either.
    s_report_counter = 0;
    s_report05_counter = 0;
    s_selected_report_id = GC_REPORT_ID_NONE;
    s_bulk_cmd_count = 0;
    memset(s_gc_ltk, 0, sizeof(s_gc_ltk));
    report_set_rumble(0, 0, 0);
    s_rumble_pulse_armed = false;
    // NS2_DIAG: SET_CONFIGURATION always implies device+config descriptor reads already
    // happened, so this is a direct set (not a high-water-mark floor) -- correctly resets a
    // stale higher value from a previous connection attempt, since a fresh mount() really is a
    // new attempt whose actual progress this diagnostic should reflect.
    g_gc_stage = 3;
}

// Thin wrapper around switch_gc_encode_report05(), mirroring switch_gc_build_report()'s shape.
void switch_gc_build_report05(uint8_t *p) {
    switch_pro_input_t in;
    get_global_gamepad_input(0, &in);
    switch_gc_encode_report05(&in, s_report05_counter++, p);
}

void switch_gc_task(void) {
    if (tud_vendor_available()) {
        uint8_t cmd[64];
        uint32_t n = tud_vendor_read(cmd, sizeof(cmd));
        switch_gc_vendor_dispatch(cmd, n);
    }
    // A pulse can only be extended by another decoded ON command. OFF/ZLP therefore cannot
    // recreate the old latched-rumble failure, even if an explicit STOP is lost.
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (s_rumble_pulse_armed && (int32_t)(now_ms - s_rumble_pulse_until_ms) >= 0) {
        report_set_rumble(0, 0, 0);
        s_rumble_pulse_armed = false;
    }
    // Stream input only after the host has selected a supported report ID via the Confirmed
    // 0x03/0x0A command above -- mirrors switch_pro2.c's own ns2_streaming/ns2_report_id gate
    // shape exactly, and matches this project's own passive-listen test showing a real GC
    // controller stays silent absent it. Cadence: called every main-loop tick, gated by
    // tud_hid_n_ready(), identical to switch_pro2.c's own ns2_task() -- the actual on-wire
    // interval is TinyUSB's own EP scheduling honoring this personality's descriptor bInterval
    // (already Confirmed/Stage-B), not a separate timer this function needs to implement.
    if (tud_hid_n_ready(0)) {
        uint8_t report[63];
        if (s_selected_report_id == 0x05) {
            switch_gc_build_report05(report);
            tud_hid_n_report(0, 0x05, report, sizeof(report));
        } else if (s_selected_report_id == 0x0A) {
            switch_gc_build_report(report);
            tud_hid_n_report(0, 0x0A, report, sizeof(report));
        }
        // GC_REPORT_ID_NONE (or anything else): stay silent, matches genuine hardware absent a
        // supported Select-Input-Report command.
    }
}

void switch_gc_hid_out_report(uint8_t report_id, const uint8_t *data, uint16_t len) {
    // Report 0x03 (rumble) framing: Confirmed -- report ID byte 0x03, 4-byte
    // rumble-data field, 59-byte reserved padding (63 data bytes total;
    // corrected 2026-07-13 from an earlier 37-byte miscount inherited from
    // ndeadly's table -- see docs/switch2-gc/protocol.md "Output Report 0x03").
    // Genuine captured command forms are decoded in switch_gc_rumble_decode.c. In particular,
    // active USB samples are `sequence 00 01 00`, which the previous data[1]-only decoder
    // misclassified as OFF and therefore never forwarded to any physical controller.
    //
    // `report_id`/`data`/`len` are already normalized by usb_descriptors.c's
    // tud_hid_set_report_cb() dispatcher (Phase 4, 2026-07-13) so this function
    // never has to know whether the transfer arrived via interrupt OUT (ID
    // inline at buffer[0]) or control SET_REPORT (ID stripped, passed
    // separately) -- see that dispatcher's own comment for the audited TinyUSB
    // contract. `data` here is ALWAYS the 4-byte rumble field only (report ID
    // already removed), so offsets are data[0..3], not data[1..4].
    //
    switch_gc_rumble_command_t command = switch_gc_decode_rumble(report_id, data, len);
    if (command == SWITCH_GC_RUMBLE_NO_CHANGE) return;

    if (command == SWITCH_GC_RUMBLE_STOP) {
        report_set_rumble(0, 0, 0);
        s_rumble_pulse_armed = false;
        return;
    }

    if (command == SWITCH_GC_RUMBLE_OFF) {
        // Do not erase a just-issued ON before core1's 3 ms feedback poll can observe it. The
        // bounded pulse expiry above will stop it, and OFF never extends the deadline.
        if (!s_rumble_pulse_armed) report_set_rumble(0, 0, 0);
        return;
    }

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    report_set_rumble(0, GC_RUMBLE_ON_AMPLITUDE, GC_RUMBLE_ON_AMPLITUDE);
    s_rumble_pulse_until_ms = now_ms + GC_RUMBLE_PULSE_MS;
    s_rumble_pulse_armed = true;
}

bool switch_gc_vendor_control_xfer(uint8_t rhport, uint8_t stage, const void *request_v) {
    tusb_control_request_t const *request = (tusb_control_request_t const *)request_v;
    if (stage != CONTROL_STAGE_SETUP) return true;

    // Windows WinUSB auto-bind (MS OS 1.0 Extended Compat ID) -- see the
    // switch_gc_ms_os_string_descriptor block above for why this is
    // implemented in Stage B rather than deferred to Stage D.
    if (request->bRequest == GC_MS_OS_VENDOR_CODE && request->wIndex == 0x0004) {
        return tud_control_xfer(rhport, request, (void *)switch_gc_ms_compat_id,
                                sizeof(switch_gc_ms_compat_id));
    }

    // Nintendo Switch 2 console identity handshake over EP0 vendor control -- see this
    // function's own comment above for why this is required for real console recognition
    // (proven by Pro2's own prior fix for the identical gate).
    if (request->bRequest == 0x02 || request->bRequest == 0x03 || request->bRequest == 0x04) {
        if (g_gc_stage < 4) g_gc_stage = 4;  // NS2_DIAG: console is now talking to us over EP0
    }
    switch (request->bRequest) {
        case 0x03: {  // identity block (64 B)
            uint16_t len = request->wLength < sizeof(switch_gc_ctrl_identity)
                               ? request->wLength : (uint16_t)sizeof(switch_gc_ctrl_identity);
            ns2_protocol_trace_record(time_us_32(), (uint8_t)g_usb_personality,
                                      NS2_TRACE_EP0_RESPONSE,
                                      NS2_TRACE_DEVICE_TO_CONSOLE,
                                      request->bRequest, 0,
                                      switch_gc_ctrl_identity, len);
            return tud_control_xfer(rhport, request, (void *)switch_gc_ctrl_identity, len);
        }
        case 0x02: {  // firmware/version info (16 B)
            ns2_gc_build_ep0_info(SWITCH_GC_UNIT_ID, switch_gc_ctrl_info);
            uint16_t len = request->wLength < sizeof(switch_gc_ctrl_info)
                               ? request->wLength : (uint16_t)sizeof(switch_gc_ctrl_info);
            ns2_protocol_trace_record(time_us_32(), (uint8_t)g_usb_personality,
                                      NS2_TRACE_EP0_RESPONSE,
                                      NS2_TRACE_DEVICE_TO_CONSOLE,
                                      request->bRequest, 0,
                                      switch_gc_ctrl_info, len);
            return tud_control_xfer(rhport, request, (void *)switch_gc_ctrl_info, len);
        }
        case 0x04:  // OUT, no data -> ACK
            ns2_protocol_trace_record(time_us_32(), (uint8_t)g_usb_personality,
                                      NS2_TRACE_EP0_RESPONSE,
                                      NS2_TRACE_DEVICE_TO_CONSOLE,
                                      request->bRequest, 0, NULL, 0);
            return tud_control_status(rhport, request);
    }

    // Anything else stalls -- no other EP0 vendor request is documented or observed as required
    // for either PC/Steam or the console-recognition gate above.
    return false;
}

#endif  // NS2_PRO
