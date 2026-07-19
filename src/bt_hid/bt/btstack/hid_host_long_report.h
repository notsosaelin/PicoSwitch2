#ifndef NS2_HID_HOST_LONG_REPORT_H
#define NS2_HID_HOST_LONG_REPORT_H

#include <stdint.h>

// BTstack 1.6.2 stores report_len as uint16_t internally, but its public
// hid_host_send_report() parameter is uint8_t. This compatibility extension
// exposes the existing 16-bit-capable state machine without changing ordinary
// HID Host callers.
uint8_t ns2_hid_host_send_long_report(uint16_t hid_cid, uint16_t report_id,
                                      const uint8_t *report,
                                      uint16_t report_len);

#endif
