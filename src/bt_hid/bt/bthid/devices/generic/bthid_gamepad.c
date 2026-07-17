// bthid_gamepad.c - Generic Bluetooth Gamepad Driver
// Handles basic HID gamepads over Bluetooth
// This is a fallback driver for gamepads without a specific driver
//
// For BLE devices with HID descriptors, uses the same HID report parser
// as the USB path (hid_parser.c) to dynamically extract field locations.
// Falls back to hardcoded 6-byte layout for Classic BT devices without descriptors.

#include "bthid_gamepad.h"
#include "bt/bthid/bthid.h"
#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
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

    gp->map.quirk = gamepad_quirks_identify(device->vendor_id, device->product_id,
                                             device->name, gp->map.buttonCnt);
    gp->has_report_map = true;
    printf("[BTHID_GAMEPAD] Parsed: %d btns, X@%d Y@%d Z@%d RZ@%d RX@%d RY@%d hat@%d(min=%d) sim=%d quirk=%s\n",
           btns_count,
           gp->map.xLoc.byteIndex, gp->map.yLoc.byteIndex,
           gp->map.zLoc.byteIndex, gp->map.rzLoc.byteIndex,
           gp->map.rxLoc.byteIndex, gp->map.ryLoc.byteIndex,
           gp->map.hatLoc.byteIndex, gp->map.hat_min, gp->map.has_sim_triggers,
           gp->map.quirk->name);
}

void bthid_gamepad_update_vid(bthid_device_t* device)
{
    bthid_gamepad_data_t* gp = (bthid_gamepad_data_t*)device->driver_data;
    if (!gp || !gp->has_report_map) return;

    gp->map.quirk = gamepad_quirks_identify(device->vendor_id, device->product_id,
                                             device->name, gp->map.buttonCnt);
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
    // Preserve the existing diagnostic JSON contract even though runtime
    // dispatch now uses one resolved profile instead of these booleans.
    bool is_xbox = device->vendor_id == 0x045E ||
                   (device->name[0] && strstr(device->name, "Xbox") != NULL);
    bool is_8bitdo = device->vendor_id == 0x2DC8;
    bool is_elite2 = device->vendor_id == 0x045E &&
                     (device->product_id == 0x0B05 || device->product_id == 0x0B22);
    bool is_ngc_modkit = device->vendor_id == 0x2DC8 && device->product_id == 0x286A;
    bool digital_shoulder_triggers = m->quirk && m->quirk->digital_shoulder_triggers;
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
        m->report_id, m->buttonCnt, is_xbox ? "true" : "false",
        is_8bitdo ? "true" : "false", is_elite2 ? "true" : "false",
        is_ngc_modkit ? "true" : "false",
        m->has_sim_triggers ? "true" : "false", digital_shoulder_triggers ? "true" : "false",
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
    if (map->quirk->digital_shoulder_triggers) {
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

    // The shared engine owns descriptor parsing; the resolved profile owns only
    // the usage-number interpretation for this controller family/model.
    const uint32_t *btn_map = map->quirk->button_map;
    uint8_t btn_map_size = map->quirk->button_map_size;
    if (map->quirk->select_button_map) {
        map->quirk->select_button_map(map->buttonCnt, map->has_sim_triggers,
                                      &btn_map, &btn_map_size);
    }
    if (!btn_map || btn_map_size == 0) {
        // gamepad_quirks_identify() always returns a complete profile. Keep a
        // defensive neutral result if a future incomplete profile violates it.
        btn_map_size = 0;
    }

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
            }
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

    // Reset profile-owned state every report before the optional extractor
    // adds raw-byte controls or native semantic fields.
    gp->event.gc_has_native_layout = false;
    gp->event.gc_native_z = false;
    gp->event.gc_l_detent = false;
    gp->event.gc_r_detent = false;
    if (map->quirk->extract_extra) {
        map->quirk->extract_extra(map, data, len, &gp->event);
    }

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
            gamepad_data[i].map.quirk = gamepad_quirks_identify(
                device->vendor_id, device->product_id, device->name, 0);

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

// The generic driver only forwards hardware-validated output through a resolved profile.
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
        // Preserve the v1.2 behavior gate: name-only Xbox matches parse input,
        // but generic-driver rumble is sent only after Microsoft VID resolves.
        if (device->vendor_id == 0x045E && gp->map.quirk->send_rumble) {
            if (!gp->map.quirk->send_rumble(device->conn_index, left, right)) {
                return;  // Preserve dirty state and cache so failed OFF transitions retry.
            }
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
