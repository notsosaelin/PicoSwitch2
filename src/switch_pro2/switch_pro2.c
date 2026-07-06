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

#include "report.h"      // shared cross-core controller input
#include "switch_pro.h"  // switch_pro_input_t + SWITCH_MASK_* (Switch 1 layout)
#include "switch_pro2.h"
#include "usb.h"         // g_usb_config_mode

// This whole module is only built into the NS2 firmware. The vendor-class calls
// below need CFG_TUD_VENDOR, enabled only when NS2_PRO is defined, so guarding
// the body keeps the Switch-1 (-DNS2_PRO=OFF) build linkable.
#ifdef NS2_PRO

#ifdef NS2_AUDIO
#include "device/usbd_pvt.h"  // custom class-driver API for the audio stub
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
//                           enumerated by a stub driver (no functional audio).
// iConfiguration / iInterface are 0 (the retail strings 4/5/6 are unknown).
#ifndef NS2_AUDIO
#define NS2_CONFIG_LEN 80
static const uint8_t ns2_config_desc[] = {
    0x09, 0x02, (NS2_CONFIG_LEN & 0xFF), (NS2_CONFIG_LEN >> 8), 0x02, 0x01, 0x00, 0xC0, 0xFA,
    // IAD + Interface 0: HID
    0x08, 0x0B, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x61, 0x00,  // HID desc, report len 97
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x04,  // EP 0x81 interrupt IN
    0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x04,  // EP 0x01 interrupt OUT
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
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x04,
    0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x04,
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
// vendor request 0x02 (firmware/version + a per-unit id, taken verbatim from the capture).
static uint8_t ns2_ctrl_identity[64];
static const uint8_t ns2_ctrl_info[16] = {
    0x01, 0x01, 0x05, 0x00, 0x00, 0x00, 0x0C, 0x00,
    0x00, 0x00, 0x9E, 0x2B, 0xAB, 0xAB, 0xA9, 0x3C};

static void fac(uint32_t addr, const uint8_t *d, size_t n) {
    uint32_t o = addr - FACTORY_BASE;
    if (o + n <= FACTORY_SIZE) memcpy(&factory[o], d, n);
}

static void ns2_factory_init(void) {
    static const uint8_t blk[40] = {
        0x01, 0xAD, 0xD9, 0x9A, 0x55, 0x56, 0x65, 0xA0, 0x00, 0x0A, 0xA0, 0x00, 0x0A, 0xE2,
        0x20, 0x0E, 0xE2, 0x20, 0x0E, 0x9A, 0xAD, 0xD9, 0x9A, 0xAD, 0xD9, 0x0A, 0xA5, 0x50,
        0x0A, 0xA5, 0x50, 0x2F, 0xF6, 0x62, 0x2F, 0xF6, 0x62, 0x0A, 0xFF, 0xFF};
    memset(factory, 0, sizeof(factory));
    fac(0x13000, (const uint8_t[]){0x01, 0x00}, 2);
    fac(0x13002, (const uint8_t[]){0x48, 0x45, 0x4A, 0x37, 0x31, 0x30, 0x30, 0x31, 0x31, 0x32,
                                   0x31, 0x32, 0x34, 0x37, 0x00, 0x00}, 16);  // serial
    fac(0x13012, (const uint8_t[]){0x7E, 0x05}, 2);  // VID
    fac(0x13014, (const uint8_t[]){0x69, 0x20}, 2);  // PID
    fac(0x13016, (const uint8_t[]){0x01, 0x06, 0x01}, 3);
    fac(0x13019, (const uint8_t[]){0x23, 0x23, 0x23}, 3);  // body colour
    fac(0x1301C, (const uint8_t[]){0xA0, 0xA0, 0xA0}, 3);  // button colour
    fac(0x1301F, (const uint8_t[]){0xE6, 0xE6, 0xE6}, 3);  // highlight
    fac(0x13022, (const uint8_t[]){0x32, 0x32, 0x32}, 3);  // grip
    fac(0x13040, (const uint8_t[]){0x3B, 0xE0, 0xD3, 0x41, 0xC6, 0x60, 0x6A, 0xBC, 0x4D, 0xD7,
                                   0xA2, 0xBB, 0x71, 0x1E, 0xDD, 0x37}, 16);
    fac(0x13060, (const uint8_t[]){0x4C, 0x09, 0x00, 0x00}, 4);
    fac(0x13080, blk, 40);
    fac(0x130A8, (const uint8_t[]){0xB3, 0x67, 0x83, 0x2E, 0x66, 0x5E, 0x3A, 0x06, 0x5F}, 9);  // L stick cal
    fac(0x130C0, blk, 40);
    fac(0x130E8, (const uint8_t[]){0x2C, 0x08, 0x84, 0xD1, 0x65, 0x63, 0x2A, 0x26, 0x62}, 9);  // R stick cal
    fac(0x13140, (const uint8_t[]){0x00, 0xD7, 0xA3, 0xBC, 0x41, 0xD7, 0xA3, 0xBC, 0x41}, 9);

    // Identity block for the EP0 vendor 0x03 request = first 0x25 B of factory (01 00,
    // serial, VID, PID, 01 06 01, colours) then 0xFF fill to 64 (matches the capture).
    memset(ns2_ctrl_identity, 0xFF, sizeof(ns2_ctrl_identity));
    memcpy(ns2_ctrl_identity, factory, 0x25);
}

static void ns2_mem_read(uint32_t addr, uint8_t len, uint8_t *out) {
    for (uint8_t i = 0; i < len; i++) {
        uint32_t a = addr + i;
        if (a >= FACTORY_BASE && a < FACTORY_BASE + FACTORY_SIZE)
            out[i] = factory[a - FACTORY_BASE];
        else if (a == 0x1FA000)
            out[i] = 0x00;  // Bluetooth pairing entry count = 0 (no bonds over USB)
        else
            out[i] = 0xFF;  // uninitialised flash reads as 0xFF
    }
}

//--------------------------------------------------------------------+
// Bluetooth-pairing crypto (the console runs this over USB during first pairing).
// Verified against ndeadly's capture:
//   LTK        = reverse(A1) XOR reverse(B1)      (A1 from 0x15/04, B1 public)
//   B2(onwire) = AES128_ECB(LTK, reverse(A2))     (A2 from 0x15/02 challenge)
//--------------------------------------------------------------------+

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static uint8_t aes_xtime(uint8_t a) { return (uint8_t)((a & 0x80) ? ((a << 1) ^ 0x1b) : (a << 1)); }

static void aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t rk[176];
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t tmp = t[0];
            t[0] = aes_sbox[t[1]] ^ rcon;
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
            rcon = aes_xtime(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int j = 0; j < 16; j++) s[j] ^= rk[j];
    for (int round = 1; round <= 10; round++) {
        for (int j = 0; j < 16; j++) s[j] = aes_sbox[s[j]];
        uint8_t t;
        t = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13];  s[13] = t;   // row 1 <<1
        t = s[2];  s[2] = s[10]; s[10] = t;  t = s[6]; s[6] = s[14]; s[14] = t;  // row 2 <<2
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;     // row 3 <<3
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + 4 * c;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = aes_xtime(a0) ^ (aes_xtime(a1) ^ a1) ^ a2 ^ a3;
                col[1] = a0 ^ aes_xtime(a1) ^ (aes_xtime(a2) ^ a2) ^ a3;
                col[2] = a0 ^ a1 ^ aes_xtime(a2) ^ (aes_xtime(a3) ^ a3);
                col[3] = (aes_xtime(a0) ^ a0) ^ a1 ^ a2 ^ aes_xtime(a3);
            }
        }
        for (int j = 0; j < 16; j++) s[j] ^= rk[round * 16 + j];
    }
    memcpy(out, s, 16);
}

// Public device key B1 (constant), used to derive the LTK with the console's A1.
static const uint8_t ns2_device_key_b1[16] = {
    0x5C, 0xF6, 0xEE, 0x79, 0x2C, 0xDF, 0x05, 0xE1,
    0xBA, 0x2B, 0x63, 0x25, 0xC4, 0x1A, 0x5F, 0x10};
static uint8_t ns2_ltk[16];

static void ns2_rev16(const uint8_t *in, uint8_t *out) {
    for (int i = 0; i < 16; i++) out[i] = in[15 - i];
}

// 0x15/04: LTK = reverse(A1) XOR reverse(B1).
static void ns2_pair_set_ltk(const uint8_t *a1_wire) {
    uint8_t a1r[16], b1r[16];
    ns2_rev16(a1_wire, a1r);
    ns2_rev16(ns2_device_key_b1, b1r);
    for (int i = 0; i < 16; i++) ns2_ltk[i] = a1r[i] ^ b1r[i];
}

// 0x15/02: B2(wire) = AES128_ECB(LTK, reverse(A2)).
static void ns2_pair_challenge(const uint8_t *a2_wire, uint8_t *b2_wire) {
    uint8_t a2r[16];
    ns2_rev16(a2_wire, a2r);
    aes128_encrypt(ns2_ltk, a2r, b2_wire);
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

// Diagnostic (NS2_DIAG): how far the host got, blinked on the LED by
// pico_switch_platform.c. 0 none · 1 device desc read · 2 config desc read ·
// 3 configured (SET_CONFIGURATION) · 4 first vendor cmd · 5 pairing challenge
// (0x15/02) · 6 pairing finalised (0x15/03) · 7 report selected (0x03/0A).
volatile uint8_t g_ns2_stage = 0;

static void vend_send(const uint8_t *r, uint16_t len) {
    tud_vendor_write(r, len);
    tud_vendor_write_flush();
}

// Response header: echo cmd, dir=0x01, echo transport, echo subcmd, ACK 00 f8.
static void ns2_dispatch(const uint8_t *c, uint32_t n) {
    if (n < 8) return;
    uint8_t id = c[0], transport = c[2], sub = c[3];

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
            } else if (sub == 0x02) {  // confirm LTK: B2(wire) = AES128(LTK, rev(A2))
                d[0] = 0x01;
                ns2_pair_challenge(&c[9], &d[1]);  // A2 = c[9..24]
                dl = 17;
            } else if (sub == 0x03) {  // finalise
                d[0] = 0x01; dl = 1;
            } else if (sub == 0x04) {  // exchange keys: derive LTK from A1, return B1
                ns2_pair_set_ltk(&c[9]);  // A1 = c[9..24]
                d[0] = 0x01;
                memcpy(&d[1], ns2_device_key_b1, 16);
                dl = 17;
            }
            break;
        case 0x09:  // player LEDs -> ACK only
            dl = 0;
            break;
        case 0x0C:  // feature select
            if (sub == 0x01) {  // get feature info
                uint8_t f = c[8];
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
                d[4] = c[12];
                dl = 40;
            } else {  // set/clear/enable/disable mask
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
                ns2_mem_read(addr, len, &d[8]);
                dl = 8 + len;
            } else if (sub == 0x01) {  // read 0x40 block
                d[0] = 0x40;
                d[4] = c[12]; d[5] = c[13]; d[6] = c[14]; d[7] = c[15];
                ns2_mem_read(addr, 0x40, &d[8]);
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
            memcpy(d, (const uint8_t[]){0x01, 0x01, 0x05, 0x02, 0x0C, 0x00, 0x00, 0x00,
                                        0xFF, 0xFF, 0xFF, 0xFF}, 12);
            dl = 12;
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
            else dl = 0;
            break;
        case 0x18:
            if (sub == 0x01) { memcpy(d, (const uint8_t[]){0, 0, 0x40, 0xF0, 0, 0, 0x60, 0}, 8); dl = 8; }
            else if (sub == 0x03) { d[0] = c[8]; dl = 1; }
            else dl = 0;
            break;
        default:  // 0x06 shutdown, 0x0A vibration, and anything else -> bare ACK
            dl = 0;
            break;
    }
    vend_send(r, (uint16_t)(8 + dl));
}

//--------------------------------------------------------------------+
// Input report 0x09 (streamed on the HID IN endpoint)
//--------------------------------------------------------------------+

// Translate the shared Switch-1-layout input (report.c, published by bluepad32
// on core1) into the Switch 2 Pro Controller report 0x09 layout.
static void ns2_build_report(uint8_t *p) {
    static uint8_t counter = 0;
    switch_pro_input_t in;
    get_global_gamepad_input(0, &in);  // NS2 milestone: single controller (slot 0)

    memset(p, 0, 63);
    p[0x00] = counter++;
    p[0x01] = 0x25;  // power: external power + battery full

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
    // Switch 2 extra buttons (in.extra): C / GL / GR. From the joypad-os stack these
    // come from L4/R4 (Elite paddles / Edge back buttons) or a native Pro Controller 2;
    // 0 otherwise (e.g. the bluepad32 build, which doesn't expose them).
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
    // 0x0C NFC, 0x0D headset.

    // Motion (IMU): length byte at 0x0E, data at 0x0F (enabled by feature bit 2). The
    // packing is NOT officially documented (ndeadly: "unknown packed format", observed
    // lengths {0,30,40}). This is a HYPOTHESIS following the Nintendo pattern — 3 samples
    // per report of accel XYZ + gyro XYZ (int16 LE), using the same pre-scaled values
    // report 0x05 emits (which works on PC). Pending console verification; only emitted
    // for controllers that actually report motion (else length stays 0 = IMU off).
    if (in.has_motion) {
        static uint32_t mtime = 0;
        p[0x0E] = 40;
        // Hypothesis 2: [timestamp u32 LE @0x0F][3 samples @0x13, each accel XYZ + gyro
        // XYZ int16 LE]. 4 + 3*12 = 40 (the tidy read of the observed length). TommyWabg
        // confirms accel-then-gyro int16 single-sample; the 3-sample batching is the
        // Nintendo convention. Still a guess — the real format is undocumented and the
        // reference captures are link-encrypted (undecryptable).
        mtime += 3;
        memcpy(&p[0x0F], &mtime, 4);
        for (int s = 0; s < 3; s++) {
            uint8_t *m = &p[0x13 + s * 12];
            memcpy(&m[0], in.accel, 6);  // accel X/Y/Z int16 LE
            memcpy(&m[6], in.gyro, 6);   // gyro  X/Y/Z int16 LE
        }
    }
}

// Report 0x05 (common format): 4-byte buttons + DOCUMENTED accel/gyro block.
// Used by the PC / Steam "Switch Pro Controller" profile (see usb-spec / hid_reports).
static void ns2_build_report_05(uint8_t *p) {
    static uint32_t counter = 0;
    switch_pro_input_t in;
    get_global_gamepad_input(0, &in);

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

    memcpy(&p[0x0A], in.left_stick, 3);
    memcpy(&p[0x0D], in.right_stick, 3);

    p[0x1F] = 0xA0;  // battery voltage ~4000 mV (0x0FA0 LE)
    p[0x20] = 0x0F;
    p[0x21] = 0x20;  // charge state
    p[0x29] = 0x01;  // always 0x01

    // Motion block @ 0x2A: timestamp(4) temp(2) accelXYZ(6) gyroXYZ(6) — int16 LE.
    memcpy(&p[0x30], in.accel, 6);
    memcpy(&p[0x36], in.gyro, 6);
}

//--------------------------------------------------------------------+
// Public entry points
//--------------------------------------------------------------------+

// Host completed SET_CONFIGURATION (device configured) — diagnostic stage 3.
void tud_mount_cb(void) {
    if (g_ns2_stage < 3) g_ns2_stage = 3;
}

void ns2_init(void) {
    ns2_factory_init();
    ns2_report_id = 0x09;
    ns2_streaming = false;
}

// Decode one HD-rumble motor's peak amplitude (0..1023) from its 5-byte packed LRA field.
// 40-bit little-endian: freq_0[0:10] | amp_0[10:20] | freq_1[20:30] | amp_1[30:40]
// (format from ndeadly's switch2_input_viewer.py send_vibration). We drive rumble off the
// AMPLITUDE fields only — the old peak-of-all-bytes read the frequency fields, which are
// non-zero at rest and produced a constant idle buzz on HD-rumble pads (Pro Controller 2).
static uint16_t ns2_rumble_motor_amp(const uint8_t *p) {
    uint64_t packed = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
                      ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32);
    uint16_t amp0 = (packed >> 10) & 0x3FF;
    uint16_t amp1 = (packed >> 30) & 0x3FF;
    return amp0 > amp1 ? amp0 : amp1;
}

void ns2_hid_out_report(const uint8_t *buf, uint16_t len) {
    // Rumble output report 0x02: [id][16B left LRA][16B right LRA][9B reserved]; each motor
    // block = [0x50|counter][5B packed freq/amp][zeros]. Take the peak amplitude of both
    // motors, scale 10-bit -> 8-bit, and publish on the seam (feedback bridge -> the pad).
    if (!buf || len < 7 || buf[0] != 0x02) return;
    uint16_t amp = ns2_rumble_motor_amp(&buf[2]);     // left packed = bytes 2..6
    if (len >= 23) {
        uint16_t r = ns2_rumble_motor_amp(&buf[18]);  // right packed = bytes 18..22
        if (r > amp) amp = r;
    }
    report_set_rumble(0, (uint8_t)(amp >> 2));  // 0..1023 -> 0..255
}

void ns2_task(void) {
    if (tud_vendor_available()) {
        uint8_t cmd[64];
        uint32_t n = tud_vendor_read(cmd, sizeof(cmd));
        ns2_dispatch(cmd, n);
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

const uint16_t *ns2_ms_os_string_descriptor(void) {
    return g_usb_config_mode ? NULL : ns2_ms_os_str;
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
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
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
            return tud_control_xfer(rhport, request, ns2_ctrl_identity, len);
        }
        case 0x02: {  // firmware/version info (16 B)
            if (g_ns2_stage < 4) g_ns2_stage = 4;
            uint16_t len = request->wLength < sizeof(ns2_ctrl_info)
                               ? request->wLength : (uint16_t)sizeof(ns2_ctrl_info);
            return tud_control_xfer(rhport, request, (void *)ns2_ctrl_info, len);
        }
        case 0x04:  // OUT, no data -> ACK
            if (g_ns2_stage < 4) g_ns2_stage = 4;
            return tud_control_status(rhport, request);
    }
    return false;
}

#ifdef NS2_AUDIO
//--------------------------------------------------------------------+
// USB Audio stub driver — lets the PC2's 3 audio interfaces (IF2-4) enumerate
// for fidelity without implementing USB audio. Alt-0 has no endpoints, so there
// is nothing to service; audio-class control + alt-setting requests are stalled.
//--------------------------------------------------------------------+

static uint16_t audio_stub_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc,
                                uint16_t max_len) {
    (void)rhport;
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
    return consumed;
}

static bool audio_stub_control(uint8_t rhport, uint8_t stage,
                               tusb_control_request_t const *request) {
    (void)rhport;
    (void)stage;
    (void)request;
    return false;  // stall audio control / alt-setting (unused during detection)
}

static bool audio_stub_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                            uint32_t xferred_bytes) {
    (void)rhport;
    (void)ep_addr;
    (void)result;
    (void)xferred_bytes;
    return true;  // no endpoints opened; not expected to be called
}

static void audio_stub_init(void) {}
static void audio_stub_reset(uint8_t rhport) { (void)rhport; }

static const usbd_class_driver_t audio_stub_driver = {
    .init = audio_stub_init,
    .reset = audio_stub_reset,
    .open = audio_stub_open,
    .control_xfer_cb = audio_stub_control,
    .xfer_cb = audio_stub_xfer,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &audio_stub_driver;
}
#endif  // NS2_AUDIO

#endif  // NS2_PRO
