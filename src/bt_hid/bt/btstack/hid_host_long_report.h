#ifndef NS2_HID_HOST_LONG_REPORT_H
#define NS2_HID_HOST_LONG_REPORT_H

#include <stdint.h>

// Send an output report through BTstack's HID Host, refusing the send while one
// is already pending rather than overwriting it.
//
// Upstream hid_host_send_report() accepts any state from
// HID_HOST_CONNECTION_ESTABLISHED up to (but excluding)
// HID_HOST_W4_INTERRUPT_CONNECTION_DISCONNECTED, and HID_HOST_W2_SEND_REPORT is
// inside that range: a send arriving while an earlier report is still queued
// replaces the buffer L2CAP has not read yet. The DualSense audio path retries
// on every 2 ms service pass, so it hits that window routinely.
//
// Returns ERROR_CODE_COMMAND_DISALLOWED when a report is already pending; the
// caller is expected to retry. Available only in builds that substitute
// cmake/btstack_hid_host_long_report.c for upstream hid_host.c.
//
// The name is historical: it also carried the >255-byte report length that
// BTstack 1.6.2's uint8_t API could not express. BTstack 1.8.2 takes a uint16_t
// length natively, so the length is no longer the reason this exists.
uint8_t ns2_hid_host_send_long_report(uint16_t hid_cid, uint16_t report_id,
                                      const uint8_t *report,
                                      uint16_t report_len);

#endif
