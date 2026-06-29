#ifndef _USB_H_
#define _USB_H_

#include <stdbool.h>

// Set true by the USB core (core0) once it has registered as a multicore
// lockout victim, so the Bluetooth core may safely park it to read BOOTSEL.
extern volatile bool usb_lockout_ready;

void usb_core_task();

#endif
