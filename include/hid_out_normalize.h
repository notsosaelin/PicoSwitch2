/*
 * Pure normalization of TinyUSB's two divergent tud_hid_set_report_cb() transports into one
 * consistent (report_id, data, data_len) triple. Zero pico-sdk/TinyUSB dependency,
 * host-compilable and host-tested (see tools/test_hid_out_normalize.c), mirroring
 * usb_mode_cycle.h/.c and switch_gc_encode.h/.c's own pattern for the same reason.
 *
 * See src/usb_descriptors.c's tud_hid_set_report_cb() for the audited TinyUSB 2.2.0 source
 * (lib/tinyusb/src/class/hid/hid_device.c) this implements:
 *   - Interrupt OUT: hidd_xfer_cb() always calls back with report_id==0 and the real report
 *     ID byte still at buffer[0] (bufsize includes it). A zero-length OUT packet (ZLP) also
 *     arrives this way, with bufsize==0.
 *   - Control SET_REPORT (HID_REQ_CONTROL_SET_REPORT, CONTROL_STAGE_ACK): report_id carries
 *     the real ID; a matching leading ID byte is stripped from buffer before the callback.
 */
#ifndef HID_OUT_NORMALIZE_H
#define HID_OUT_NORMALIZE_H

#include <stdint.h>

typedef struct {
    uint8_t report_id;
    const uint8_t *data;  // never includes the report ID
    uint16_t data_len;
} hid_out_normalized_t;

hid_out_normalized_t hid_out_normalize(uint8_t report_id, const uint8_t *buffer, uint16_t bufsize);

#endif  // HID_OUT_NORMALIZE_H
