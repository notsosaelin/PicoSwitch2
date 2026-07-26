/*
 * Switch 2 Pro Controller (VID 057E / PID 2069) USB emulation.
 * Built only when -DNS2_PRO is set. See docs/switch2/usb-spec.md for the
 * byte-exact protocol this implements (verified against ndeadly's USB capture,
 * captures/usb, Pro Controller 2 = device 7).
 *
 * Command channel = vendor-bulk interface (EP 0x02 OUT / 0x82 IN). Input report
 * 0x09 + rumble report 0x02 ride the HID interface (EP 0x81 IN / 0x01 OUT).
 */
#include <string.h>

#include "tusb.h"
#include "pico/time.h"  // time_us_32() for the report-0x05 IMU timestamp

#include "ns2_pairing_crypto.h"  // shared AES-128/LTK-derivation pairing crypto
#include "ns2_wake.h"    // learn the console wake identity from USB pairing
#include "config.h"      // configured body colour shared with Sony lightbars
#include "report.h"      // shared cross-core controller input
#include "switch_pro.h"  // switch_pro_input_t + SWITCH_MASK_* (Switch 1 layout)
#include "controller_battery.h"
#include "ds5_audio_bridge.h"
#include "switch_pro2.h"
#include "ns2_firmware_profile.h"
#include "ns2_protocol_trace.h"
#include "ns2_native_motion.h"
#include "ns2_motion_probe.h"
#include "ns2_diag_input.h"
#include "ns2_vendor_tx.h"
#include "ns2_nfc_mirror.h"
#include "ns2_virtual_nfc_runtime.h"
#include "ns2_vendor_rx.h"
#include "virtual_amiibo_store.h"
#include "ns2_amiibo_v3.h"
#include "ns2_ds5_motion.h"
#include "usb.h"         // g_usb_config_mode

// This whole module is only built into the NS2 firmware. The vendor-class calls
// below need CFG_TUD_VENDOR, enabled only when NS2_PRO is defined, so guarding
// the body keeps the Switch-1 (-DNS2_PRO=OFF) build linkable.
#ifdef NS2_PRO

#ifdef NS2_AUDIO
#include "device/usbd_pvt.h"    // custom UAC1 class-driver API
#include "class/audio/audio.h"  // AUDIO_FU_CTRL_* constants (CFG_TUD_AUDIO stays disabled)
#endif

//--------------------------------------------------------------------+
// Descriptors
//--------------------------------------------------------------------+

static const uint8_t ns2_device_desc[] = {
    0x12,        // bLength
    0x01,        // bDescriptorType (Device)
    0x00, 0x02,  // bcdUSB 2.00
    0xEF,        // bDeviceClass (Misc, IAD composite)
    0x02,        // bDeviceSubClass
    0x01,        // bDeviceProtocol
    0x40,        // bMaxPacketSize0 64
    0x7E, 0x05,  // idVendor 0x057E (Nintendo)
    0x69, 0x20,  // idProduct 0x2069 (Switch 2 Pro Controller)
    0x10, 0x02,  // bcdDevice 2.10 — deliberately NOT the real 2.00. Hardware A/B settled it:
                 // diag3 (2.10) and diag4 (2.00) BOTH stall the console at the same point, so the
                 // console does NOT gate on bcdDevice. On PC, though, 2.00 collides with the user's
                 // retail PC2 (also 2.00) in Windows' WinUSB cache (key = VID+PID+bcdDevice) → WinUSB
                 // won't bind → no Steam debug loop. 2.10 is a distinct key: console-neutral AND it
                 // preserves the PC/WinUSB proxy we use to exercise the vendor protocol.
    0x01,        // iManufacturer
    0x02,        // iProduct
    0x03,        // iSerialNumber
    0x01,        // bNumConfigurations
};

static const char *ns2_strings[] = {
    (const char[]){0x09, 0x04},   // 0: language id (en-US)
    "Nintendo",                    // 1: manufacturer
    "Switch 2 Pro Controller",     // 2: product
    "00",                          // 3: serial number
};

// HID report descriptor (97 bytes) — exact from the real PC2.
static const uint8_t ns2_report_desc[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x05,        //   Report ID (5)
    0x05, 0xFF,        //   Usage Page (Vendor 0xFF)
    0x09, 0x01,        //   Usage (0x01)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x95, 0x3F,        //   Report Count (63)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x85, 0x09,        //   Report ID (9)
    0x09, 0x01,        //   Usage (0x01)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x15,        //   Usage Maximum (0x15)  -> 21 buttons
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
    0x05, 0xFF,        //   Usage Page (Vendor 0xFF)
    0x09, 0x02,        //   Usage (0x02)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x95, 0x34,        //   Report Count (52)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (0x01)
    0x95, 0x3F,        //   Report Count (63)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0xC0,              // End Collection
};
_Static_assert(sizeof(ns2_report_desc) == 97, "PC2 HID report descriptor must be 97 bytes");

// Configuration descriptor. Two build variants selected by NS2_AUDIO:
//   Option B (default off): IF0 HID + IF1 vendor bulk only (80 B, 2 interfaces).
//   Option A (NS2_AUDIO):   adds the real PC2's 3 audio interfaces (268 B, 5 IF),
//                           serviced by the PC2-specific UAC1 driver below.
// iConfiguration / iInterface are 0 (the retail strings 4/5/6 are unknown).
#ifndef NS2_AUDIO
#define NS2_CONFIG_LEN 80
static const uint8_t ns2_config_desc[] = {
    0x09, 0x02, (NS2_CONFIG_LEN & 0xFF), (NS2_CONFIG_LEN >> 8), 0x02, 0x01, 0x00, 0xC0, 0xFA,
    // IAD + Interface 0: HID
    0x08, 0x0B, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x61, 0x00,  // HID desc, report len 97
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01,  // EP 0x81 interrupt IN, bInterval 1 = 1000Hz FS max
    0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x01,  // EP 0x01 interrupt OUT, bInterval 1
    // IAD + Interface 1: vendor bulk
    0x08, 0x0B, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x01, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,  // EP 0x02 bulk OUT
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,  // EP 0x82 bulk IN
};
#else  // NS2_AUDIO: full 5-interface descriptor matching the retail PC2
#define NS2_CONFIG_LEN 268
static const uint8_t ns2_config_desc[] = {
    0x09, 0x02, (NS2_CONFIG_LEN & 0xFF), (NS2_CONFIG_LEN >> 8), 0x05, 0x01, 0x00, 0xC0, 0xFA,
    // IAD + Interface 0: HID
    0x08, 0x0B, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x61, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01,  // bInterval 1 = 1000Hz FS max (see Option B above)
    0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x01,
    // IAD + Interface 1: vendor bulk
    0x08, 0x0B, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x01, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,
    // IAD: Interfaces 2-4, USB Audio function
    0x08, 0x0B, 0x02, 0x03, 0x01, 0x01, 0x00, 0x00,
    // Interface 2: Audio Control
    0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x0A, 0x24, 0x01, 0x00, 0x01, 0x47, 0x00, 0x02, 0x03, 0x04,        // AC header (wTotalLength 71)
    0x0C, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,  // IN terminal (USB stream)
    0x0A, 0x24, 0x06, 0x02, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00,        // feature unit
    0x09, 0x24, 0x03, 0x03, 0x02, 0x03, 0x00, 0x02, 0x00,              // OUT terminal (headphones)
    0x0C, 0x24, 0x02, 0x04, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,  // IN terminal (mic)
    0x09, 0x24, 0x06, 0x05, 0x04, 0x01, 0x03, 0x00, 0x00,              // feature unit
    0x09, 0x24, 0x03, 0x06, 0x01, 0x01, 0x00, 0x05, 0x00,              // OUT terminal (USB stream)
    // Interface 3: Audio Streaming OUT (speaker), alt 0 + alt 1
    0x09, 0x04, 0x03, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x03, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x01, 0x00, 0x01, 0x00,                          // AS general
    0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00,  // format: 48kHz stereo 16-bit
    0x07, 0x05, 0x03, 0x0D, 0xC0, 0x00, 0x01,                          // EP 0x03 iso OUT 192B
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,                          // CS EP
    // Interface 4: Audio Streaming IN (mic), alt 0 + alt 1
    0x09, 0x04, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x04, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x06, 0x00, 0x01, 0x00,                          // AS general
    0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00,  // format
    0x07, 0x05, 0x83, 0x0D, 0xC0, 0x00, 0x01,                          // EP 0x83 iso IN 192B
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,                          // CS EP
};
#endif
_Static_assert(sizeof(ns2_config_desc) == NS2_CONFIG_LEN, "config descriptor size mismatch");

const uint8_t *ns2_device_descriptor(void) { return ns2_device_desc; }
const uint8_t *ns2_config_descriptor(void) { return ns2_config_desc; }
const uint8_t *ns2_hid_report_descriptor(void) { return ns2_report_desc; }
const char **ns2_string_table(size_t *count) {
    *count = sizeof(ns2_strings) / sizeof(ns2_strings[0]);
    return ns2_strings;
}

//--------------------------------------------------------------------+
// Factory / calibration memory served on 0x02 memory-read commands.
// Values are the real captured factory block (memory_layout.md); the console
// reads 0x13040/0x13060/0x13080/0x130A8/0x130C0/0x130E8 during init.
//--------------------------------------------------------------------+

#define FACTORY_BASE 0x13000u
#define FACTORY_SIZE 0x160u
static uint8_t factory[FACTORY_SIZE];

// Identity + info blocks the console fetches over EP0 vendor control BEFORE it will
// touch the bulk command channel (verified from ndeadly's USB capture, PC2 = device 7;
// see tud_vendor_control_xfer_cb). ns2_ctrl_identity is the first 64 B of factory memory
// (0x13000: 01 00, serial, VID, PID, 01 06 01, colours) with a 0xFF tail; built in
// ns2_factory_init once `factory` is populated. ns2_ctrl_info is the fixed 16-B reply to
// vendor request 0x02 (firmware/version + a per-unit id). The opaque middle and
// per-unit bytes remain verbatim from our capture; only the documented leading
// firmware triplet advances to the updated retail version.
static uint8_t ns2_ctrl_identity[64];
static uint8_t ns2_ctrl_info[16];

// Command 0x10/0x01 response payload:
// firmware[3], controller type, Bluetooth patch[3], pad, DSP[3], pad.
static uint8_t ns2_firmware_info[12];

static void fac(uint32_t addr, const uint8_t *d, size_t n) {
    uint32_t o = addr - FACTORY_BASE;
    if (o + n <= FACTORY_SIZE) memcpy(&factory[o], d, n);
}

static void ns2_factory_init(void) {
    static const uint8_t unit_id[6] = {0x9E, 0x2B, 0xAB, 0xAB, 0xA9, 0x3C};
    static const uint8_t blk[40] = {
        0x01, 0xAD, 0xD9, 0x9A, 0x55, 0x56, 0x65, 0xA0, 0x00, 0x0A, 0xA0, 0x00, 0x0A, 0xE2,
        0x20, 0x0E, 0xE2, 0x20, 0x0E, 0x9A, 0xAD, 0xD9, 0x9A, 0xAD, 0xD9, 0x0A, 0xA5, 0x50,
        0x0A, 0xA5, 0x50, 0x2F, 0xF6, 0x62, 0x2F, 0xF6, 0x62, 0x0A, 0xFF, 0xFF};
    memset(factory, 0, sizeof(factory));
    ns2_firmware_build_ep0_info(ns2_ctrl_info, unit_id);
    ns2_firmware_build_command_info(0x02, ns2_firmware_info);  // 0x02 = Pro Controller
    fac(0x13000, (const uint8_t[]){0x01, 0x00}, 2);
    fac(0x13002, (const uint8_t[]){0x48, 0x45, 0x4A, 0x37, 0x31, 0x30, 0x30, 0x31, 0x31, 0x32,
                                   0x31, 0x32, 0x34, 0x37, 0x00, 0x00}, 16);  // serial
    fac(0x13012, (const uint8_t[]){0x7E, 0x05}, 2);  // VID
    fac(0x13014, (const uint8_t[]){0x69, 0x20}, 2);  // PID
    fac(0x13016, (const uint8_t[]){0x01, 0x06, 0x01}, 3);
    uint8_t body_color[3];
    config_get_body_color(body_color);
    fac(0x13019, body_color, sizeof(body_color));          // configured body colour
    fac(0x1301C, (const uint8_t[]){0xA0, 0xA0, 0xA0}, 3);  // button colour
    fac(0x1301F, (const uint8_t[]){0xE6, 0xE6, 0xE6}, 3);  // highlight
    fac(0x13022, (const uint8_t[]){0x32, 0x32, 0x32}, 3);  // grip
    fac(0x13040, (const uint8_t[]){0x3B, 0xE0, 0xD3, 0x41, 0xC6, 0x60, 0x6A, 0xBC, 0x4D, 0xD7,
                                   0xA2, 0xBB, 0x71, 0x1E, 0xDD, 0x37}, 16);
    // 0x13060..0x1307F (32 B) reads back erased/unprogrammed (0xFF) on a real unit, not the
    // previously-hardcoded `4C 09 00 00` (source unknown/unannotated). Confirmed by TWO
    // independent real-capture sources (2026-07-12): ndeadly's raw USB capture (a 32-byte read
    // at this address, all 0xFF) and Dycool/NS-PC-Control's own factory-table comment ("reads
    // back erased (0xFF) on the real unit — Captured read: addr=0x13060 len=0x20"). The
    // factory[] array default-zero-fills anything not explicitly populated (see the memset
    // above), so this region must be set to 0xFF explicitly, not just left unpopulated.
    fac(0x13060, (const uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 32);
    fac(0x13080, blk, 40);
    fac(0x130A8, (const uint8_t[]){0xB3, 0x67, 0x83, 0x2E, 0x66, 0x5E, 0x3A, 0x06, 0x5F}, 9);  // L stick cal
    fac(0x130C0, blk, 40);
    fac(0x130E8, (const uint8_t[]){0x2C, 0x08, 0x84, 0xD1, 0x65, 0x63, 0x2A, 0x26, 0x62}, 9);  // R stick cal
    // Magnetometer bias (zero, unit never calibrated) + accelerometer bias, both float32 x,y,z —
    // decoded 2026-07-10 (docs/switch2/report-0x09-motion.md "Factory motion calibration") from
    // this repo's own SPI dump but never wired into the served memory table until 2026-07-12,
    // when a fresh independent decode of a real console-side USB capture (ndeadly's
    // rumble-procon-gccon.pcapng, a DIFFERENT physical unit) confirmed the console actually reads
    // this exact address (0x02/04, len 0x18) during init and expects non-zero data there — this
    // repo was previously returning zero-fill (memset default) for any read at 0x13100, a real
    // gap between documented-and-decoded vs. actually-served. Exact bytes are this repo's own
    // unit's SPI dump (`dumps/SPI/2069_spi_dump_2026-07-10_1422.bin`), for internal consistency
    // with the surrounding factory-block entries above.
    fac(0x13100, (const uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0xC1, 0xF9, 0x23, 0x3E, 0x51, 0xAC, 0x8C, 0xBD,
                                   0x21, 0x0F, 0x26, 0x41}, 24);
    fac(0x13140, (const uint8_t[]){0x00, 0xD7, 0xA3, 0xBC, 0x41, 0xD7, 0xA3, 0xBC, 0x41}, 9);

    // Identity block for the EP0 vendor 0x03 request = first 0x25 B of factory (01 00,
    // serial, VID, PID, 01 06 01, colours) then 0xFF fill to 64 (matches the capture).
    memset(ns2_ctrl_identity, 0xFF, sizeof(ns2_ctrl_identity));
    memcpy(ns2_ctrl_identity, factory, 0x25);
}

static void ns2_mem_read(uint8_t subcommand, uint32_t addr, uint8_t len, uint8_t *out) {
    ns2_firmware_diagnostics_record_read(subcommand, addr, len);
    for (uint8_t i = 0; i < len; i++) {
        uint32_t a = addr + i;
        if (a >= FACTORY_BASE && a < FACTORY_BASE + FACTORY_SIZE)
            out[i] = factory[a - FACTORY_BASE];
        else if (a == 0x1FA000)
            out[i] = 0x00;  // Bluetooth pairing entry count = 0 (no bonds over USB)
        else {
            // Sparse, observed post-update state supplied by the selected profile.
            if (!ns2_firmware_profile_flash_byte(a, &out[i]))
                out[i] = 0xFF;  // uninitialised flash reads as 0xFF
        }
    }
}

//--------------------------------------------------------------------+
// Bluetooth-pairing crypto (the console runs this over USB during first pairing).
// Verified against ndeadly's capture:
//   LTK        = reverse(A1) XOR reverse(B1)      (A1 from 0x15/04, B1 public)
//   B2(onwire) = AES128_ECB(LTK, reverse(A2))     (A2 from 0x15/02 challenge)
// The actual AES-128/rev16/LTK-derivation math was extracted 2026-07-13 into the shared,
// host-tested src/ns2_pairing_crypto.c (see include/ns2_pairing_crypto.h) so switch_gc.c can
// also perform real pairing crypto instead of placeholder bytes -- this file just keeps its own
// session-scoped key state and the two thin wrappers below, byte-for-byte identical behavior to
// before the extraction (verified: tools/test_ns2_pairing_crypto.c's FIPS-197 vector + this
// file's own unchanged hardware-validated pairing history).
//--------------------------------------------------------------------+

// Public device key B1 (constant), used to derive the LTK with the console's A1.
static const uint8_t ns2_device_key_b1[16] = {
    0x5C, 0xF6, 0xEE, 0x79, 0x2C, 0xDF, 0x05, 0xE1,
    0xBA, 0x2B, 0x63, 0x25, 0xC4, 0x1A, 0x5F, 0x10};
static uint8_t ns2_ltk[16];

// 0x15/04: LTK = reverse(A1) XOR reverse(B1).
static void ns2_pair_set_ltk(const uint8_t *a1_wire) {
    ns2_pairing_derive_ltk(a1_wire, ns2_device_key_b1, ns2_ltk);
}

// 0x15/02: B2(wire) = AES128_ECB(LTK, reverse(A2)).
static void ns2_pair_challenge(const uint8_t *a2_wire, uint8_t *b2_wire) {
    ns2_pairing_challenge(ns2_ltk, a2_wire, b2_wire);
}

//--------------------------------------------------------------------+
// Command protocol (vendor-bulk endpoints)
//--------------------------------------------------------------------+

// Input report the host selected via 0x03/0A: the console picks 0x09; a PC /
// Steam "Switch Pro Controller" profile picks 0x05. Default = PC2 power-up 0x09.
static uint8_t ns2_report_id = 0x09;

// Stream input only after the host selects a report (0x03/0A). A real PC2 is silent
// on the HID endpoint until enabled; a strict console may refuse to init a device
// that sends unsolicited reports. A WinUSB PC also sends 0x03/0A, so Steam still works.
static bool ns2_streaming = false;

// IMU feature gate. A real PC2 streams report 0x09 with motion length 0 until the host enables the
// IMU via the 0x0C/0x04 feature command (mask 0x27) — Experiment C. We withhold the report-0x09
// motion block until then (report 0x05 / Steam is unaffected). Reset per host session (tud_mount_cb).
static bool ns2_imu_enabled = false;

// Debug instrumentation: the motion length byte the last report-0x09 build emitted, and
// a getter for the USB-side report state (read by config mode to bisect the gyro pipeline).
static uint8_t ns2_dbg_motion_len = 0;
void ns2_dbg_report_state(uint8_t *report_id, uint8_t *streaming, uint8_t *motion_len) {
    if (report_id) *report_id = ns2_report_id;
    if (streaming) *streaming = ns2_streaming ? 1 : 0;
    if (motion_len) *motion_len = ns2_dbg_motion_len;
}

// Motion-integration state for report 0x09, promoted to file scope (was function-local
// statics) so ns2_dbg_motion_bias() below can expose it live — see the 2026-07-10 (test 2)
// hardware finding that the stillness gate may never fire; this lets the next hardware pass
// confirm it directly instead of guessing again.
static uint16_t ns2_imu_tick;
static uint32_t ns2_phase[3] = { 0, 0, 0x80000000u };  // Z starts ~ -180 deg (from capture)
static uint32_t ns2_motion_last_us;
static int32_t  ns2_gyro_lp[3];      // gyro low-pass, <<6 fixed point
static int32_t  ns2_gyro_bias[3];    // slow bias estimate, <<6 fixed point
static int32_t  ns2_gyro_prev_raw[3];
static int32_t  ns2_gyro_jitter[3];  // EMA of |raw - prev_raw|, <<6 fixed point — the stillness signal
static uint8_t  ns2_dbg_still;       // stillness gate state from the most recent report
static bool     ns2_native_hold_active;
static uint16_t ns2_native_hold_previous_tick;
static ns2_ds5_motion_state_t ns2_ds5_motion;
static bool ns2_ds5_motion_enabled = true;
static bool ns2_ds5_motion_source_active;
static uint32_t ns2_ds5_motion_last_sequence;
static uint8_t ns2_ds5_motion_report[30];
static bool ns2_ds5_motion_report_valid;
static bool ns2_ds5_motion_probe_active;
static int16_t ns2_ds5_motion_probe_gyro[3];

// Debug: bias estimate (converted to raw LSB units) + whether the stillness gate is
// currently open. If `still` never reads 1 while the controller sits motionless on real
// hardware, the gate itself is broken (not just under-tuned) — read this before touching
// the bias-tracker constants again.
void ns2_dbg_motion_bias(int32_t bias_out[3], uint8_t *still_out) {
    if (bias_out) for (int i = 0; i < 3; i++) bias_out[i] = ns2_gyro_bias[i] >> 6;
    if (still_out) *still_out = ns2_dbg_still;
}

// Debug: the CURRENT report-0x09 phase[] accumulator (raw int32, binary-angle units — 2^32 ==
// 360 deg), as it would be written into the report right now if the IMU were enabled. Added
// 2026-07-10 after the symptom was reclassified from gradual "drift" to abrupt multidirectional
// jumps: bias/still alone can't distinguish "our own phase computation has a discontinuity" from
// "our math is smooth but the console expects a different value semantic (e.g. a bounded raw
// sample rather than an unbounded accumulator, per the sibling native-BLE motion format —
// docs/experiments/switch2_native_motion_map_DyCOOL.md)." Watching this while the controller sits
// still answers the first question directly, with no console needed.
void ns2_dbg_motion_phase(int32_t phase_out[3]) {
    if (phase_out) for (int i = 0; i < 3; i++) phase_out[i] = (int32_t)ns2_phase[i];
}

void ns2_dbg_ds5_motion(ns2_ds5_motion_diag_t *out) {
    if (!out) return;
    out->enabled = ns2_ds5_motion_enabled ? 1u : 0u;
    out->source_active = ns2_ds5_motion_source_active ? 1u : 0u;
    out->initialized = ns2_ds5_motion.initialized ? 1u : 0u;
    out->has_sample = ns2_ds5_motion.has_sample ? 1u : 0u;
    out->probe_active = ns2_ds5_motion_probe_active ? 1u : 0u;
    memcpy(out->probe_gyro, ns2_ds5_motion_probe_gyro,
           sizeof(out->probe_gyro));
    for (unsigned i = 0; i < 3; ++i) {
        out->input_gyro[i] = (int16_t)ns2_ds5_motion.gyro_prev[i];
        out->bias_gyro[i] =
            ns2_ds5_motion.gyro_bias[i] / 64;
        out->corrected_gyro[i] =
            ns2_ds5_motion.gyro_corrected[i];
        out->jitter[i] =
            ns2_ds5_motion.gyro_jitter[i] / 64;
        out->gyro_map[i] = ns2_ds5_motion.gyro_map[i];
    }
    out->carrier = (uint8_t)ns2_ds5_motion.carrier;
    out->body_frame = ns2_ds5_motion.body_frame ? 1u : 0u;
    for (unsigned i = 0; i < 4; ++i)
        out->quaternion_million[i] =
            (int32_t)(ns2_ds5_motion.quaternion[i] * 1000000.0f);
    out->updates = ns2_ds5_motion.updates;
    out->representation_rejects =
        ns2_ds5_motion.representation_rejects;
    out->host_dt_us = ns2_ds5_motion.last_host_elapsed_us;
    out->sensor_dt_us = ns2_ds5_motion.last_sensor_elapsed_us;
    out->sensor_dt_max_us = ns2_ds5_motion.max_sensor_elapsed_us;
    out->timestamp_fallbacks =
        ns2_ds5_motion.sensor_timestamp_fallbacks;
    out->timestamp_invalid =
        ns2_ds5_motion.sensor_timestamp_invalid;
    out->sequence_gaps = ns2_ds5_motion.sequence_gaps;
    out->integration_substeps =
        ns2_ds5_motion.integration_substeps;
}

bool ns2_dbg_ds5_motion_enabled(void) {
    return ns2_ds5_motion_enabled;
}

void ns2_dbg_ds5_motion_set_enabled(bool enabled) {
    if (ns2_ds5_motion_enabled == enabled) return;
    ns2_ds5_motion_enabled = enabled;
    // Do not resume from orientation or learned bias state that aged while
    // the diagnostic gate was closed.
    ns2_ds5_motion_reset(&ns2_ds5_motion);
    ns2_ds5_motion_source_active = false;
    ns2_ds5_motion_last_sequence = 0;
    ns2_ds5_motion_report_valid = false;
    ns2_dbg_ds5_motion_probe_off();
}

bool ns2_dbg_ds5_motion_probe_rate(uint8_t axis, int16_t rate) {
    if (axis >= 3u) return false;
    memset(ns2_ds5_motion_probe_gyro, 0,
           sizeof(ns2_ds5_motion_probe_gyro));
    ns2_ds5_motion_probe_gyro[axis] = rate;
    ns2_ds5_motion_probe_active = true;
    return true;
}

void ns2_dbg_ds5_motion_probe_off(void) {
    ns2_ds5_motion_probe_active = false;
    memset(ns2_ds5_motion_probe_gyro, 0,
           sizeof(ns2_ds5_motion_probe_gyro));
}

void ns2_dbg_ds5_motion_set_body_frame(bool body_frame) {
    ns2_ds5_motion_set_body_frame(&ns2_ds5_motion, body_frame);
}

bool ns2_dbg_ds5_motion_set_map(const int8_t map[3]) {
    return ns2_ds5_motion_set_gyro_map(&ns2_ds5_motion, map);
}

void ns2_dbg_ds5_motion_set_carrier(uint8_t carrier) {
    ns2_ds5_motion_carrier_t selected = NS2_DS5_CARRIER_SWITCH2_WXYZ;
    if (carrier == (uint8_t)NS2_DS5_CARRIER_SWITCH1_DSCALE)
        selected = NS2_DS5_CARRIER_SWITCH1_DSCALE;
    else if (carrier == (uint8_t)NS2_DS5_CARRIER_LEGACY_STATE0)
        selected = NS2_DS5_CARRIER_LEGACY_STATE0;
    ns2_ds5_motion_set_carrier(&ns2_ds5_motion, selected);
}

// The tracked timing word for the CURRENT phase[] values (set inside ns2_motion_tick(),
// consumed by ns2_build_report() when actually writing report bytes).
static uint16_t ns2_motion_timing;

// Serialize timing/temperature/phase/accel into the 30-byte report-0x09 motion block
// (docs/switch2/report-0x09-motion.md layout). Shared by ns2_build_report() (the real,
// transmitted report) and the anomaly capture below (ns2_last_anom.motion_bytes) — using
// ONE function for both means "does the phase->bytes serialization ever diverge from what's
// actually sent" is answered by code structure (it physically cannot: same function, same
// inputs, called at the same point), not by a separate, potentially-drifting copy of the
// byte-packing logic.
static void ns2_encode_motion30(uint8_t out[30], uint16_t timing, const int32_t phase[3],
                                 const int16_t accel[3]) {
    out[0x00] = (uint8_t)timing;  out[0x01] = (uint8_t)(timing >> 8);
    out[0x02] = 0x00;             out[0x03] = 0x0C;  // temperature 0x0C00
    for (int ax = 0; ax < 3; ax++) {
        uint8_t *q = &out[0x04 + ax * 4];
        int32_t v = phase[ax];
        q[0] = (uint8_t)v;         q[1] = (uint8_t)(v >> 8);
        q[2] = (uint8_t)(v >> 16); q[3] = (uint8_t)(v >> 24);
    }
    for (int ax = 0; ax < 3; ax++) {  // accel -> Q16.16 (integer counts << 16; 4096 = 1 g)
        int32_t a = (int32_t)accel[ax] * 65536;
        uint8_t *q = &out[0x10 + ax * 4];
        q[0] = (uint8_t)a;         q[1] = (uint8_t)(a >> 8);
        q[2] = (uint8_t)(a >> 16); q[3] = (uint8_t)(a >> 24);
    }
    out[0x1C] = 0; out[0x1D] = 0;  // tail
}

// ---- Discontinuity detector: bound derived from ns2_motion_tick()'s own arithmetic, not
// chosen. Every value that can flow into a single phase increment is independently bounded:
//   - in->gyro[ax] is int16_t (type-bounded, [-32768,32767]) and additionally clamped to that
//     same range by ns2_clamp16() in ns2_seam.c before it ever reaches here.
//   - ns2_gyro_lp[] is an EMA (weight 1/4) of that bounded input. By induction (a convex
//     combination — glp_next = glp + (input-glp)>>2 — of values already in [-M,M] with an
//     input in [-M,M] stays in [-M,M], and glp starts at 0), |ns2_gyro_lp[ax]| <= 32768<<6.
//   - ns2_gyro_bias[] is the same EMA shape (weight 1/256) tracking ns2_gyro_lp[], so by the
//     same induction |ns2_gyro_bias[ax]| <= 32768<<6 too.
//   - g = (ns2_gyro_lp - ns2_gyro_bias) >> 6. By the triangle inequality, |g| <=
//     2 * 32768<<6 / 64 = 2 * 32768 -- the honest worst case if glp and bias were ever at
//     opposite extremes simultaneously (not expected of well-behaved sensor data, but not
//     excluded by the code's own logic, so used here rather than a tighter guess).
//   - dt_us is clamped to [500,16000] by this same function, a few lines below.
// NS2_MAX_PHASE_DELTA is the largest |phase increment| this arithmetic can produce without a
// computation defect (overflow, a clamp bypassed, memory corruption). It is a ceiling derived
// from the code as written, not a heuristic "this looks too big" guess -- exceeding it proves a
// defect in this function specifically, independent of what the phase VALUE should mean.
#define NS2_MAX_G_MAGNITUDE 65536   // 2 * int16 range; see derivation above
#define NS2_MAX_DT_US       16000   // this function's own dt_us ceiling, a few lines below
#define NS2_MAX_PHASE_DELTA \
    ((int32_t)((int64_t)NS2_MAX_G_MAGNITUDE * NS2_MAX_DT_US * 72818 / 100000))  // ~763.5M, ~64.0 deg

// Small ring buffer of recent ticks' lightweight state, kept purely so an anomaly capture (below)
// has real preceding context ("surrounding" state), not just the offending tick in isolation.
// NS2_ANOM_TRAIL is defined in switch_pro2.h (shared with ns2_anom_capture_t's trail[] sizing).
static ns2_anom_trail_t ns2_anom_trail[NS2_ANOM_TRAIL];
static uint8_t ns2_anom_trail_pos;

// Full context captured the moment a phase increment exceeds NS2_MAX_PHASE_DELTA. Pure
// observability: nothing here feeds back into ns2_phase[], the bias tracker, or the transmitted
// report -- this task is explicitly "detect and expose," not "correct."
static ns2_anom_capture_t ns2_last_anom;
static uint32_t ns2_anom_seq;  // total anomalies seen since boot (0 = none yet)

const ns2_anom_capture_t *ns2_dbg_motion_anomaly(void) { return &ns2_last_anom; }

// One step of the motion integration/bias-tracking state machine (timing, stillness gate,
// bias tracker, phase integration) — everything EXCEPT writing bytes into a report buffer.
// Deliberately independent of ns2_imu_enabled/ns2_streaming: a genuine PC2 withholds the
// motion BLOCK until the host negotiates it, but there's no reason the internal state has to
// wait too, and (2026-07-10 hardware finding) it must not, structurally — see
// ns2_motion_debug_tick() below for why.
static void ns2_motion_tick(const switch_pro_input_t *in) {
    // Real elapsed time since the last tick -> 800 Hz IMU ticks. The genuine timing word
    // tracks REAL time (sample_count 3/4), not a fixed pattern; this also gives the exact dt
    // for integration so the console's rate reconstruction is consistent.
    uint32_t now = time_us_32();
    uint32_t dt_us = now - ns2_motion_last_us;
    ns2_motion_last_us = now;
    if (dt_us < 500)   dt_us = 500;    // clamp (first tick / stalls): ~2 kHz .. ~60 Hz
    if (dt_us > 16000) dt_us = 16000;
    uint8_t count = (uint8_t)((dt_us + 625) / 1250);   // 1 tick = 1/800 s = 1250 us
    if (count < 1)  count = 1;
    if (count > 15) count = 15;
    ns2_imu_tick = (uint16_t)((ns2_imu_tick + count) & 0x0FFF);
    ns2_motion_timing = (uint16_t)(((uint16_t)count << 12) | ns2_imu_tick);

    // Angular phase = integral of rate over real dt. The genuine gyro is extremely clean
    // (~0.05 dps noise, ~0.03 dps bias); a DualSense is noisier AND has non-negligible bias,
    // so low-pass the gyro first (EMA a=0.25, <<6 fixed point so slow aiming still resolves).
    //
    // Stationary drift fix, take 2 (2026-07-10, second hardware pass — see
    // gyro-hardware-validation-2026-07-10.md §6). Take 1 gated the bias tracker on RAW GYRO
    // MAGNITUDE (|raw| < 40 LSB), which is self-defeating: a MEMS gyro's constant zero-rate
    // bias is part of its magnitude, so if the DualSense's bias alone exceeds the threshold
    // (very plausible — consumer MEMS bias is commonly several dps, and 40 LSB is only ~2.4
    // dps at this 16.384 LSB/dps scale), the "still" gate never opens even when the
    // controller is dead still, the bias estimate never adapts, and the fix is a no-op —
    // which matches the second test's symptom exactly (drift persisted unchanged). Stillness
    // is redefined as a STEADY reading (small frame-to-frame delta), not a SMALL one: a
    // constant bias offset held still has ~zero derivative regardless of its absolute size,
    // so this gates correctly no matter how large the bias turns out to be. Only fast motion
    // (a real derivative) closes the gate now.
    // Per-us-per-LSB phase step (16.384 LSB/dps): 2^32 / (16.384*360*1e6) = 0.72818 (*72818/1e5).
    bool still = true;
    for (int ax = 0; ax < 3; ax++) {
        int32_t d = (int32_t)in->gyro[ax] - ns2_gyro_prev_raw[ax];
        ns2_gyro_prev_raw[ax] = in->gyro[ax];
        if (d < 0) d = -d;
        ns2_gyro_jitter[ax] += ((d << 6) - ns2_gyro_jitter[ax]) >> 3;  // EMA of the derivative
        if (ns2_gyro_jitter[ax] > (6 << 6)) still = false;             // > ~6 LSB/report of change
    }
    ns2_dbg_still = still ? 1 : 0;

    int32_t phase_before[3], g_val[3], increment[3];
    bool anomaly = false;
    for (int ax = 0; ax < 3; ax++) {
        ns2_gyro_lp[ax] += (((int32_t)in->gyro[ax] << 6) - ns2_gyro_lp[ax]) >> 2;  // low-pass
        if (still) ns2_gyro_bias[ax] += (ns2_gyro_lp[ax] - ns2_gyro_bias[ax]) >> 8;  // slow (~seconds)
        int32_t g = (ns2_gyro_lp[ax] - ns2_gyro_bias[ax]) >> 6;
        int32_t inc = (int32_t)(((int64_t)g * dt_us * 72818) / 100000);
        phase_before[ax] = (int32_t)ns2_phase[ax];
        g_val[ax] = g;
        increment[ax] = inc;
        ns2_phase[ax] += (uint32_t)inc;
        if (inc > NS2_MAX_PHASE_DELTA || inc < -NS2_MAX_PHASE_DELTA) anomaly = true;
    }

    // Trail ring buffer updates every tick (anomalous or not) so a capture always has real
    // preceding context, not just the tick that tripped the check.
    ns2_anom_trail_t *slot = &ns2_anom_trail[ns2_anom_trail_pos];
    for (int ax = 0; ax < 3; ax++) { slot->gyro[ax] = in->gyro[ax]; slot->delta[ax] = increment[ax]; }
    slot->still = ns2_dbg_still;
    slot->dt_us = dt_us;
    ns2_anom_trail_pos = (uint8_t)((ns2_anom_trail_pos + 1) % NS2_ANOM_TRAIL);

    if (anomaly) {
        ns2_anom_seq++;
        ns2_last_anom.valid = 1;
        ns2_last_anom.seq = ns2_anom_seq;
        // Chronological order, oldest first: the slot we're about to overwrite next is the
        // oldest surviving entry right now.
        for (int i = 0; i < NS2_ANOM_TRAIL; i++)
            ns2_last_anom.trail[i] = ns2_anom_trail[(ns2_anom_trail_pos + i) % NS2_ANOM_TRAIL];
        for (int ax = 0; ax < 3; ax++) {
            ns2_last_anom.gyro[ax] = in->gyro[ax];
            ns2_last_anom.accel[ax] = in->accel[ax];
            ns2_last_anom.g[ax] = g_val[ax];
            ns2_last_anom.bias[ax] = ns2_gyro_bias[ax] >> 6;
            ns2_last_anom.phase_before[ax] = phase_before[ax];
            ns2_last_anom.phase_after[ax] = (int32_t)ns2_phase[ax];
            ns2_last_anom.delta[ax] = increment[ax];
        }
        ns2_last_anom.still = ns2_dbg_still;
        ns2_last_anom.dt_us = dt_us;
        ns2_last_anom.imu_tick = ns2_imu_tick;
        ns2_last_anom.tick_count = (uint8_t)(ns2_motion_timing >> 12);
        ns2_last_anom.imu_enabled = ns2_imu_enabled ? 1 : 0;
        // What THIS tick's phase/accel would encode to, regardless of whether the IMU gate is
        // currently open — answers "is the 30-byte serialization itself a source of
        // discontinuity" independent of "was this tick actually transmitted." motion_len
        // records whether it really was.
        int32_t phase_now[3] = { (int32_t)ns2_phase[0], (int32_t)ns2_phase[1], (int32_t)ns2_phase[2] };
        ns2_encode_motion30(ns2_last_anom.motion_bytes, ns2_motion_timing, phase_now, in->accel);
        ns2_last_anom.motion_len = ns2_imu_enabled ? 30 : 0;
    }
}

// Shared ~250 Hz gate in front of ns2_motion_tick(). The low-pass/bias/jitter EMAs inside it use
// fixed per-CALL right-shifts (>>2, >>3, >>8), not per-elapsed-time scaling, so their effective
// real-time time-constants are inversely proportional to how often they're called — they were
// tuned (first-cut, unvalidated — see STATUS.md "Technical Debt") assuming a 250 Hz caller.
// The HID report cadence used to BE 250 Hz (bInterval 4), which made calling this once per
// streamed report a no-op gate. Since the poll rate was raised to 1000Hz (bInterval 1, more
// button/stick freshness), ns2_build_report() would otherwise call ns2_motion_tick() up to 4x more
// often than before, silently compressing the bias tracker's ~1s adaptation time to ~250ms with
// nobody having decided that — this gate keeps the tracker's cadence exactly as tuned regardless
// of USB poll rate. (Phase integration itself is dt_us-scaled and unaffected either way; only the
// EMA-shaped bias/jitter/low-pass state depends on call rate.) Shared by both callers: the normal
// streaming path (ns2_build_report()) and the config-mode debug hook below.
static bool ns2_motion_tick_gated(const switch_pro_input_t *in) {
    static uint32_t last_us = 0;
    uint32_t now = time_us_32();
    if (now - last_us < 3800) return false;  // ~250 Hz cap
    last_us = now;
    ns2_motion_tick(in);
    return true;
}

// Config-mode debug hook. ns2_task()/ns2_build_report() — the only place ns2_motion_tick() used
// to run — are NEVER called while the dongle is in config mode (usb.c's main loop takes the
// `if (g_usb_config_mode) { config_cdc_task(); continue; }` branch unconditionally). That made
// the `imu` debug command's bias=[...]/still=... fields dead static memory, frozen at their
// power-on zero, whenever read the only way they're reachable — a real bug found 2026-07-10 from
// hardware output (`bias=[0,0,0] still=0` on a moving, motion-feeding DualSense, which is
// impossible if the tracker were actually running). This runs the same tick independently,
// through the shared ~250 Hz gate above.
void ns2_motion_debug_tick(void) {
    switch_pro_input_t in;
    get_global_gamepad_input(0, &in);
    if (in.has_motion) ns2_motion_tick_gated(&in);
}

// Diagnostic (NS2_DIAG): how far the host got, blinked on the LED by
// pico_switch_platform.c. 0 none · 1 device desc read · 2 config desc read ·
// 3 configured (SET_CONFIGURATION) · 4 first vendor cmd · 5 pairing challenge
// (0x15/02) · 6 pairing finalised (0x15/03) · 7 report selected (0x03/0A).
volatile uint8_t g_ns2_stage = 0;

static ns2_vendor_tx_t ns2_vendor_tx;
static ns2_vendor_rx_t ns2_vendor_rx;
static ns2_virtual_nfc_runtime_t ns2_virtual_nfc_runtime;
static uint8_t ns2_virtual_nfc_raw[VIRTUAL_AMIIBO_RAW_SIZE];
static uint8_t ns2_virtual_nfc_signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE];
static uint32_t ns2_virtual_nfc_operation_generation;
static bool ns2_virtual_nfc_presented_last;

static bool ns2_virtual_nfc_sync_presentation(void)
{
    const bool presented = virtual_amiibo_store_loaded();
    if (presented != ns2_virtual_nfc_presented_last) {
        // Manual Eject/Present is controlled from config context. Reset the
        // core0-owned transaction state on the observed edge so re-presenting
        // starts like a fresh physical tag instead of resuming a stale scan or
        // completed-write removal state.
        ns2_virtual_nfc_runtime_init(&ns2_virtual_nfc_runtime);
        ns2_virtual_nfc_operation_generation = 0;
        ns2_virtual_nfc_presented_last = presented;
    }
    return presented;
}

static size_t vend_write_some(void *context, const uint8_t *data, size_t size) {
    (void)context;
    return tud_vendor_write(data, (uint32_t)size);
}

static void vend_pump(void) {
    if (ns2_vendor_tx_pump(&ns2_vendor_tx, vend_write_some, NULL) != 0)
        tud_vendor_write_flush();
}

static void vend_send(const uint8_t *r, uint16_t len) {
    // Preserve the validated one-shot path for all existing small replies.
    // NFC's 630-byte response uses the queued path and is resumed by ns2_task
    // without blocking HID/audio/BOOTSEL work while the 128-byte FIFO is full.
    if (len <= CFG_TUD_VENDOR_TX_BUFSIZE &&
        !ns2_vendor_tx_active(&ns2_vendor_tx)) {
        tud_vendor_write(r, len);
        tud_vendor_write_flush();
        return;
    }
    if (ns2_vendor_tx_queue(&ns2_vendor_tx, r, len))
        vend_pump();
}

// NTAG I2C 2K ("figure v3", e.g. Kirby Air Riders) present-and-trace serve.
// Deliberately minimal and isolated from the 540 runtime: it reports the v3 tag
// as present and serves the 2048-byte image via the existing status/chunk
// helpers so the UART tracer captures exactly how the console reads a 2 KB tag.
// Every command is already traced by ns2_dispatch(); this only shapes replies.
// Not wired to writes or flash. See docs/switch2/kirby-air-riders-extended-amiibo.md.
static uint8_t ns2_v3_report_state = 0;
static bool ns2_v3_operation_active = false;

static bool ns2_v3_serve(const uint8_t *command, uint32_t length)
{
    static uint8_t image[NS2_AMIIBO_V3_SIZE];
    if (!virtual_amiibo_store_v3_copy(image)) return false;

    const uint8_t sub = command[3];
    const uint8_t *request = command + 8;
    const size_t request_size = length - 8u;
    uint8_t payload[NS2_NFC_READ_CHUNK_PAYLOAD_SIZE];
    size_t payload_size = 0;
    uint8_t direction = 0x04; // bare ACK unless a data reply is produced

    uint8_t uid[7];
    ns2_amiibo_v3_uid(image, uid);

    switch (sub) {
        case 0x03: // scan
            ns2_v3_operation_active = false;
            ns2_v3_report_state = (uint8_t)((ns2_v3_report_state + 1u) & 0x07u);
            break;
        case 0x04: // stop
            ns2_v3_operation_active = false;
            break;
        case 0x05: { // status
            ns2_virtual_nfc_build_status(true, uid, payload);
            payload[0] = 0x09; // ready
            payload[1] = 0x00;
            payload_size = NS2_NFC_STATUS_PAYLOAD_SIZE;
            direction = 0x01;
            break;
        }
        case 0x06: // begin read operation
            ns2_v3_operation_active = true;
            break;
        case 0x15: { // fetch a chunk of the tag image at a little-endian offset
            if (ns2_v3_operation_active && request_size >= 2u) {
                const uint16_t offset =
                    (uint16_t)request[0] | ((uint16_t)request[1] << 8);
                size_t out_size = 0;
                if (ns2_virtual_nfc_build_buffer_chunk(
                        image, NS2_AMIIBO_V3_SIZE, offset, payload,
                        &out_size) == NS2_VIRTUAL_NFC_OK) {
                    payload_size = out_size;
                    direction = 0x01;
                }
            }
            break;
        }
        default:
            break; // ACK-and-trace anything else so the log shows it
    }

    uint8_t packet[8u + NS2_NFC_READ_CHUNK_PAYLOAD_SIZE];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x01;
    packet[1] = direction;
    packet[2] = command[2];
    packet[3] = sub;
    packet[5] = 0xF8;
    if (payload_size) memcpy(packet + 8, payload, payload_size);
    const uint16_t packet_length = (uint16_t)(8u + payload_size);
    ns2_protocol_trace_record(
        time_us_32(), (uint8_t)g_usb_personality, NS2_TRACE_BULK_RESPONSE,
        NS2_TRACE_DEVICE_TO_CONSOLE, packet[0], sub, packet, packet_length);
    vend_send(packet, packet_length);
    return true;
}

static bool ns2_virtual_nfc_dispatch_usb(const uint8_t *command,
                                         uint32_t length)
{
    if (!command || length < 8u || command[0] != 0x01u)
        return false;

    // Experimental v3 path takes precedence when a 2 KB tag is loaded; the 540
    // NTAG215 store is never loaded at the same time, so this cannot disturb it.
    if (virtual_amiibo_store_v3_loaded())
        return ns2_v3_serve(command, length);

    if (!ns2_virtual_nfc_sync_presentation())
        return false;

    virtual_amiibo_status_t status;
    virtual_amiibo_store_status(&status);
    bool tag_present = false;
    uint32_t generation = 0;
    if (status.loaded &&
        virtual_amiibo_store_copy_image(
            ns2_virtual_nfc_raw, ns2_virtual_nfc_signature, NULL,
            &generation) ==
            VIRTUAL_AMIIBO_OK)
        tag_present = true;

    ns2_virtual_nfc_response_t response;
    if (!ns2_virtual_nfc_runtime_dispatch(
            &ns2_virtual_nfc_runtime, to_ms_since_boot(get_absolute_time()),
            command[3], command + 8, length - 8u, tag_present,
            tag_present ? ns2_virtual_nfc_raw : NULL,
            tag_present ? ns2_virtual_nfc_signature : NULL, &response))
        return false;

    if (command[3] == 0x06u &&
        ns2_virtual_nfc_runtime.operation_active)
        ns2_virtual_nfc_operation_generation = generation;

    if (response.write_committed &&
        virtual_amiibo_store_apply_console_write(
            ns2_virtual_nfc_raw,
            ns2_virtual_nfc_operation_generation) != VIRTUAL_AMIIBO_OK) {
        // A browser upload or other selection change won the race. Never
        // overwrite that newer image with a transaction begun against an
        // older generation.
        ns2_virtual_nfc_runtime_write_apply_failed(
            &ns2_virtual_nfc_runtime,
            to_ms_since_boot(get_absolute_time()));
        response.write_committed = false;
    }

    uint8_t packet[8u + NS2_NFC_READ_CHUNK_PAYLOAD_SIZE];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x01;
    packet[1] = response.response_direction;
    packet[2] = command[2];
    packet[3] = command[3];
    packet[5] = 0xF8;
    if (response.payload_size != 0)
        memcpy(packet + 8, response.payload, response.payload_size);
    const uint16_t packet_length =
        (uint16_t)(8u + response.payload_size);
    ns2_protocol_trace_record(
        time_us_32(), (uint8_t)g_usb_personality, NS2_TRACE_BULK_RESPONSE,
        NS2_TRACE_DEVICE_TO_CONSOLE, packet[0], packet[3], packet,
        packet_length);
    vend_send(packet, packet_length);
    return true;
}

// Response header: echo cmd, dir=0x01, echo transport, echo subcmd, ACK 00 f8.
static void ns2_dispatch(const uint8_t *c, uint32_t n) {
    if (n < 8) return;
    uint8_t id = c[0], transport = c[2], sub = c[3];
    ns2_protocol_trace_record(time_us_32(), (uint8_t)g_usb_personality,
                              NS2_TRACE_BULK_COMMAND,
                              NS2_TRACE_CONSOLE_TO_DEVICE, id, sub, c, n);
    if (id == 0x01 && ns2_virtual_nfc_dispatch_usb(c, n))
        return;
    if (id == 0x01 && ns2_nfc_mirror_submit(c, n)) {
        // The UART-gated native-reader path is asynchronous: BTstack sends
        // this to the genuine controller and publishes its matching reply for
        // ns2_task() to return from core0. Do not emit the placeholder ACK.
        return;
    }

    // Fine-grained handshake milestones for the LED tracer, to pinpoint where the console
    // stalls in the long post-pairing sequence (these are strictly ordered in the capture).
    uint8_t stage = 4;                                              // any vendor command
    if (id == 0x15 && sub == 0x02) stage = 5;                       // pairing AES challenge
    else if (id == 0x15 && sub == 0x03) stage = 6;                  // pairing finalised
    else if (id == 0x02 && (sub == 0x04 || sub == 0x01)) stage = 7; // calibration memory read
    else if (id == 0x11) stage = 8;                                 // 0x11 query (after mem reads)
    else if (id == 0x0C && (sub == 0x06 || sub == 0x04)) stage = 9; // feature configure / enable
    else if (id == 0x03 && sub == 0x0A) stage = 10;                 // report selected -> streaming
    if (stage > g_ns2_stage) g_ns2_stage = stage;

    // Must hold the largest reply: memory read = 8 (hdr) + 8 (len/addr) + up to 0x50 data = 96 B.
    // (Was r[72], which the 0x40 memory reads overflowed by 8 B -> stack corruption mid-handshake.)
    uint8_t r[128];
    memset(r, 0, sizeof(r));
    r[0] = id;
    r[1] = 0x01;
    r[2] = transport;
    r[3] = sub;
    r[4] = 0x00;
    r[5] = 0xF8;
    uint8_t *d = &r[8];  // response data area
    uint16_t dl = 0;     // response data length

    switch (id) {
        case 0x03:  // Initialisation
            if (sub == 0x0D) { d[0] = 0x01; dl = 4; }        // Init USB
            else if (sub == 0x03) { d[0] = 0x01; dl = 4; }   // Enable USB HID reports
            else if (sub == 0x0A) {  // Select input report (0x05 or 0x09) -> begin streaming
                if (c[8] == 0x05 || c[8] == 0x09) ns2_report_id = c[8];
                ns2_streaming = true;
                dl = 0;
            }
            else dl = 0;
            break;
        case 0x07:  // first-init command
            d[0] = 0x00; dl = 1;
            break;
        case 0x16:  // unknown, 24 zero bytes
            dl = 24;
            break;
        case 0x15:  // Bluetooth pairing (run over USB)
            if (sub == 0x01) {  // exchange addresses -> return our controller BD_ADDR
                // Same 6 bytes as the EP0 info block tail (ns2_ctrl_info) so our advertised
                // controller address is self-consistent, like the real PC2 (capture frame 463).
                static const uint8_t a[] = {0x01, 0x04, 0x01, 0x9E, 0x2B, 0xAB, 0xAB, 0xA9, 0x3C};
                memcpy(d, a, sizeof(a));
                dl = sizeof(a);
                // The request contains the console address(es) needed by the
                // Switch 2 wake advertisement. Stage now, but persist only if
                // the console later finalises this pairing with 0x15/03.
                if (n >= 8) {
                    ns2_wake_pairing_stage(&c[8], n - 8, 0x2069, &a[3]);
                }
            } else if (sub == 0x02) {  // confirm LTK: B2(wire) = AES128(LTK, rev(A2))
                d[0] = 0x01;
                ns2_pair_challenge(&c[9], &d[1]);  // A2 = c[9..24]
                dl = 17;
            } else if (sub == 0x03) {  // finalise
                d[0] = 0x01; dl = 1;
                ns2_wake_pairing_commit();
            } else if (sub == 0x04) {  // exchange keys: derive LTK from A1, return B1
                ns2_pair_set_ltk(&c[9]);  // A1 = c[9..24]
                d[0] = 0x01;
                memcpy(&d[1], ns2_device_key_b1, 16);
                dl = 17;
            }
            break;
        case 0x09:  // console-assigned player LED bitfield
            if (n > 8) report_set_player_leds(0, c[8]);
            dl = 0;
            break;
        case 0x0C:  // feature select
            if (sub == 0x01) {  // get feature info
                uint8_t f = (n > 8) ? c[8] : 0;
                d[4] = (f & 0x01) ? 0x07 : 0x00;
                d[5] = (f & 0x02) ? 0x07 : 0x00;
                d[6] = (f & 0x04) ? 0x01 : 0x00;
                d[7] = (f & 0x80) ? 0x01 : 0x00;
                d[8] = (f & 0x10) ? 0x01 : 0x00;
                d[9] = (f & 0x20) ? 0x03 : 0x00;
                dl = 12;
            } else if (sub == 0x06) {  // configure features -> 40-byte reply echoing feature id
                // Real PC2 replies with 40 data bytes: zeros except data[4] = the requested
                // feature id (request data[4] = c[12]). Capture frames 9494-9687.
                memset(d, 0, 40);
                d[4] = (n > 12) ? c[12] : 0;
                dl = 40;
            } else {  // set/clear/enable/disable mask
                // 0x0C/0x04 with a non-zero feature mask (0x27) is the command that flips report-0x09
                // motion on in the capture (Experiment C) — use it to gate the IMU block.
                if (sub == 0x04 && n > 8) ns2_imu_enabled = (c[8] != 0);
                dl = 4;
            }
            break;
        case 0x02: {  // flash memory
            uint32_t addr = (uint32_t)c[12] | ((uint32_t)c[13] << 8) |
                            ((uint32_t)c[14] << 16) | ((uint32_t)c[15] << 24);
            if (sub == 0x04) {  // memory read
                uint8_t len = c[8];
                if (len > 0x50) len = 0x50;
                d[0] = len;
                d[4] = c[12]; d[5] = c[13]; d[6] = c[14]; d[7] = c[15];
                ns2_mem_read(sub, addr, len, &d[8]);
                dl = 8 + len;
            } else if (sub == 0x01) {  // read 0x40 block
                d[0] = 0x40;
                d[4] = c[12]; d[5] = c[13]; d[6] = c[14]; d[7] = c[15];
                ns2_mem_read(sub, addr, 0x40, &d[8]);
                dl = 8 + 0x40;
            } else if (sub == 0x05) {  // memory write -> ack (we don't persist)
                d[4] = c[12]; d[5] = c[13]; d[6] = c[14]; d[7] = c[15];
                dl = 8;
            } else {
                dl = 0;
            }
            break;
        }
        case 0x10:  // firmware info (type byte 0x02 = Pro Controller)
            ns2_firmware_diagnostics_record_command();
            memcpy(d, ns2_firmware_info, sizeof(ns2_firmware_info));
            dl = sizeof(ns2_firmware_info);
            break;
        case 0x0B:  // battery
            if (sub == 0x03) { memcpy(d, (const uint8_t[]){0xA5, 0x0E, 0x00, 0x00}, 4); dl = 4; }
            else if (sub == 0x04) { memcpy(d, (const uint8_t[]){0x34, 0x00, 0x83, 0x00}, 4); dl = 4; }
            else dl = 0;
            break;
        case 0x11:  // unknown; opaque values replayed from the USB capture (last 16 B are a
                    // constant shared across units, so replaying is safe). Frames 9152/9386.
            if (sub == 0x01) { d[0] = 0x03; dl = 4; }  // USB form (0x03; the BLE form is 0x01)
            else if (sub == 0x03) {
                static const uint8_t r11_03[29] = {
                    0x01, 0xC0, 0x03, 0x00, 0x00, 0xE7, 0xD0, 0x1C, 0x3B, 0x79, 0x22, 0xA0, 0x3A,
                    0x0A, 0xE8, 0x9C, 0x42, 0x58, 0xA0, 0x0B, 0x42, 0x0A, 0xE8, 0x9C, 0x41, 0x58,
                    0xA0, 0x0B, 0x41};
                memcpy(d, r11_03, sizeof(r11_03));
                dl = sizeof(r11_03);
            }
            else dl = 0;
            break;
        case 0x01:  // NFC
            if (sub == 0x0C) { memcpy(d, (const uint8_t[]){0x61, 0x12, 0x50, 0x10}, 4); dl = 4; }
            else {
                // Bare/no-data NFC acks use dir=0x04, not the default 0x01 — confirmed against
                // the genuine capture for sub 0x01 (packet #30532: `01 04 00 01 00 f8 00 00`),
                // and the same dir=0x04-on-bare-ack shape recurs on an unrelated cmd=0x08 response
                // in the same window. See docs/switch2/nfc-protocol-inventory.md §2.3.
                r[1] = 0x04;
                dl = 0;
            }
            break;
        case 0x18:
            if (sub == 0x01) { memcpy(d, (const uint8_t[]){0, 0, 0x40, 0xF0, 0, 0, 0x60, 0}, 8); dl = 8; }
            else if (sub == 0x03) { d[0] = (n > 8) ? c[8] : 0; dl = 1; }
            else dl = 0;
            break;
        default:  // 0x06 shutdown, 0x0A vibration, and anything else -> bare ACK
            dl = 0;
            break;
    }
    uint16_t response_length = (uint16_t)(8 + dl);
    ns2_protocol_trace_record(time_us_32(), (uint8_t)g_usb_personality,
                              NS2_TRACE_BULK_RESPONSE,
                              NS2_TRACE_DEVICE_TO_CONSOLE, id, sub, r,
                              response_length);
    vend_send(r, response_length);
}

//--------------------------------------------------------------------+
// Input report 0x09 (streamed on the HID IN endpoint)
//--------------------------------------------------------------------+

// Translate the shared input (report.c, published by the joypad-os seam on core1)
// into the Switch 2 Pro Controller report 0x09 layout.
static void ns2_build_report(uint8_t *p) {
    static uint8_t counter = 0;
    switch_pro_input_t in;
    get_global_gamepad_input(0, &in);  // NS2 milestone: single controller (slot 0)
    if (ns2_diag_input_y_pressed(time_us_32()))
        in.buttons[0] |= SWITCH_MASK_Y;

    memset(p, 0, 63);
#ifdef NS2_DS5_AUDIO
    uint8_t const report_counter = counter++;
    p[0x00] = report_counter;
#else
    p[0x00] = counter++;
#endif
    p[0x01] = controller_battery_switch2_power_info(
        in.battery_valid != 0, in.battery_level, in.battery_charging != 0);
    p[0x0C] = virtual_amiibo_store_v3_loaded()
        ? ns2_v3_report_state
        : ns2_virtual_nfc_sync_presentation()
            ? ns2_virtual_nfc_runtime_report_state(&ns2_virtual_nfc_runtime)
            : ns2_nfc_mirror_report_state();

    // Remap the 3-byte button field: report.c uses the Switch 1 Pro bit layout,
    // report 0x09 uses a different assignment (see docs/switch2/usb-spec.md §7).
    uint8_t s0 = in.buttons[0], s1 = in.buttons[1], s2 = in.buttons[2];
    uint8_t b0 = 0, b1 = 0, b2 = 0;
    if (s0 & SWITCH_MASK_B)  b0 |= 0x01;
    if (s0 & SWITCH_MASK_A)  b0 |= 0x02;
    if (s0 & SWITCH_MASK_Y)  b0 |= 0x04;
    if (s0 & SWITCH_MASK_X)  b0 |= 0x08;
    if (s0 & SWITCH_MASK_R)  b0 |= 0x10;
    if (s0 & SWITCH_MASK_ZR) b0 |= 0x20;
    if (s1 & SWITCH_MASK_PLUS) b0 |= 0x40;
    if (s1 & SWITCH_MASK_R3)   b0 |= 0x80;  // right stick click
    if (s2 & SWITCH_MASK_DPAD_DOWN)  b1 |= 0x01;
    if (s2 & SWITCH_MASK_DPAD_RIGHT) b1 |= 0x02;
    if (s2 & SWITCH_MASK_DPAD_LEFT)  b1 |= 0x04;
    if (s2 & SWITCH_MASK_DPAD_UP)    b1 |= 0x08;
    if (s2 & SWITCH_MASK_L)  b1 |= 0x10;
    if (s2 & SWITCH_MASK_ZL) b1 |= 0x20;
    if (s1 & SWITCH_MASK_MINUS) b1 |= 0x40;
    if (s1 & SWITCH_MASK_L3)    b1 |= 0x80;  // left stick click
    if (s1 & SWITCH_MASK_HOME)    b2 |= 0x01;
    if (s1 & SWITCH_MASK_CAPTURE) b2 |= 0x02;
    // Switch 2 extra buttons (in.extra): C / GL / GR. These come from the joypad-os
    // seam — Elite/Edge paddles and Fn buttons, or a native Pro Controller 2's own C/GL/GR.
    if (in.extra & SWITCH_EXTRA_C)  b2 |= 0x10;
    if (in.extra & SWITCH_EXTRA_GL) b2 |= 0x08;
    if (in.extra & SWITCH_EXTRA_GR) b2 |= 0x04;
    p[0x02] = b0;
    p[0x03] = b1;
    p[0x04] = b2;

    // Sticks arrive already packed 12-bit-in-3-bytes (identical to report 0x09).
    memcpy(&p[0x05], in.left_stick, 3);
    memcpy(&p[0x08], in.right_stick, 3);

    p[0x0B] = 0x30;
    // 0x0C NFC.
#ifdef NS2_DS5_AUDIO
    // Only advertise a headset while the source controller's physical jack is
    // occupied; otherwise the Switch must not open its audio stream and route
    // ordinary console audio to a bare DualSense speaker.
    p[0x0D] = controller_headset_switch2_state(in.headset_state,
                                               report_counter);
#endif

    // Motion (IMU) — report-0x09 int32 format, VERIFIED (docs/switch2/report-0x09-motion.md):
    //   [0x0F] u16 timing    (low12 = 800 Hz IMU tick, high4 = ticks elapsed since last report)
    //   [0x11] i16 temperature (0x0C00)
    //   [0x13/0x17/0x1B] i32 angular phase X/Y/Z  (2^32 = 360 deg; the integral of gyro rate)
    //   [0x1F/0x23/0x27] i32 accel X/Y/Z, Q16.16  (65536*4096 = 1 g; integer counts in the high 16)
    //   [0x2B] u16 tail (0)
    // Motion is a NEGOTIATED feature: emit length 0 until the host enables the IMU via 0x0C/0x04
    // (Experiment C) — gates what's WRITTEN below, not the tracker state itself (ns2_motion_tick()
    // always runs on live motion so it keeps working, and is debuggable, regardless of whether a
    // host has negotiated the feature — see ns2_motion_tick()'s comment). Rate-limited to ~250 Hz
    // by ns2_motion_tick_gated() independent of the USB poll rate (see that function's comment) —
    // this call site runs once per report, which is now up to 1000 Hz (bInterval 1).
    // FIRST CUT pending on-console validation: the phase-integration constant and axis SIGNS are
    // best-effort — if the console reads rate too fast/slow, tune PHASE_K; if inverted, flip signs.
    // A genuine Pro Controller 2 can supply the console's native variable-length motion PDU
    // directly over BLE. Prefer that opaque, controller-generated block when fresh: decoding its
    // still-partly-unknown representation only to synthesize the same bytes again would add failure
    // modes without adding information. The side channel publishes only PID 0x2069 report-0x000E
    // data and expires quickly, so every other controller keeps the existing generic IMU path.
    uint16_t source_vid = 0;
    uint16_t source_pid = 0;
    get_global_device(0, NULL, 0, &source_vid, &source_pid);
    ns2_native_motion_snapshot_t native_motion;
    bool native_motion_fresh = ns2_native_motion_snapshot(
        &native_motion, time_us_32(), 50000u); // >6 packets at the verified 133Hz cadence
    bool native_motion_owned = native_motion_fresh &&
        ns2_native_motion_output_slot(native_motion.source_conn_index) == 0 &&
        source_vid == 0x057E && source_pid == 0x2069;
    // Use the decoder's explicit provenance, not Bluetooth SDP identity, for
    // translator ownership. Hardware proved that a genuine DualSense can be
    // fully streaming with VID 0x054C while PID remains 0x0000. The former
    // VID/PID gate therefore skipped this path and fell into the known-bad
    // generic phase encoder, producing violent motion spam. DS4 and other Sony
    // devices remain excluded because only ds5_bt stamps this source value.
    const bool ds5_motion_owned =
        in.motion_source == SWITCH_MOTION_SOURCE_DUALSENSE &&
        in.has_motion;
    if (ds5_motion_owned && ns2_ds5_motion_enabled) {
        if (!ns2_ds5_motion_source_active) {
            ns2_ds5_motion_reset(&ns2_ds5_motion);
            ns2_ds5_motion_last_sequence = 0;
            ns2_ds5_motion_report_valid = false;
        }
        ns2_ds5_motion_source_active = true;
        // USB report generation runs near 1 kHz while a DualSense Bluetooth
        // IMU report normally arrives near 250 Hz. Re-integrating the held
        // sample four times is mathematically redundant and contends with the
        // RAM-resident Opus encoder on core1. Consume each physical sample
        // exactly once; the latest quaternion is still emitted every USB poll.
        if (in.motion_sequence == 0 ||
            in.motion_sequence != ns2_ds5_motion_last_sequence) {
            switch_pro_input_t ds5_motion_input = in;
            if (ns2_ds5_motion_probe_active)
                memcpy(ds5_motion_input.gyro, ns2_ds5_motion_probe_gyro,
                       sizeof(ds5_motion_input.gyro));
            if (ns2_ds5_motion_update(&ns2_ds5_motion,
                                      &ds5_motion_input, time_us_32())) {
                // Encoding the unequal-width Switch 2 quaternion slots uses
                // floating-point scaling. Do it once per physical ~250 Hz IMU
                // sample, not again on every ~1 kHz USB poll. The console can
                // receive the latest complete PDU repeatedly between samples.
                ns2_ds5_motion_report_valid =
                    ns2_ds5_motion_build(&ns2_ds5_motion,
                                         ns2_ds5_motion_report);
            }
            ns2_ds5_motion_last_sequence = in.motion_sequence;
        }
    } else if (ns2_ds5_motion_source_active) {
        // A source change must not carry the former controller's integrated
        // orientation or learned zero-rate bias into a later DS5 session.
        ns2_ds5_motion_reset(&ns2_ds5_motion);
        ns2_ds5_motion_source_active = false;
        ns2_ds5_motion_last_sequence = 0;
        ns2_ds5_motion_report_valid = false;
    }
    uint8_t probe_motion[30];
    bool motion_probe_active = ns2_imu_enabled &&
        ns2_motion_probe_build(probe_motion);
    if (motion_probe_active) {
        p[0x0E] = sizeof(probe_motion);
        memcpy(&p[0x0F], probe_motion, sizeof(probe_motion));
        ns2_native_hold_active = false;
    } else if (ns2_imu_enabled && native_motion_owned) {
        p[0x0E] = native_motion.length;
        memcpy(&p[0x0F], native_motion.data, native_motion.length);
        if (native_motion.held_after_disconnect && native_motion.length == 0x1E) {
            // Preserve the last genuine phase+accel values, but keep the 800Hz timing word
            // advancing so the console receives an explicit zero-angular-velocity sample instead
            // of a disappearing motion block. The USB builder owns this tiny emit-side state;
            // the cross-core snapshot remains immutable.
            uint16_t base_timing = (uint16_t)p[0x0F] | ((uint16_t)p[0x10] << 8);
            uint16_t base_tick = base_timing & 0x0FFFu;
            uint32_t elapsed_us = time_us_32() - native_motion.captured_us;
            uint16_t tick = (uint16_t)((base_tick + 1u + elapsed_us / 1250u) & 0x0FFFu);
            if (!ns2_native_hold_active) {
                ns2_native_hold_previous_tick = base_tick;
                ns2_native_hold_active = true;
            }
            uint16_t count = (uint16_t)((tick - ns2_native_hold_previous_tick) & 0x0FFFu);
            if (count > 15u) count = 15u;
            ns2_native_hold_previous_tick = tick;
            uint16_t held_timing = (uint16_t)((count << 12) | tick);
            p[0x0F] = (uint8_t)held_timing;
            p[0x10] = (uint8_t)(held_timing >> 8);
        } else {
            ns2_native_hold_active = false;
        }
    } else if (ds5_motion_owned) {
        // Reports decoded by the DualSense driver use the native quaternion
        // translator. Its production carrier uses the Switch 2 w/x/y/z
        // smallest-three representation and changes the omitted component
        // when a transmitted component reaches the chart boundary.
        //
        // Keep this on the proven 0x1E carrier. The 2026-07-24 UART experiment
        // proved that a genuine 0x28 template with only timing and G6/G7/G8
        // replaced produces random motion: the unresolved leading/middle
        // lanes are semantically active. See docs/switch2/uart-magprobe.md.
        if (ns2_ds5_motion_enabled && ns2_imu_enabled &&
            ns2_ds5_motion_report_valid) {
            p[0x0E] = sizeof(ns2_ds5_motion_report);
            memcpy(&p[0x0F], ns2_ds5_motion_report,
                   sizeof(ns2_ds5_motion_report));
        }
        ns2_native_hold_active = false;
    } else if (in.has_motion) {
        ns2_motion_tick_gated(&in);
        if (ns2_imu_enabled) {
            p[0x0E] = 30;
            int32_t phase_now[3] = { (int32_t)ns2_phase[0], (int32_t)ns2_phase[1], (int32_t)ns2_phase[2] };
            ns2_encode_motion30(&p[0x0F], ns2_motion_timing, phase_now, in.accel);
        }
    }
    ns2_dbg_motion_len = p[0x0E];  // debug: report-0x09 motion length just emitted (0 or 30)
}

// Report 0x05 (common format): 4-byte buttons + DOCUMENTED accel/gyro block.
// Used by the PC / Steam "Switch Pro Controller" profile (see usb-spec / hid_reports).
static void ns2_build_report_05(uint8_t *p) {
    static uint32_t counter = 0;
    switch_pro_input_t in;
    get_global_gamepad_input(0, &in);
    if (ns2_diag_input_y_pressed(time_us_32()))
        in.buttons[0] |= SWITCH_MASK_Y;

    memset(p, 0, 63);
    p[0] = (uint8_t)counter;
    p[1] = (uint8_t)(counter >> 8);
    p[2] = (uint8_t)(counter >> 16);
    p[3] = (uint8_t)(counter >> 24);
    counter++;

    // 4-byte button field (report-0x05 bit layout).
    uint8_t s0 = in.buttons[0], s1 = in.buttons[1], s2 = in.buttons[2];
    uint8_t b0 = 0, b1 = 0, b2 = 0;
    if (s0 & SWITCH_MASK_ZR) b0 |= 0x80;
    if (s0 & SWITCH_MASK_R)  b0 |= 0x40;
    if (s0 & SWITCH_MASK_A)  b0 |= 0x08;
    if (s0 & SWITCH_MASK_B)  b0 |= 0x04;
    if (s0 & SWITCH_MASK_X)  b0 |= 0x02;
    if (s0 & SWITCH_MASK_Y)  b0 |= 0x01;
    if (s1 & SWITCH_MASK_CAPTURE) b1 |= 0x20;
    if (s1 & SWITCH_MASK_HOME)    b1 |= 0x10;
    if (s1 & SWITCH_MASK_L3)      b1 |= 0x08;
    if (s1 & SWITCH_MASK_R3)      b1 |= 0x04;
    if (s1 & SWITCH_MASK_PLUS)    b1 |= 0x02;
    if (s1 & SWITCH_MASK_MINUS)   b1 |= 0x01;
    if (in.extra & SWITCH_EXTRA_C) b1 |= 0x40;  // C (chat): byte1 bit6 (ndeadly format-0 layout)
    if (s2 & SWITCH_MASK_ZL) b2 |= 0x80;
    if (s2 & SWITCH_MASK_L)  b2 |= 0x40;
    if (s2 & SWITCH_MASK_DPAD_LEFT)  b2 |= 0x08;
    if (s2 & SWITCH_MASK_DPAD_RIGHT) b2 |= 0x04;
    if (s2 & SWITCH_MASK_DPAD_UP)    b2 |= 0x02;
    if (s2 & SWITCH_MASK_DPAD_DOWN)  b2 |= 0x01;
    p[0x4] = b0;
    p[0x5] = b1;
    p[0x6] = b2;
    // byte3: Switch 2 grips + headset. GL/GR mirror the confirmed report-0x09 emit so PC/Steam
    // (which reads report 0x05) sees them too. Layout from the ndeadly BLE viewer (format 0).
    if (in.extra & SWITCH_EXTRA_GL) p[0x7] |= 0x02;
    if (in.extra & SWITCH_EXTRA_GR) p[0x7] |= 0x01;
#ifdef NS2_DS5_AUDIO
    if (in.headset_state != CONTROLLER_HEADSET_NONE) p[0x7] |= 0x10;
#endif

    memcpy(&p[0x0A], in.left_stick, 3);
    memcpy(&p[0x0D], in.right_stick, 3);

    p[0x1F] = 0xA0;  // battery voltage ~4000 mV (0x0FA0 LE)
    p[0x20] = 0x0F;
    p[0x21] = 0x20;  // charge state
    p[0x29] = 0x01;  // always 0x01

    // Motion block @ 0x2A: timestamp(4) temp(2) accelXYZ(6) gyroXYZ(6) — int16 LE.
    // The IMU timestamp @0x2A is a free-running ~1 MHz counter. A real PC2 increments it every
    // report; Steam integrates motion against it, so a frozen (all-zero) timestamp makes gyro
    // appear stuck after one sample. Experiment A: genuine changed 19553/19553 reports, ours
    // 0/20330 (docs/experiments/gyro-experiment-a-results.md). time_us_32() matches the genuine
    // ~0.8 MHz cadence closely enough; only monotonic advance matters. Gated on has_motion so
    // non-IMU pads keep an all-zero block (unchanged behavior) rather than a phantom timestamp.
    if (in.has_motion) {
        uint32_t ts = time_us_32();
        p[0x2A] = (uint8_t)ts;
        p[0x2B] = (uint8_t)(ts >> 8);
        p[0x2C] = (uint8_t)(ts >> 16);
        p[0x2D] = (uint8_t)(ts >> 24);
        p[0x2E] = 0x01;  // constant byte a real PC2 sends here (Experiment A)
        memcpy(&p[0x30], in.accel, 6);
        memcpy(&p[0x36], in.gyro, 6);
    }
}

//--------------------------------------------------------------------+
// Public entry points
//--------------------------------------------------------------------+

// Host completed SET_CONFIGURATION (device configured) — diagnostic stage 3.
// Renamed from tud_mount_cb (2026-07-13): called from usb_descriptors.c's centralized
// tud_mount_cb dispatcher. See docs/switch2-gc/usb-personality.md "TinyUSB dispatch...".
void ns2_mount(void) {
    if (g_ns2_stage < 3) g_ns2_stage = 3;
    ns2_vendor_rx_init(&ns2_vendor_rx);
    ns2_virtual_nfc_runtime_init(&ns2_virtual_nfc_runtime);
    ns2_virtual_nfc_operation_generation = 0;
    ns2_virtual_nfc_presented_last = virtual_amiibo_store_loaded();
    ns2_imu_enabled = false;  // new host session: IMU off until the host re-enables it (0x0C/0x04)
    ns2_ds5_motion_reset(&ns2_ds5_motion);
    ns2_ds5_motion_source_active = false;
    ns2_ds5_motion_last_sequence = 0;
    ns2_ds5_motion_report_valid = false;
    ns2_dbg_ds5_motion_probe_off();
}

static void ns2_dispatch_complete_vendor_command(
    void *context, const uint8_t *command, size_t length)
{
    (void)context;
    ns2_dispatch(command, (uint32_t)length);
}

void ns2_init(void) {
    ns2_vendor_tx_init(&ns2_vendor_tx);
    ns2_vendor_rx_init(&ns2_vendor_rx);
    ns2_virtual_nfc_runtime_init(&ns2_virtual_nfc_runtime);
    ns2_virtual_nfc_operation_generation = 0;
    ns2_virtual_nfc_presented_last = virtual_amiibo_store_loaded();
    ns2_firmware_diagnostics_reset();
    ns2_factory_init();
    ns2_wake_pairing_reset();
    ns2_report_id = 0x09;
    ns2_streaming = false;
    ns2_imu_enabled = false;
    ns2_ds5_motion_reset(&ns2_ds5_motion);
    ns2_ds5_motion_source_active = false;
    ns2_ds5_motion_last_sequence = 0;
    ns2_ds5_motion_report_valid = false;
    ns2_dbg_ds5_motion_probe_off();
}

// Decode one HD-rumble motor's peak amplitude (0..1023) from its 5-byte packed LRA field.
// 40-bit little-endian: freq_0[0:10] | amp_0[10:20] | freq_1[20:30] | amp_1[30:40]
// (format from ndeadly's switch2_input_viewer.py send_vibration). We drive rumble off the
// AMPLITUDE fields only — the old peak-of-all-bytes read the frequency fields, which are
// non-zero at rest and produced a constant idle buzz on HD-rumble pads (Pro Controller 2).
// Cross-validated 2026-07-14 against a second, independent source: the real Linux kernel
// "HID: nintendo" driver's switch2_encode_rumble() (Vicki Pfau, linux-input mailing list v11,
// https://marc.info/?l=linux-input&w=2&r=1&s=hid+switch2&q=b) packs its own `switch2_hd_rumble`
// struct (hi_freq/hi_amp/lo_freq/lo_amp, each 10 bits) into the identical four-consecutive-10-bit
// field layout across 5 bytes -- same field order, same bit widths, same positions. This was
// previously sourced from a single Python reference script; it now has two independent sources
// agreeing byte-for-bit, which is the strongest evidence this decode has had. "amp0"/"amp1" here
// correspond to the kernel's hi_amp/lo_amp -- two frequency bands of the SAME physical motor
// (Nintendo's own dual-frequency HD Rumble composition), not left/right motors -- this function
// is already called once per physical motor (see ns2_hid_out_report() below), so taking the max
// of the two bands to get one scalar per motor remains the right collapse for a downstream
// controller that only has a single amplitude per side.
static uint16_t ns2_rumble_motor_amp(const uint8_t *p) {
    uint64_t packed = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
                      ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32);
    uint16_t amp0 = (packed >> 10) & 0x3FF;
    uint16_t amp1 = (packed >> 30) & 0x3FF;
    return amp0 > amp1 ? amp0 : amp1;
}
void ns2_hid_out_report(uint8_t report_id, const uint8_t *data, uint16_t len) {
    // Rumble output report 0x02: [id][16B left LRA][16B right LRA][9B reserved]; each motor
    // block = [0x50|counter][5B packed freq/amp][zeros]. Each physical motor's own peak (across
    // its 2 internal frequency bands) is forwarded independently, not collapsed to one shared
    // peak, so joypad-os drivers with true per-motor output (feedback_set_rumble()'s left/right —
    // e.g. DualSense, Xbox) preserve stereo separation instead of both motors buzzing identically.
    //
    // `data`/`len` no longer include the report ID (normalized by usb_descriptors.c's
    // tud_hid_set_report_cb() dispatcher, Phase 4 2026-07-13) -- every offset below is shifted
    // by -1 versus this function's pre-2026-07-13 form, which read them out of a combined
    // [id][...] buffer.
    if (!data || report_id != 0x02 || len < 6) return;
    uint16_t left = ns2_rumble_motor_amp(&data[1]);   // left packed = data bytes 1..5 (was buf 2..6)
    uint16_t right = len >= 22 ? ns2_rumble_motor_amp(&data[17]) : 0;  // right packed = data 17..21 (was buf 18..22)
    report_set_rumble(0, (uint8_t)(left >> 2), (uint8_t)(right >> 2));  // 0..1023 -> 0..255
}

void ns2_task(void) {
    vend_pump();
    if (ns2_virtual_nfc_sync_presentation()) {
        ns2_virtual_nfc_runtime_set_write_persisted(
            &ns2_virtual_nfc_runtime,
            !virtual_amiibo_store_persist_pending());
        ns2_virtual_nfc_runtime_tick(
            &ns2_virtual_nfc_runtime,
            to_ms_since_boot(get_absolute_time()));
    }
    if (!ns2_vendor_tx_active(&ns2_vendor_tx)) {
        uint8_t nfc_response[NS2_NFC_MIRROR_RESPONSE_MAX];
        size_t nfc_response_length = 0;
        if (ns2_nfc_mirror_take_usb_response(
                nfc_response, sizeof(nfc_response),
                &nfc_response_length)) {
            ns2_protocol_trace_record(
                time_us_32(), (uint8_t)g_usb_personality,
                NS2_TRACE_BULK_RESPONSE, NS2_TRACE_DEVICE_TO_CONSOLE,
                nfc_response[0], nfc_response[3], nfc_response,
                nfc_response_length);
            vend_send(nfc_response, (uint16_t)nfc_response_length);
        } else if (tud_vendor_available()) {
            uint8_t fragment[CFG_TUD_VENDOR_RX_BUFSIZE];
            uint32_t n = tud_vendor_read(fragment, sizeof(fragment));
            ns2_vendor_rx_feed(
                &ns2_vendor_rx, fragment, n,
                ns2_dispatch_complete_vendor_command, NULL);
        }
    }
    // Stream input only after the host has selected a report (0x03/0A) — a real PC2
    // stays silent on the HID endpoint until then. (See ns2_streaming.)
    if (ns2_streaming && tud_hid_n_ready(0)) {
        uint8_t report[63];
        if (ns2_report_id == 0x05)
            ns2_build_report_05(report);
        else
            ns2_build_report(report);
        tud_hid_n_report(0, ns2_report_id, report, sizeof(report));
    }
}

//--------------------------------------------------------------------+
// MS OS 1.0 descriptors — auto-bind WinUSB to the vendor interface (IF1), like
// the retail PC2 (its MI_01 exposes USB\MS_COMP_WINUSB). Lets Windows / Steam
// drive the command channel over WinUSB, giving a PC-side test loop. The Switch
// console ignores these (Windows-specific); bcdUSB stays 2.00 to match the real
// device. Confined to NS2 normal mode (not config mode).
//--------------------------------------------------------------------+

#define MS_OS_VENDOR_CODE 0x20  // echoed to Windows in the 0xEE OS string descriptor

// OS string descriptor (index 0xEE): "MSFT100" + the vendor request code.
static const uint16_t ns2_ms_os_str[] = {
    0x0312, 'M', 'S', 'F', 'T', '1', '0', '0', MS_OS_VENDOR_CODE};

// Personality-scoped (not just "not config mode"): GameCube has a distinct VID/PID/bcdDevice,
// so Windows probes 0xEE fresh for it too -- must not hand back Pro2's WinUSB compat-ID binding
// while a different personality is active (Stage B doesn't need or want WinUSB for GameCube).
const uint16_t *ns2_ms_os_string_descriptor(void) {
    return g_usb_personality == USB_PERSONALITY_SWITCH2_PRO2 ? ns2_ms_os_str : NULL;
}

// Extended Compat ID OS feature descriptor: WINUSB for interface 1 (vendor).
static const uint8_t ns2_ms_compat_id[] = {
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
_Static_assert(sizeof(ns2_ms_compat_id) == 40, "MS compat ID descriptor must be 40 bytes");

// EP0 vendor control requests. Two independent users:
//  1) Windows MS OS 1.0: bRequest = MS_OS_VENDOR_CODE (0x20), wIndex 0x0004 -> Extended
//     Compat ID (binds WinUSB to IF1 -> our PC/Steam debug loop).
//  2) The Switch 2 console's post-SET_CONFIGURATION identity handshake (verified from
//     ndeadly's USB capture, PC2 = device 7). The console fetches identity over EP0 vendor
//     control FIRST and only proceeds to the bulk command channel once it succeeds:
//       C0 03 (IN, 64 B)              -> identity block (01 00, serial, VID, PID, colours)
//       C0 02 (IN, 16 B)              -> firmware/version + per-unit id
//       40 04 (OUT, wValue 0x0276)    -> no data, ACK
//     Stalling these was why the console configured us (diag stage 3) then went silent.
//     The Switch requests are console-specific; Windows/Steam never send them (no PC impact).
// Renamed from tud_vendor_control_xfer_cb (2026-07-13): this personality's own EP0-vendor
// handler, called from usb_descriptors.c's centralized tud_vendor_control_xfer_cb dispatcher
// now that GameCube is a second personality that also needs a hook here. See
// docs/switch2-gc/usb-personality.md "TinyUSB dispatch and resource constraints".
bool ns2_vendor_control_xfer(uint8_t rhport, uint8_t stage, const void *request_v) {
    tusb_control_request_t const *request = (tusb_control_request_t const *)request_v;
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (g_usb_config_mode) return false;

    // Windows WinUSB auto-bind (MS OS 1.0 Extended Compat ID).
    if (request->bRequest == MS_OS_VENDOR_CODE && request->wIndex == 0x0004) {
        return tud_control_xfer(rhport, request, (void *)ns2_ms_compat_id,
                                sizeof(ns2_ms_compat_id));
    }

    // Nintendo Switch 2 identity handshake over EP0 vendor control.
    switch (request->bRequest) {
        case 0x03: {  // identity block (64 B)
            if (g_ns2_stage < 4) g_ns2_stage = 4;  // console is now talking to us
            uint16_t len = request->wLength < sizeof(ns2_ctrl_identity)
                               ? request->wLength : (uint16_t)sizeof(ns2_ctrl_identity);
            ns2_protocol_trace_record(time_us_32(), (uint8_t)g_usb_personality,
                                      NS2_TRACE_EP0_RESPONSE,
                                      NS2_TRACE_DEVICE_TO_CONSOLE,
                                      request->bRequest, 0, ns2_ctrl_identity, len);
            return tud_control_xfer(rhport, request, ns2_ctrl_identity, len);
        }
        case 0x02: {  // firmware/version info (16 B)
            if (g_ns2_stage < 4) g_ns2_stage = 4;
            ns2_firmware_diagnostics_record_ep0();
            uint16_t len = request->wLength < sizeof(ns2_ctrl_info)
                               ? request->wLength : (uint16_t)sizeof(ns2_ctrl_info);
            ns2_protocol_trace_record(time_us_32(), (uint8_t)g_usb_personality,
                                      NS2_TRACE_EP0_RESPONSE,
                                      NS2_TRACE_DEVICE_TO_CONSOLE,
                                      request->bRequest, 0, ns2_ctrl_info, len);
            return tud_control_xfer(rhport, request, (void *)ns2_ctrl_info, len);
        }
        case 0x04:  // OUT, no data -> ACK
            if (g_ns2_stage < 4) g_ns2_stage = 4;
            ns2_protocol_trace_record(time_us_32(), (uint8_t)g_usb_personality,
                                      NS2_TRACE_EP0_RESPONSE,
                                      NS2_TRACE_DEVICE_TO_CONSOLE,
                                      request->bRequest, 0, NULL, 0);
            return tud_control_status(rhport, request);
    }
    return false;
}

#ifdef NS2_AUDIO
//--------------------------------------------------------------------+
// USB Audio Class 1 driver for the retail PC2 descriptor above.
//
// Pico SDK 2.2.0's generic TinyUSB audio driver accepts UAC2 only (its open()
// explicitly requires bInterfaceProtocol == AUDIO_INT_PROTOCOL_CODE_V2), while
// the PC2 is UAC1. Keep this narrow driver instead of altering the retail
// descriptor. It implements the endpoint lifecycle Windows expects:
//   - IF3 alt 1: 48 kHz stereo 16-bit speaker OUT, consumed into a sink
//   - IF4 alt 1: 48 kHz stereo 16-bit microphone IN, currently silent
//   - writable master mute/volume controls for Feature Units 0x02 and 0x05
//
// The streams are intentionally PCM-complete before Bluetooth audio transport
// is added. Receiving/discarding speaker PCM and continuously supplying silent
// microphone PCM makes the USB audio function operational without claiming that
// audio has already been bridged to a wireless controller.
//--------------------------------------------------------------------+

#define NS2_AUDIO_SPEAKER_ITF 0x03
#define NS2_AUDIO_MIC_ITF     0x04
#define NS2_AUDIO_SPEAKER_EP  0x03
#define NS2_AUDIO_MIC_EP      0x83
#define NS2_AUDIO_PACKET_SIZE 192u

// Standard endpoint descriptors copied byte-for-byte from ns2_config_desc.
// Keeping dedicated copies avoids depending on fragile offsets into the full
// configuration descriptor when an alternate setting is selected.
static const uint8_t ns2_audio_speaker_ep_desc[] = {
    0x07, TUSB_DESC_ENDPOINT, NS2_AUDIO_SPEAKER_EP, 0x0D,
    (NS2_AUDIO_PACKET_SIZE & 0xFF), (NS2_AUDIO_PACKET_SIZE >> 8), 0x01,
};
static const uint8_t ns2_audio_mic_ep_desc[] = {
    0x07, TUSB_DESC_ENDPOINT, NS2_AUDIO_MIC_EP, 0x0D,
    (NS2_AUDIO_PACKET_SIZE & 0xFF), (NS2_AUDIO_PACKET_SIZE >> 8), 0x01,
};

CFG_TUD_MEM_SECTION CFG_TUSB_MEM_ALIGN
static uint8_t ns2_audio_speaker_packet[NS2_AUDIO_PACKET_SIZE];
CFG_TUD_MEM_SECTION CFG_TUSB_MEM_ALIGN
static uint8_t ns2_audio_mic_silence[NS2_AUDIO_PACKET_SIZE];

static uint8_t ns2_audio_alt_speaker;
static uint8_t ns2_audio_alt_mic;
// RP2040/RP2350 ISO allocation persists for the whole USB configuration.
// Track hardware activation and a pending transfer separately from the host's
// current alternate setting so alt 1 -> 0 -> 1 does not restart an already
// active endpoint and corrupt unrelated HID endpoint progress.
static bool ns2_audio_speaker_activated;
static bool ns2_audio_mic_activated;
static bool ns2_audio_speaker_armed;
static bool ns2_audio_mic_armed;
static uint8_t ns2_audio_control_data[2];

// UAC1 volume is signed 1/256 dB. These conservative controls expose -60 dB
// through 0 dB in 1 dB steps, matching what Windows' mixer expects to query.
#define NS2_AUDIO_VOLUME_MIN ((int16_t)-15360)
#define NS2_AUDIO_VOLUME_MAX ((int16_t)0)
#define NS2_AUDIO_VOLUME_RES ((int16_t)256)

typedef struct {
    uint8_t mute;
    int16_t volume;
} ns2_audio_feature_state_t;

static ns2_audio_feature_state_t ns2_audio_speaker_feature;
static ns2_audio_feature_state_t ns2_audio_mic_feature;

static ns2_audio_feature_state_t *ns2_audio_feature(uint8_t unit_id) {
    if (unit_id == 0x02) return &ns2_audio_speaker_feature;
    if (unit_id == 0x05) return &ns2_audio_mic_feature;
    return NULL;
}

static uint16_t ns2_audio_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc,
                               uint16_t max_len) {
    if (itf_desc->bInterfaceClass != TUSB_CLASS_AUDIO) return 0;  // not ours
    // Claim the whole audio function: consume descriptors until the next
    // interface of a different class (or the end of this config descriptor).
    uint8_t const *p = (uint8_t const *)itf_desc;
    uint16_t consumed = 0;
    while (consumed < max_len) {
        if (tu_desc_type(p) == TUSB_DESC_INTERFACE && consumed > 0 &&
            ((tusb_desc_interface_t const *)p)->bInterfaceClass != TUSB_CLASS_AUDIO)
            break;
        uint8_t l = tu_desc_len(p);
        if (l == 0) break;
        consumed += l;
        p += l;
    }
#ifdef TUP_DCD_EDPT_ISO_ALLOC
    // RP2040/RP2350 cannot open an isochronous endpoint through the ordinary
    // dcd_edpt_open path. Reserve DPRAM once while the configuration is opened,
    // then activate the relevant endpoint when its interface selects alt 1.
    if (!usbd_edpt_iso_alloc(rhport, NS2_AUDIO_SPEAKER_EP, NS2_AUDIO_PACKET_SIZE))
        return 0;
    if (!usbd_edpt_iso_alloc(rhport, NS2_AUDIO_MIC_EP, NS2_AUDIO_PACKET_SIZE))
        return 0;
#else
    (void)rhport;
#endif
    return consumed;
}

// UAC1 (not UAC2) request codes — this descriptor's AC header is bcdADC 0x0100, so GET/SET use
// separate opcodes (unlike UAC2, where tinyusb's audio.h AUDIO_CS_REQ_* enum applies instead).
// Table A-9, USB Audio Class 1.0 spec.
#define UAC1_REQ_SET_CUR 0x01
#define UAC1_REQ_GET_CUR 0x81
#define UAC1_REQ_GET_MIN 0x82
#define UAC1_REQ_GET_MAX 0x83
#define UAC1_REQ_GET_RES 0x84

// Feature Unit IDs from ns2_config_desc's AC descriptors above: 0x02 = speaker path (2-channel,
// feeds IF3 Audio Streaming OUT), 0x05 = mic path (mono, feeds IF4 Audio Streaming IN).
static bool ns2_audio_set_alt(uint8_t rhport, uint8_t itf, uint8_t alt) {
    if (alt > 1) return false;

    uint8_t *current_alt;
    uint8_t ep_addr;
    tusb_desc_endpoint_t const *ep_desc;
    uint8_t *packet;
    bool *activated;
    bool *armed;

    if (itf == NS2_AUDIO_SPEAKER_ITF) {
        current_alt = &ns2_audio_alt_speaker;
        ep_addr = NS2_AUDIO_SPEAKER_EP;
        ep_desc = (tusb_desc_endpoint_t const *)ns2_audio_speaker_ep_desc;
        packet = ns2_audio_speaker_packet;
        activated = &ns2_audio_speaker_activated;
        armed = &ns2_audio_speaker_armed;
    } else if (itf == NS2_AUDIO_MIC_ITF) {
        current_alt = &ns2_audio_alt_mic;
        ep_addr = NS2_AUDIO_MIC_EP;
        ep_desc = (tusb_desc_endpoint_t const *)ns2_audio_mic_ep_desc;
        packet = ns2_audio_mic_silence;
        activated = &ns2_audio_mic_activated;
        armed = &ns2_audio_mic_armed;
    } else {
        return false;
    }

    if (*current_alt == alt) return true;
#ifndef TUP_DCD_EDPT_ISO_ALLOC
    if (*current_alt != 0) {
        usbd_edpt_close(rhport, ep_addr);
        *activated = false;
        *armed = false;
    }
#endif
    *current_alt = 0;

    if (alt != 0) {
        if (!*activated) {
#ifdef TUP_DCD_EDPT_ISO_ALLOC
            if (!usbd_edpt_iso_activate(rhport, ep_desc)) return false;
#else
            if (!usbd_edpt_open(rhport, ep_desc)) return false;
#endif
            *activated = true;
        }
        *current_alt = alt;
        // Under the RP ISO allocation API, selecting alt 0 does not close or
        // abort the endpoint. Reuse a transfer that is still pending; if it
        // completed while inactive, the callback cleared `armed` and we
        // safely queue a new one here.
        if (!*armed) {
            if (!usbd_edpt_xfer(rhport, ep_addr, packet,
                                NS2_AUDIO_PACKET_SIZE)) {
#ifndef TUP_DCD_EDPT_ISO_ALLOC
                usbd_edpt_close(rhport, ep_addr);
                *activated = false;
#endif
                *current_alt = 0;
                return false;
            }
            *armed = true;
        }
    }
    ds5_audio_bridge_set_usb_streams(ns2_audio_alt_speaker != 0,
                                     ns2_audio_alt_mic != 0);
    return true;
}

static bool ns2_audio_control(uint8_t rhport, uint8_t stage,
                              tusb_control_request_t const *request) {
    uint8_t const recipient = request->bmRequestType_bit.recipient;
    uint8_t const type = request->bmRequestType_bit.type;

    // TinyUSB forwards standard alternate-setting requests to the owning class
    // driver. Open/close the isochronous endpoint before completing SET_INTERFACE.
    if (type == TUSB_REQ_TYPE_STANDARD && recipient == TUSB_REQ_RCPT_INTERFACE) {
        uint8_t const itf = tu_u16_low(request->wIndex);
        if (stage != CONTROL_STAGE_SETUP) return true;
        if (request->bRequest == TUSB_REQ_GET_INTERFACE) {
            uint8_t *alt = NULL;
            if (itf == NS2_AUDIO_SPEAKER_ITF) alt = &ns2_audio_alt_speaker;
            if (itf == NS2_AUDIO_MIC_ITF) alt = &ns2_audio_alt_mic;
            return alt ? tud_control_xfer(rhport, request, alt, 1) : false;
        }
        if (request->bRequest == TUSB_REQ_SET_INTERFACE) {
            uint8_t const alt = tu_u16_low(request->wValue);
            return ns2_audio_set_alt(rhport, itf, alt)
                       ? tud_control_status(rhport, request)
                       : false;
        }
        return false;
    }

    if (type != TUSB_REQ_TYPE_CLASS || recipient != TUSB_REQ_RCPT_INTERFACE)
        return false;

    uint8_t const unit_id = tu_u16_high(request->wIndex);
    uint8_t const cs = tu_u16_high(request->wValue);
    uint8_t const channel = tu_u16_low(request->wValue);
    ns2_audio_feature_state_t *feature = ns2_audio_feature(unit_id);
    if (!feature || channel != 0) return false;  // only master controls are advertised

    if (request->bRequest == UAC1_REQ_SET_CUR) {
        uint16_t expected_len;
        if (cs == AUDIO_FU_CTRL_MUTE) expected_len = 1;
        else if (cs == AUDIO_FU_CTRL_VOLUME) expected_len = 2;
        else return false;
        if (request->wLength != expected_len) return false;

        if (stage == CONTROL_STAGE_SETUP)
            return tud_control_xfer(rhport, request, ns2_audio_control_data, expected_len);
        if (stage == CONTROL_STAGE_DATA) {
            if (cs == AUDIO_FU_CTRL_MUTE) {
                feature->mute = ns2_audio_control_data[0] ? 1 : 0;
            } else {
                int16_t value = (int16_t)((uint16_t)ns2_audio_control_data[0] |
                                          ((uint16_t)ns2_audio_control_data[1] << 8));
                if (value < NS2_AUDIO_VOLUME_MIN) value = NS2_AUDIO_VOLUME_MIN;
                if (value > NS2_AUDIO_VOLUME_MAX) value = NS2_AUDIO_VOLUME_MAX;
                feature->volume = value;
            }
            if (unit_id == 0x02) {
                ds5_audio_bridge_set_speaker_control(
                    ns2_audio_speaker_feature.mute != 0,
                    ns2_audio_speaker_feature.volume);
            }
        }
        return true;
    }

    if (stage != CONTROL_STAGE_SETUP) return true;
    if (cs == AUDIO_FU_CTRL_MUTE && request->bRequest == UAC1_REQ_GET_CUR)
        return tud_control_xfer(rhport, request, &feature->mute, 1);

    if (cs == AUDIO_FU_CTRL_VOLUME) {
        int16_t const *value = NULL;
        static const int16_t volume_min = NS2_AUDIO_VOLUME_MIN;
        static const int16_t volume_max = NS2_AUDIO_VOLUME_MAX;
        static const int16_t volume_res = NS2_AUDIO_VOLUME_RES;
        if (request->bRequest == UAC1_REQ_GET_CUR) value = &feature->volume;
        else if (request->bRequest == UAC1_REQ_GET_MIN) value = &volume_min;
        else if (request->bRequest == UAC1_REQ_GET_MAX) value = &volume_max;
        else if (request->bRequest == UAC1_REQ_GET_RES) value = &volume_res;
        if (value) return tud_control_xfer(rhport, request, (void *)value, 2);
    }
    return false;
}

static bool ns2_audio_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                           uint32_t xferred_bytes) {
    if (ep_addr == NS2_AUDIO_SPEAKER_EP) {
        ns2_audio_speaker_armed = false;
        if (ns2_audio_alt_speaker == 0) return true;
        // Isochronous packets are allowed to be lost. TinyUSB's own audio
        // driver deliberately ignores the prior transfer result and always
        // rearms the endpoint; leaving it unarmed after one missed packet
        // stops PCM delivery until Windows recovers the interface. Only feed a
        // successful payload to the bridge, but keep listening either way.
        if (result == XFER_RESULT_SUCCESS && xferred_bytes != 0) {
            ds5_audio_bridge_submit_speaker_pcm(ns2_audio_speaker_packet,
                                                (uint16_t)xferred_bytes);
        }
        bool const armed =
            usbd_edpt_xfer(rhport, ep_addr, ns2_audio_speaker_packet,
                           NS2_AUDIO_PACKET_SIZE);
        ns2_audio_speaker_armed = armed;
        return armed;
    }
    if (ep_addr == NS2_AUDIO_MIC_EP) {
        ns2_audio_mic_armed = false;
        if (ns2_audio_alt_mic == 0) return true;
        bool const armed =
            usbd_edpt_xfer(rhport, ep_addr, ns2_audio_mic_silence,
                           NS2_AUDIO_PACKET_SIZE);
        ns2_audio_mic_armed = armed;
        return armed;
    }
    return true;
}

static void ns2_audio_init(void) {
    ns2_audio_alt_speaker = 0;
    ns2_audio_alt_mic = 0;
    ns2_audio_speaker_activated = false;
    ns2_audio_mic_activated = false;
    ns2_audio_speaker_armed = false;
    ns2_audio_mic_armed = false;
    ns2_audio_speaker_feature = (ns2_audio_feature_state_t){0, 0};
    ns2_audio_mic_feature = (ns2_audio_feature_state_t){0, 0};
    ds5_audio_bridge_set_speaker_control(false, 0);
    memset(ns2_audio_mic_silence, 0, sizeof(ns2_audio_mic_silence));
    ds5_audio_bridge_set_usb_streams(false, false);
}

static void ns2_audio_reset(uint8_t rhport) {
    (void)rhport;
    ns2_audio_init();
}

static const usbd_class_driver_t ns2_audio_driver = {
    .init = ns2_audio_init,
    .reset = ns2_audio_reset,
    .open = ns2_audio_open,
    .control_xfer_cb = ns2_audio_control,
    .xfer_cb = ns2_audio_xfer,
};

// The audio driver only owns PC2's audio interfaces (IF2-4), which only exist in
// PC2's own config descriptor -- it must not be offered while a different personality
// (GameCube, CDC config) is active and presenting a completely different interface set.
// See docs/switch2-gc/usb-personality.md "TinyUSB dispatch and resource constraints".
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    if (g_usb_personality != USB_PERSONALITY_SWITCH2_PRO2) {
        *driver_count = 0;
        return NULL;
    }
    *driver_count = 1;
    return &ns2_audio_driver;
}
#endif  // NS2_AUDIO

#endif  // NS2_PRO
