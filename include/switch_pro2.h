/*
 * Switch 2 Pro Controller (VID 057E / PID 2069) USB emulation.
 * Compiled/active only when the firmware is built with -DNS2_PRO.
 *
 * Protocol + byte-exact details: docs/switch2/usb-spec.md (verified against
 * ndeadly's USB capture). USB layout (Option B, no audio):
 *   IF0 HID    - input report 0x09 on EP 0x81 IN, rumble report 0x02 on EP 0x01 OUT
 *   IF1 Vendor - 8-byte command protocol on EP 0x02 bulk OUT / 0x82 bulk IN
 */
#ifndef SWITCH_PRO2_H
#define SWITCH_PRO2_H

#include <stddef.h>
#include <stdint.h>

// Descriptor accessors used by usb_descriptors.c when NS2_PRO is defined.
const uint8_t *ns2_device_descriptor(void);
const uint8_t *ns2_config_descriptor(void);
const uint8_t *ns2_hid_report_descriptor(void);  // 97 bytes (length declared in config desc)
const char   **ns2_string_table(size_t *count);
const uint16_t *ns2_ms_os_string_descriptor(void);  // MS OS 1.0 OS string (index 0xEE)
extern volatile uint8_t g_ns2_stage;  // NS2_DIAG: enumeration + handshake progress (0-7)

// core0 lifecycle: init once, then service every loop iteration in normal mode.
void ns2_init(void);
void ns2_task(void);

// Rumble output report 0x02 delivered on the HID OUT endpoint.
void ns2_hid_out_report(const uint8_t *buf, uint16_t len);

#endif  // SWITCH_PRO2_H
