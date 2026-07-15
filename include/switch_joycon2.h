/*
 * Joy-Con 2 (VID 057E / PID 2067 Left, 2066 Right) USB emulation. Compiled/active only under
 * -DNS2_PRO, mirroring switch_gc.h's own scoping. Backs TWO separate usb_personality_t values,
 * USB_PERSONALITY_JOYCON2_L and _R (include/usb.h) -- deliberately not one personality with a
 * side toggle, and deliberately not a combined/paired identity. Both are EXPERIMENTAL/test
 * personalities for hardware validation, not the recommended full-controller mode -- that
 * remains Pro Controller 2 (USB_PERSONALITY_SWITCH2_PRO2), the default and primary supported
 * personality for using one paired controller as a complete Switch 2 controller.
 *
 * Evidence base: docs/switch2-joycon2/protocol.md (USB device/config/HID Report descriptors all
 * Confirmed byte-exact via live USBPcap capture, cross-validated against
 * ndeadly/switch2_controller_research; wire report field layout Confirmed from that same
 * reference's real decrypted BLE captures), docs/experiments/joycon2-spi-dump-analysis-2026-07-14.md
 * (factory data block, from the project owner's own genuine-unit SPI dumps).
 *
 * Status: Stage B+C -- USB enumeration and input report construction are backed by real,
 * Confirmed evidence. EP0 vendor identity handshake and vendor bulk command dispatch are
 * templated from switch_gc.c's own already-hardware-validated pattern (same command
 * architecture, confirmed shared across the whole controller family by
 * ndeadly's commands.md) but are NOT independently confirmed for Joy-Con 2 against a real
 * console -- same evidence tier GameCube's Stage D started at. Rumble (output report 0x01) is
 * PROVISIONAL, not a resolved byte decode -- see switch_joycon2.c's own comment.
 *
 * L vs R: this module tracks which side to present via an internal joycon2_side_t, set through
 * switch_joycon2_set_side() -- but the CALLER (usb.c's usb_reset_personality_state()) is what
 * decides the value, automatically, as a direct consequence of which of the two separate
 * personalities the BOOTSEL mode-cycle just selected. This is never a user-facing choice inside
 * the module, and there is no config-UI side toggle to wire up -- the two personalities ARE the
 * selection mechanism. No "merged"/paired identity exists or is planned (project owner decision,
 * 2026-07-14): a genuine wired Joy-Con pair is a real USB hub (the Charging Grip) with two
 * independently-addressed child devices, and this project's single Pico USB peripheral can only
 * ever hold one USB address at a time (confirmed at the register level -- see
 * docs/switch2-joycon2/protocol.md "Why not simultaneous L+R"), so presenting two Joy-Con
 * identities concurrently from one Pico is not implementable, not merely undesigned.
 */
#ifndef SWITCH_JOYCON2_H
#define SWITCH_JOYCON2_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "switch_joycon2_encode.h"  // switch_joycon2_encode_report[05] (pure, host-testable), joycon2_side_t

// Which side this personality currently presents as. Set automatically by usb.c's
// usb_reset_personality_state() based on whether USB_PERSONALITY_JOYCON2_L or _R is the active
// personality -- there is no config UI for this and none is planned (see this file's own top
// comment): the two personalities ARE the selection. Changing side while a host is connected is
// not a defined operation (would need a fresh enumeration, since VID/PID/serial differ) -- only
// called during a personality transition, before the new personality becomes active.
void switch_joycon2_set_side(joycon2_side_t side);
joycon2_side_t switch_joycon2_get_side(void);

// Descriptor accessors, used by usb_descriptors.c's personality dispatch. All read
// switch_joycon2_get_side() to return the correct side's variant.
const uint8_t *switch_joycon2_device_descriptor(void);
const uint8_t *switch_joycon2_config_descriptor(void);
const uint8_t *switch_joycon2_hid_report_descriptor(void);  // 100 bytes -- Confirmed
const char   **switch_joycon2_string_table(size_t *count);

// Microsoft OS 1.0 string (index 0xEE) -- same WinUSB auto-bind mechanism switch_gc.h's own
// identically-named accessor already documents in full; not re-explained here.
const uint16_t *switch_joycon2_ms_os_string_descriptor(void);

// core0 lifecycle, mirroring switch_gc.h's switch_gc_init()/_task()/_reset()/_mount() shape
// exactly (same personality-lifecycle contract, different protocol).
void switch_joycon2_init(void);
void switch_joycon2_task(void);
void switch_joycon2_reset(void);
void switch_joycon2_mount(void);

// HID OUT report entry point (report 0x01, rumble/LED). `report_id`/`data`/`len` already
// normalized by usb_descriptors.c's tud_hid_set_report_cb() dispatcher, same contract
// switch_gc.h's identically-named function documents in full.
void switch_joycon2_hid_out_report(uint8_t report_id, const uint8_t *data, uint16_t len);

// EP0 vendor control request entry point, called from the centralized
// tud_vendor_control_xfer_cb dispatcher in usb_descriptors.c. Same role as
// switch_gc_vendor_control_xfer() -- the Windows WinUSB auto-bind request, plus the Nintendo
// Switch 2 console identity handshake (bRequest 0x02/0x03/0x04).
bool switch_joycon2_vendor_control_xfer(uint8_t rhport, uint8_t stage, const void *request);

#endif  // SWITCH_JOYCON2_H
