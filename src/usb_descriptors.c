/*
 * USB descriptors + TinyUSB HID callbacks for the emulated Nintendo Switch
 * Pro Controller (VID 057E / PID 2009).
 *
 * Descriptor byte arrays follow the genuine Pro Controller (values from
 * dekuNukem's reverse-engineering notes via the bmelanman/retro-pico-switch
 * reference, MIT). The interface exposes both an interrupt IN endpoint (input
 * reports) and an interrupt OUT endpoint (console commands / handshake).
 */

#include <string.h>

#include "tusb.h"

#include "hid_out_normalize.h"
#include "switch_pro.h"
#include "usb.h"  // g_usb_config_mode, g_usb_personality

#ifdef NS2_PRO
#include "switch_pro2.h"
#include "switch_gc.h"
#endif

//--------------------------------------------------------------------+
// Device + string descriptors
//--------------------------------------------------------------------+

#ifndef NS2_PRO
static const uint8_t switch_pro_device_descriptor[] = {
    0x12,        // bLength
    0x01,        // bDescriptorType (Device)
    0x00, 0x02,  // bcdUSB 2.00
    0x00,        // bDeviceClass (use interface descriptors)
    0x00,        // bDeviceSubClass
    0x00,        // bDeviceProtocol
    0x40,        // bMaxPacketSize0 64
    0x7E, 0x05,  // idVendor 0x057E (Nintendo)
    0x09, 0x20,  // idProduct 0x2009 (Pro Controller)
    0x10, 0x02,  // bcdDevice 2.10
    0x01,        // iManufacturer
    0x02,        // iProduct
    0x03,        // iSerialNumber
    0x01,        // bNumConfigurations
};

static const char *switch_pro_strings[] = {
    (const char[]){0x09, 0x04},  // 0: language id (en-US)
    "Nintendo Co., Ltd.",        // 1: manufacturer
    "Pro Controller",            // 2: product
    "000000000001",              // 3: serial number
};

//--------------------------------------------------------------------+
// HID report descriptor (USB Pro Controller, 203 bytes)
//--------------------------------------------------------------------+

static const uint8_t switch_pro_report_descriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x15, 0x00,        // Logical Minimum (0)
    0x09, 0x04,        // Usage (Joystick)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x30,        //   Report ID (48)
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x0A,        //   Usage Maximum (0x0A)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0A,        //   Report Count (10)
    0x55, 0x00,        //   Unit Exponent (0)
    0x65, 0x00,        //   Unit (None)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x0B,        //   Usage Minimum (0x0B)
    0x29, 0x0E,        //   Usage Maximum (0x0E)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x03,        //   Input (Const)
    0x0B, 0x01, 0x00, 0x01, 0x00,  //   Usage (0x010001)
    0xA1, 0x00,                    //   Collection (Physical)
    0x0B, 0x30, 0x00, 0x01, 0x00,  //     Usage (0x010030) X
    0x0B, 0x31, 0x00, 0x01, 0x00,  //     Usage (0x010031) Y
    0x0B, 0x32, 0x00, 0x01, 0x00,  //     Usage (0x010032) Z
    0x0B, 0x35, 0x00, 0x01, 0x00,  //     Usage (0x010035) Rz
    0x15, 0x00,                    //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65534)
    0x75, 0x10,                    //     Report Size (16)
    0x95, 0x04,                    //     Report Count (4)
    0x81, 0x02,                    //     Input (Data,Var,Abs)
    0xC0,                          //   End Collection
    0x0B, 0x39, 0x00, 0x01, 0x00,  //   Usage (0x010039) Hat switch
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x07,                    //   Logical Maximum (7)
    0x35, 0x00,                    //   Physical Minimum (0)
    0x46, 0x3B, 0x01,              //   Physical Maximum (315)
    0x65, 0x14,                    //   Unit (Eng Rotation: Degrees)
    0x75, 0x04,                    //   Report Size (4)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x05, 0x09,                    //   Usage Page (Button)
    0x19, 0x0F,                    //   Usage Minimum (0x0F)
    0x29, 0x12,                    //   Usage Maximum (0x12)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x04,                    //   Report Count (4)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x34,                    //   Report Count (52)
    0x81, 0x03,                    //   Input (Const)
    0x06, 0x00, 0xFF,              //   Usage Page (Vendor Defined 0xFF00)
    0x85, 0x21,                    //   Report ID (33)
    0x09, 0x01,                    //   Usage (0x01)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x81, 0x03,                    //   Input (Const)
    0x85, 0x81,                    //   Report ID (129)
    0x09, 0x02,                    //   Usage (0x02)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x81, 0x03,                    //   Input (Const)
    0x85, 0x01,                    //   Report ID (1)
    0x09, 0x03,                    //   Usage (0x03)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x91, 0x83,                    //   Output (Const,Var,Abs,Volatile)
    0x85, 0x10,                    //   Report ID (16)
    0x09, 0x04,                    //   Usage (0x04)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x91, 0x83,                    //   Output (Const,Var,Abs,Volatile)
    0x85, 0x80,                    //   Report ID (128)
    0x09, 0x05,                    //   Usage (0x05)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x91, 0x83,                    //   Output (Const,Var,Abs,Volatile)
    0x85, 0x82,                    //   Report ID (130)
    0x09, 0x06,                    //   Usage (0x06)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x91, 0x83,                    //   Output (Const,Var,Abs,Volatile)
    0xC0,                          // End Collection
};

// The genuine Pro Controller USB HID report descriptor is 203 bytes; a wrong
// length here would make the console reject enumeration.
_Static_assert(sizeof(switch_pro_report_descriptor) == 203,
               "Pro Controller HID report descriptor must be 203 bytes");

//--------------------------------------------------------------------+
// Configuration descriptor: SWITCH_PRO_MAX_CONTROLLERS HID interfaces, each with
// an interrupt IN + OUT endpoint pair (IN 0x81.., OUT 0x01..). One interface =
// one Pro Controller = one player on the console.
//--------------------------------------------------------------------+

// One HID interface block (interface + HID + IN + OUT = 32 bytes), matching the
// genuine Pro Controller layout, parameterized by interface number and endpoints.
#define SWITCH_PRO_HID_INTERFACE(ifnum, ep_in, ep_out)                       \
    0x09, 0x04, (ifnum), 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, /* Interface */ \
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,                /* HID */       \
    (uint8_t)(sizeof(switch_pro_report_descriptor) & 0xFF),                  \
    (uint8_t)((sizeof(switch_pro_report_descriptor) >> 8) & 0xFF),           \
    0x07, 0x05, (ep_in), 0x03, 0x40, 0x00, 0x04,   /* EP IN,  bInterval 4 = 250Hz (genuine PC2) */ \
    0x07, 0x05, (ep_out), 0x03, 0x40, 0x00, 0x04   /* EP OUT, bInterval 4 */

#define SWITCH_PRO_IF_LEN 32
#define SWITCH_PRO_CONFIG_LEN (9 + SWITCH_PRO_MAX_CONTROLLERS * SWITCH_PRO_IF_LEN)

static const uint8_t switch_pro_config_descriptor[] = {
    0x09,                                            // bLength
    0x02,                                            // bDescriptorType (Configuration)
    (uint8_t)(SWITCH_PRO_CONFIG_LEN & 0xFF),         // wTotalLength lo
    (uint8_t)((SWITCH_PRO_CONFIG_LEN >> 8) & 0xFF),  // wTotalLength hi
    SWITCH_PRO_MAX_CONTROLLERS,                       // bNumInterfaces
    0x01,                                            // bConfigurationValue
    0x00,                                            // iConfiguration
    0xA0,                                            // bmAttributes (bus powered, remote wakeup)
    0xFA,                                            // bMaxPower 500mA

    SWITCH_PRO_HID_INTERFACE(0, 0x81, 0x01),
#if SWITCH_PRO_MAX_CONTROLLERS > 1
    SWITCH_PRO_HID_INTERFACE(1, 0x82, 0x02),
#endif
#if SWITCH_PRO_MAX_CONTROLLERS > 2
    SWITCH_PRO_HID_INTERFACE(2, 0x83, 0x03),
#endif
#if SWITCH_PRO_MAX_CONTROLLERS > 3
    SWITCH_PRO_HID_INTERFACE(3, 0x84, 0x04),
#endif
};

_Static_assert(SWITCH_PRO_MAX_CONTROLLERS >= 1 && SWITCH_PRO_MAX_CONTROLLERS <= 4,
               "SWITCH_PRO_MAX_CONTROLLERS must be 1..4");
_Static_assert(sizeof(switch_pro_config_descriptor) == SWITCH_PRO_CONFIG_LEN,
               "configuration descriptor size must match wTotalLength");
#endif  // !NS2_PRO

//--------------------------------------------------------------------+
// Configuration-mode descriptors (USB CDC serial)
//--------------------------------------------------------------------+

#define CONFIG_USB_VID 0xCAFE
#define CONFIG_USB_PID 0x4012

static const uint8_t cdc_device_descriptor[] = {
    0x12,        // bLength
    0x01,        // bDescriptorType (Device)
    0x00, 0x02,  // bcdUSB 2.00
    0xEF,        // bDeviceClass (Misc)
    0x02,        // bDeviceSubClass (common)
    0x01,        // bDeviceProtocol (IAD)
    0x40,        // bMaxPacketSize0 64
    (uint8_t)(CONFIG_USB_VID & 0xFF), (uint8_t)(CONFIG_USB_VID >> 8),
    (uint8_t)(CONFIG_USB_PID & 0xFF), (uint8_t)(CONFIG_USB_PID >> 8),
    0x00, 0x01,  // bcdDevice 1.00
    0x01,        // iManufacturer
    0x02,        // iProduct
    0x03,        // iSerialNumber
    0x01,        // bNumConfigurations
};

// Config mode is a composite device: CDC serial (the command link) + a
// read-only Mass Storage disk (carries CONFIG.HTM so the dongle is self-contained).
enum { CDC_ITF_NOTIF = 0, CDC_ITF_DATA, MSC_ITF, CONFIG_ITF_COUNT };
#define CDC_EP_NOTIF 0x81
#define CDC_EP_OUT 0x02
#define CDC_EP_IN 0x82
#define MSC_EP_OUT 0x03
#define MSC_EP_IN 0x83
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t cdc_config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, CONFIG_ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(CDC_ITF_NOTIF, 0, CDC_EP_NOTIF, 8, CDC_EP_OUT, CDC_EP_IN, 64),
    TUD_MSC_DESCRIPTOR(MSC_ITF, 0, MSC_EP_OUT, MSC_EP_IN, 64),
};

static const char *config_strings[] = {
    (const char[]){0x09, 0x04},  // 0: language id (en-US)
    "518",                       // 1: manufacturer
    "PicoSwitch Config",         // 2: product
    "000000000001",              // 3: serial number
};

//--------------------------------------------------------------------+
// TinyUSB callbacks (return HID or CDC descriptors per the current mode)
//--------------------------------------------------------------------+

uint8_t const *tud_descriptor_device_cb(void) {
#ifdef NS2_PRO
    switch (g_usb_personality) {
        case USB_PERSONALITY_CDC_CONFIG:
            return cdc_device_descriptor;
        case USB_PERSONALITY_NSO_GAMECUBE:
            if (g_gc_stage < 1) g_gc_stage = 1;  // diag: host read our device descriptor
            return switch_gc_device_descriptor();
        case USB_PERSONALITY_SWITCH2_PRO2:
        case USB_PERSONALITY_JOYCON2:  // reserved/unreachable -- fall back to the real default
        default:
            if (g_ns2_stage < 1) g_ns2_stage = 1;  // diag: host read our device descriptor
            return ns2_device_descriptor();
    }
#else
    return g_usb_config_mode ? cdc_device_descriptor : switch_pro_device_descriptor;
#endif
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
#ifdef NS2_PRO
    switch (g_usb_personality) {
        case USB_PERSONALITY_CDC_CONFIG:
            return cdc_config_descriptor;
        case USB_PERSONALITY_NSO_GAMECUBE:
            if (g_gc_stage < 2) g_gc_stage = 2;  // diag: host read our config descriptor
            return switch_gc_config_descriptor();
        case USB_PERSONALITY_SWITCH2_PRO2:
        case USB_PERSONALITY_JOYCON2:
        default:
            if (g_ns2_stage < 2) g_ns2_stage = 2;  // diag: host read our config descriptor
            return ns2_config_descriptor();
    }
#else
    return g_usb_config_mode ? cdc_config_descriptor : switch_pro_config_descriptor;
#endif
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
#ifdef NS2_PRO
    if (g_usb_personality == USB_PERSONALITY_NSO_GAMECUBE)
        return switch_gc_hid_report_descriptor();
    return ns2_hid_report_descriptor();
#else
    return switch_pro_report_descriptor;
#endif
}

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

#ifdef NS2_PRO
    // MS OS 1.0: Windows probes string index 0xEE to discover the WinUSB binding.
    // Personality-scoped -- each personality's own vendor interface needs its
    // own compat-ID binding (see switch_gc.c's comment for why GameCube gained
    // this in Stage B: real hardware testing found Windows reports error code
    // 28 without it, matching how the genuine controller family apparently
    // ships this exact mechanism -- Pro2 already relied on it for its own
    // vendor interface).
    if (index == 0xEE) {
        const uint16_t *ms = NULL;
        if (g_usb_personality == USB_PERSONALITY_SWITCH2_PRO2)
            ms = ns2_ms_os_string_descriptor();
        else if (g_usb_personality == USB_PERSONALITY_NSO_GAMECUBE)
            ms = switch_gc_ms_os_string_descriptor();
        if (ms) return ms;
    }
#endif

    const char **strings;
    size_t count;
#ifdef NS2_PRO
    switch (g_usb_personality) {
        case USB_PERSONALITY_CDC_CONFIG:
            strings = config_strings;
            count = sizeof(config_strings) / sizeof(config_strings[0]);
            break;
        case USB_PERSONALITY_NSO_GAMECUBE:
            strings = switch_gc_string_table(&count);
            break;
        case USB_PERSONALITY_SWITCH2_PRO2:
        case USB_PERSONALITY_JOYCON2:
        default:
            strings = ns2_string_table(&count);
            break;
    }
#else
    if (g_usb_config_mode) {
        strings = config_strings;
        count = sizeof(config_strings) / sizeof(config_strings[0]);
    } else {
        strings = switch_pro_strings;
        count = sizeof(switch_pro_strings) / sizeof(switch_pro_strings[0]);
    }
#endif

    if (index == 0) {
        memcpy(&_desc_str[1], strings[0], 2);
        chr_count = 1;
    } else {
        // A table entry may be NULL for a string index this project has not
        // captured real text for yet (see switch_gc.c's iConfiguration/
        // iInterface entries) -- stall that specific GET_DESCRIPTOR(STRING)
        // request rather than fabricate placeholder text or dereference NULL.
        if (index >= count || strings[index] == NULL)
            return NULL;
        const char *str = strings[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31)
            chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

// GET_REPORT control request: the Switch drives input via the interrupt IN
// endpoint, so stall control GET_REPORT.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

// SET_REPORT / OUT endpoint: console commands and handshake arrive here.
//
// Phase 4 fix (2026-07-13): normalize TinyUSB's two divergent OUT-report transports (interrupt
// OUT vs control SET_REPORT) into one consistent (id, data, data_len) triple before handing off
// to the personality handlers -- see hid_out_normalize.h/.c (pure, host-tested,
// tools/test_hid_out_normalize.c) for the audited TinyUSB 2.2.0 contract this implements. The
// previous version of this dispatcher ignored `report_id` entirely and always treated buffer[0]
// as the ID -- correct by coincidence for the interrupt-OUT path this device actually uses in
// practice, but wrong for control SET_REPORT (would misread the first DATA byte as the ID, or
// misalign every subsequent offset by one).
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)report_type;
#ifdef NS2_PRO
    (void)instance;
    hid_out_normalized_t n = hid_out_normalize(report_id, buffer, bufsize);
    if (g_usb_personality == USB_PERSONALITY_NSO_GAMECUBE)
        switch_gc_hid_out_report(n.report_id, n.data, n.data_len);
    else
        ns2_hid_out_report(n.report_id, n.data, n.data_len);
#else
    (void)report_id;
    switch_pro_receive(instance, buffer, bufsize);  // Switch 1 path, unaudited/unchanged this pass
#endif
}

//--------------------------------------------------------------------+
// Centralized dispatch for the TinyUSB callbacks that can only have one
// definition in the link (EP0 vendor control, SET_CONFIGURATION/mount, and
// the app class-driver hook) -- see docs/switch2-gc/usb-personality.md
// "TinyUSB dispatch and resource constraints". Each personality owns its own
// plainly-named handler (ns2_*/switch_gc_*); nothing personality-specific
// lives in this file beyond the routing itself.
//--------------------------------------------------------------------+

#ifdef NS2_PRO
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    switch (g_usb_personality) {
        case USB_PERSONALITY_NSO_GAMECUBE:
            return switch_gc_vendor_control_xfer(rhport, stage, request);
        case USB_PERSONALITY_SWITCH2_PRO2:
            return ns2_vendor_control_xfer(rhport, stage, request);
        case USB_PERSONALITY_CDC_CONFIG:
        case USB_PERSONALITY_JOYCON2:
        default:
            return false;
    }
}

void tud_mount_cb(void) {
    switch (g_usb_personality) {
        case USB_PERSONALITY_NSO_GAMECUBE:
            switch_gc_mount();
            break;
        case USB_PERSONALITY_SWITCH2_PRO2:
            ns2_mount();
            break;
        case USB_PERSONALITY_CDC_CONFIG:
        case USB_PERSONALITY_JOYCON2:
        default:
            break;
    }
}
#endif  // NS2_PRO
