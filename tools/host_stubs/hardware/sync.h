#ifndef _HOST_STUB_HARDWARE_SYNC_H_
#define _HOST_STUB_HARDWARE_SYNC_H_

// Host stand-in for hardware/sync.h.
//
// The host cannot disable interrupts, and that is fine -- what a test needs from this is (a) a
// real memory barrier so cross-thread handshakes are ordered as they would be on a Cortex-M, and
// (b) visibility of the critical-section window, which on hardware is the interval during which a
// core is guaranteed to be executing from SRAM. pico_host_irqs_disabled carries (b).

#include <stdint.h>

#include "pico_host_hooks.h"

#ifndef __dmb
#define __dmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

static inline uint32_t save_and_disable_interrupts(void)
{
    pico_host_irqs_disabled = true;
    __dmb();
    return 0;
}

static inline void restore_interrupts(uint32_t status)
{
    (void)status;
    __dmb();
    pico_host_irqs_disabled = false;
}

#endif  // _HOST_STUB_HARDWARE_SYNC_H_
