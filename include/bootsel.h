#ifndef _BOOTSEL_H_
#define _BOOTSEL_H_

#include <stdint.h>
#include <stdbool.h>

#include "bootsel_gesture.h"

// BOOTSEL-button gestures. The Pico's only button doubles as the flash chip select, so reading
// it at runtime requires briefly tri-stating that pin with the *other* core parked (see
// bootsel.c).
//
// ARCHITECTURE (rewritten 2026-07-15/16 -- read this before moving the sampling back):
// The raw sample is taken by CORE0 (bootsel_sample_core0(), called from usb_core_task()'s loop),
// which parks core1 cooperatively through an SRAM handshake. The gesture state machine runs on
// CORE1 (bootsel_poll()), reading the value core0 published.
//
// It used to be the other way around (core1 sampled, parking core0) and that is unfixable as a
// design: core0 runs TinyUSB in a tight, continuous loop and cannot grant a lockout promptly
// while streaming to a host, so core1 either blocked for tens of ms per tick (BOOTSEL worked,
// but core1's stalls delayed rumble forwarding -- Xbox motors kept running because their stop
// command arrived late) or bailed out on a short timeout (rumble fine, but BOOTSEL silently did
// nothing). Both failure modes were observed on real hardware on 2026-07-15. Core1 is a
// A first inverted implementation still used multicore_lockout with a 200 us FIFO-IRQ timeout;
// real DualSense traffic could prevent core1 acknowledging inside that deadline indefinitely.
// The current handshake is asynchronous: core0 requests and returns to USB, core1 voluntarily
// parks from its timer callback, then core0 samples on a later loop iteration without waiting.
// Gesture meanings remain:
//   BOOTSEL_DOUBLE_TAP -> enter pairing window
//   BOOTSEL_TRIPLE_TAP -> wipe saved Bluetooth devices
//   BOOTSEL_HOLD       -> >= 5s: cycle USB personality under NS2_PRO, or enter configuration
//                         mode in Switch 1 builds. See usb.h.

// CORE0 ONLY. Sample the BOOTSEL pin and publish it for bootsel_poll(). Call from core0's main
// loop; it self-rate-limits, so calling it every iteration is fine and costs almost nothing
// between samples. Requires core1 to have called bootsel_core1_lockout_init() first; until then
// it is a no-op and bootsel_poll() reports no gestures.
void bootsel_sample_core0(void);

// CORE1 ONLY. Initialize and service the cooperative SRAM park point. Service returns immediately
// unless core0 has requested a sample and must be called frequently from the BTstack run loop.
void bootsel_core1_lockout_init(void);
void bootsel_core1_service(void);

// Run the gesture state machine once. This Pico wrapper passes the latest published hardware
// sample into the pure bootsel_gesture_update() recognizer. Call periodically (~30 ms) or at HID
// report boundaries with the current millisecond timestamp. Returns a recognized gesture, else
// BOOTSEL_NONE. Never parks a core and never blocks.
bootsel_gesture_t bootsel_poll(uint32_t now_ms);

#endif  // _BOOTSEL_H_
