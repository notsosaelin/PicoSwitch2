#ifndef ANDROID_CONTROLLER_HID_H
#define ANDROID_CONTROLLER_HID_H

#include <stdint.h>

#define ANDROID_CONTROLLER_REPORT_ID 0x01u
#define ANDROID_CONTROLLER_PAYLOAD_LEN 9u
#define ANDROID_CONTROLLER_WIRE_REPORT_LEN (1u + ANDROID_CONTROLLER_PAYLOAD_LEN)

// Canonical Android Controller Bridge HID descriptor. Android passes the nine
// payload bytes to BluetoothHidDevice.sendReport(); BTstack delivers the report
// ID followed by this payload to PicoSwitch2.
static const uint8_t ANDROID_CONTROLLER_HID_DESCRIPTOR[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, ANDROID_CONTROLLER_REPORT_ID,

    0x09, 0x30,       // X
    0x09, 0x31,       // Y
    0x09, 0x32,       // Z
    0x09, 0x35,       // Rz
    0x09, 0x33,       // Rx
    0x09, 0x34,       // Ry
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xFF, 0x00, // Logical Maximum (255)
    0x75, 0x08,       // Report Size (8)
    0x95, 0x06,       // Report Count (6)
    0x81, 0x02,       // Input (Data, Variable, Absolute)

    0x05, 0x09,       // Usage Page (Button)
    0x19, 0x01,       // Usage Minimum (1)
    0x29, 0x0E,       // Usage Maximum (14)
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x0E,
    0x81, 0x02,
    0x75, 0x01,       // Two padding bits
    0x95, 0x02,
    0x81, 0x03,       // Input (Constant)

    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x39,       // Usage (Hat Switch)
    0x15, 0x00,
    0x25, 0x07,
    0x35, 0x00,
    0x46, 0x3B, 0x01, // Physical Maximum (315 degrees)
    0x65, 0x14,       // Unit (degrees)
    0x75, 0x04,
    0x95, 0x01,
    0x81, 0x42,       // Input (Data, Variable, Absolute, Null State)
    0x75, 0x04,       // Four padding bits
    0x95, 0x01,
    0x81, 0x03,
    0xC0
};

static const uint8_t ANDROID_CONTROLLER_NEUTRAL_REPORT[ANDROID_CONTROLLER_WIRE_REPORT_LEN] = {
    ANDROID_CONTROLLER_REPORT_ID,
    128, 128, 128, 128, // sticks
    0, 0,               // triggers
    0, 0,               // buttons 1..14
    8                    // neutral hat + padding
};

#endif // ANDROID_CONTROLLER_HID_H
