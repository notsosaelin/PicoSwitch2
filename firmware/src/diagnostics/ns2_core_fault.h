// ns2_core_fault.h - Where a core stopped: fault capture and a core 1 PC sampler.
//
// Built for a failure mode that leaves no other trace: core 1 (BTstack IRQ +
// codec worker) stops while core 0 and the UART live on. This build has no
// stdio, so a panic or fault is silent. Two instruments, read over UART
// (`fault`):
//
// - FAULT RECORD. A HardFault on either core records the stacked PC, LR and
//   xPSR, the core, and on Armv8-M the fault status registers, then parks the
//   core. Map PC/LR to source with arm-none-eabi-addr2line on the build's ELF.
//
// - CORE 1 SAMPLER. A highest-priority timer interrupt on core 1 records the
//   interrupted PC every NS2_CORE1_SAMPLE_PERIOD_US, with the exception it
//   interrupted (0 = thread mode, else 16 + IRQ). When core 1 stops making
//   progress the last samples show where it spins. Samples that also stop
//   mean it spins with interrupts disabled (a spin lock, a critical section).
#ifndef NS2_CORE_FAULT_H
#define NS2_CORE_FAULT_H

#include <stddef.h>
#include <stdint.h>

#define NS2_CORE1_SAMPLE_PERIOD_US 10000u
#define NS2_CORE1_SAMPLE_RING 8u

// Call on core 1 before its event loop: the sampler's interrupt is per core.
void ns2_core1_sampler_start(void);

// JSON for the UART `fault` command.
void ns2_core_fault_format(char *out, size_t size);

#endif
