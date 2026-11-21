// Forced into every PicoSwitch2 source by firmware/CMakeLists.txt when
// NS2_DISCARD_PRINTF is ON (the default).
//
// WHY. This firmware has no stdio back end: USB is the controller and UART0 is
// owned by the nonblocking diagnostic console (ns2_uart_diag), so both
// pico_enable_stdio_* are 0. printf() still linked and still ran: every call
// formatted its arguments and handed them to zero output drivers. The joypad-os
// host and its drivers carry several hundred such calls, many on the BTstack
// thread, and their format strings alone were tens of kilobytes of flash.
//
// WHAT THIS DOES. Removes the call at the preprocessor, so neither the
// formatting nor the strings reach the binary. Arguments are NOT evaluated; a
// printf argument must therefore never have a side effect. That was already a
// requirement in practice -- the output went nowhere -- and
// tests/tools/test_firmware_printf_policy.py keeps it true.
//
// WHAT IT DOES NOT TOUCH. snprintf/vsnprintf (the diagnostic console and the
// management replies are built with them), the SDK, BTstack, TinyUSB and Opus:
// the option is applied to this project's sources only.
//
// To get the text back for a debugging session, configure with
// -DNS2_DISCARD_PRINTF=OFF and attach a stdio driver.
#ifndef NS2_NO_STDIO_H
#define NS2_NO_STDIO_H

#include <stdio.h>

#undef printf
#define printf(...) ((void)0)

#endif
