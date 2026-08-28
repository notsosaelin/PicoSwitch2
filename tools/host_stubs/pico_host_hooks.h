#ifndef _PICO_HOST_HOOKS_H_
#define _PICO_HOST_HOOKS_H_

// Test-visible seams for the pico-sdk stubs in tools/host_stubs. These let a host test observe
// the things that are hardware side effects on a real Pico -- the interrupt-disable window and
// the flash CS tri-state -- so cross-core handshakes can be tested against the shipped firmware
// source instead of a re-implementation of it.

#include <stdbool.h>
#include <stdint.h>

// Monotonic clock backing time_us_32(). Tests own it, so rate limiters are deterministic.
extern volatile uint32_t pico_host_now_us;

// Set between save_and_disable_interrupts() and the matching restore_interrupts(). On real
// hardware this is exactly the window in which a core is guaranteed to be running from SRAM.
extern volatile bool pico_host_irqs_disabled;

// Raised by the ioqspi stub whenever flash CS is driven to Hi-Z, i.e. whenever XIP is unavailable.
// Counted so a test can prove a sample actually happened.
extern volatile uint32_t pico_host_cs_hiz_count;

// Called by the ioqspi stub on every CS override write. A test installs this to assert that the
// other core really is parked whenever CS goes Hi-Z. NULL by default.
extern void (*pico_host_on_cs_override)(bool hi_z);

// Value reported by sio_hw->gpio_hi_in for the QSPI CS pin. false == BOOTSEL pressed, matching
// the active-low hardware.
extern volatile bool pico_host_qspi_cs_level;

void pico_host_hooks_reset(void);

#endif  // _PICO_HOST_HOOKS_H_
