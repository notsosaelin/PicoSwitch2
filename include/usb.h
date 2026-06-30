#ifndef _USB_H_
#define _USB_H_

#include <stdbool.h>

// Set true by the USB core (core0) once it has registered as a multicore
// lockout victim, so the Bluetooth core may safely park it to read BOOTSEL.
extern volatile bool usb_lockout_ready;

// Configuration mode. The Bluetooth core sets g_usb_enter_config (via the
// BOOTSEL hold gesture) to request it; the USB core then re-enumerates as a CDC
// serial device and sets g_usb_config_mode true. Used by the descriptor
// callbacks (HID vs CDC) and the LED feedback.
extern volatile bool g_usb_enter_config;
extern volatile bool g_usb_config_mode;

void usb_core_task();

#endif
