#include "bt/bthid/devices/generic/bthid_mouse_report.h"

#include <limits.h>
#include <string.h>

#include "usb/usbh/hid/devices/generic/hid_parser.h"

static bool is_relative_axis(const HID_ReportItem_t *item, uint16_t usage) {
    return item->ItemType == HID_REPORT_ITEM_In &&
           item->Attributes.Usage.Page == 0x01 &&
           item->Attributes.Usage.Usage == usage &&
           (item->ItemFlags & HID_IOF_RELATIVE) != 0 &&
           item->Attributes.BitSize > 0 && item->Attributes.BitSize <= 16;
}

static bthid_mouse_field_t field_from_item(const HID_ReportItem_t *item,
                                            bool using_report_ids) {
    bthid_mouse_field_t field = {
        .bit_offset = (uint16_t)(item->BitOffset + (using_report_ids ? 8 : 0)),
        .bit_size = item->Attributes.BitSize,
        .present = true,
    };
    return field;
}

bool bthid_mouse_parse_descriptor(const uint8_t *desc, uint16_t desc_len,
                                  bthid_mouse_report_map_t *out) {
    if (!desc || desc_len == 0 || !out) return false;

    HID_ReportInfo_t *info = NULL;
    if (USB_ProcessHIDReport(0, 0, desc, desc_len, &info) != HID_PARSE_Successful || !info)
        return false;

    bool found_x = false;
    bool found_y = false;
    uint8_t mouse_report_id = 0;

    // Pick a single report containing both relative X and relative Y. Absolute
    // gamepad sticks therefore cannot be mistaken for a mouse.
    for (HID_ReportItem_t *x = info->FirstReportItem; x && !found_y; x = x->Next) {
        if (!is_relative_axis(x, 0x30)) continue;
        for (HID_ReportItem_t *y = info->FirstReportItem; y; y = y->Next) {
            if (y->ReportID == x->ReportID && is_relative_axis(y, 0x31)) {
                mouse_report_id = x->ReportID;
                found_x = found_y = true;
                break;
            }
        }
    }

    if (!found_x || !found_y) {
        USB_FreeReportInfo(info);
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->report_id = mouse_report_id;
    out->using_report_ids = info->UsingReportIDs;

    for (HID_ReportItem_t *item = info->FirstReportItem; item; item = item->Next) {
        if (item->ItemType != HID_REPORT_ITEM_In || item->ReportID != mouse_report_id)
            continue;

        if (is_relative_axis(item, 0x30)) {
            out->x = field_from_item(item, info->UsingReportIDs);
        } else if (is_relative_axis(item, 0x31)) {
            out->y = field_from_item(item, info->UsingReportIDs);
        } else if (is_relative_axis(item, 0x38)) {
            out->wheel = field_from_item(item, info->UsingReportIDs);
        } else if (item->Attributes.Usage.Page == 0x09) {
            uint16_t usage = item->Attributes.Usage.Usage;
            if (usage >= 1 && usage <= BTHID_MOUSE_MAX_BUTTONS &&
                item->Attributes.BitSize == 1) {
                out->buttons[usage - 1] = field_from_item(item, info->UsingReportIDs);
                if (usage > out->button_count) out->button_count = (uint8_t)usage;
            }
        }
    }

    USB_FreeReportInfo(info);
    return out->x.present && out->y.present;
}

static bool extract_bits(const uint8_t *data, uint16_t len,
                         const bthid_mouse_field_t *field, uint32_t *out) {
    if (!data || !field || !field->present || !out || field->bit_size == 0 ||
        field->bit_size > 16) return false;

    uint32_t value = 0;
    for (uint8_t i = 0; i < field->bit_size; ++i) {
        uint32_t bit = (uint32_t)field->bit_offset + i;
        if ((bit >> 3) >= len) return false;
        if (data[bit >> 3] & (1u << (bit & 7))) value |= 1u << i;
    }
    *out = value;
    return true;
}

static int16_t extract_signed(const uint8_t *data, uint16_t len,
                              const bthid_mouse_field_t *field) {
    uint32_t raw = 0;
    if (!extract_bits(data, len, field, &raw)) return 0;
    const uint32_t sign = 1u << (field->bit_size - 1);
    if (raw & sign) raw |= ~((1u << field->bit_size) - 1u);
    int32_t value = (int32_t)raw;
    if (value < INT16_MIN) value = INT16_MIN;
    if (value > INT16_MAX) value = INT16_MAX;
    return (int16_t)value;
}

bool bthid_mouse_decode_report(const bthid_mouse_report_map_t *map,
                               const uint8_t *data, uint16_t len,
                               bthid_mouse_report_t *out) {
    if (!map || !data || !out || !map->x.present || !map->y.present) return false;
    if (map->using_report_ids && (len == 0 || data[0] != map->report_id)) return false;

    memset(out, 0, sizeof(*out));
    out->delta_x = extract_signed(data, len, &map->x);
    out->delta_y = extract_signed(data, len, &map->y);
    if (map->wheel.present) {
        int16_t wheel = extract_signed(data, len, &map->wheel);
        if (wheel < INT8_MIN) wheel = INT8_MIN;
        if (wheel > INT8_MAX) wheel = INT8_MAX;
        out->wheel = (int8_t)wheel;
    }

    for (uint8_t i = 0; i < BTHID_MOUSE_MAX_BUTTONS; ++i) {
        uint32_t pressed = 0;
        if (extract_bits(data, len, &map->buttons[i], &pressed) && pressed)
            out->buttons |= (uint16_t)(1u << i);
    }
    return true;
}
