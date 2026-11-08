// ns2_core_fault.c - See ns2_core_fault.h.
#include "ns2_core_fault.h"

#include <stdbool.h>
#include <stdio.h>

#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico/platform.h"

typedef struct {
    volatile uint32_t valid;
    volatile uint32_t core;
    volatile uint32_t pc;
    volatile uint32_t lr;
    volatile uint32_t xpsr;
    volatile uint32_t cfsr;
    volatile uint32_t hfsr;
    volatile uint32_t mmfar;
    volatile uint32_t bfar;
    volatile uint32_t time_us;
} fault_record_t;

static fault_record_t fault_record;

static volatile uint32_t sampler_alarm = UINT32_MAX;
static volatile uint32_t sample_count;
static volatile uint32_t sample_time_us;
static volatile uint32_t sample_pc[NS2_CORE1_SAMPLE_RING];
static volatile uint32_t sample_lr[NS2_CORE1_SAMPLE_RING];
static volatile uint32_t sample_exception[NS2_CORE1_SAMPLE_RING];

// Both handlers find the interrupted context's exception frame (MSP or PSP by
// EXC_RETURN bit 2) and pass it to C. Thumb-1 only, so the same code serves
// the RP2040 (Cortex-M0+) and the RP2350 (Cortex-M33).
#define NS2_FRAME_TRAMPOLINE(target)        \
    __asm volatile(                         \
        "movs r0, #4            \n"         \
        "mov  r1, lr            \n"         \
        "tst  r0, r1            \n"         \
        "beq  1f                \n"         \
        "mrs  r0, psp           \n"         \
        "b    2f                \n"         \
        "1:                     \n"         \
        "mrs  r0, msp           \n"         \
        "2:                     \n"         \
        "ldr  r1, =" #target "  \n"         \
        "bx   r1                \n"         \
        ".ltorg                 \n")

void __used ns2_hardfault_record(uint32_t *frame)
{
    fault_record.core = get_core_num();
    fault_record.lr = frame[5];
    fault_record.pc = frame[6];
    fault_record.xpsr = frame[7];
#if defined(__ARM_ARCH_8M_MAIN__)
    fault_record.cfsr = *(volatile uint32_t *)0xE000ED28u;
    fault_record.hfsr = *(volatile uint32_t *)0xE000ED2Cu;
    fault_record.mmfar = *(volatile uint32_t *)0xE000ED34u;
    fault_record.bfar = *(volatile uint32_t *)0xE000ED38u;
#endif
    fault_record.time_us = timer_hw->timerawl;
    __dmb();
    fault_record.valid = 1u;
    // Park this core. The other keeps running, so the UART can report.
    for (;;) __wfi();
}

// Overrides the SDK's weak handler (pico_crt0).
void __attribute__((naked)) isr_hardfault(void)
{
    NS2_FRAME_TRAMPOLINE(ns2_hardfault_record);
}

void __used __not_in_flash_func(ns2_core1_sample)(uint32_t *frame)
{
    uint32_t const alarm = sampler_alarm;
    timer_hw->intr = 1u << alarm;  // write-1-to-clear
    uint32_t const now = timer_hw->timerawl;
    timer_hw->alarm[alarm] = now + NS2_CORE1_SAMPLE_PERIOD_US;
    uint32_t const slot = sample_count % NS2_CORE1_SAMPLE_RING;
    sample_pc[slot] = frame[6];
    sample_lr[slot] = frame[5];
    sample_exception[slot] = frame[7] & 0x1FFu;
    sample_time_us = now;
    sample_count++;
}

static void __attribute__((naked)) __not_in_flash_func(ns2_core1_sampler_isr)(void)
{
    NS2_FRAME_TRAMPOLINE(ns2_core1_sample);
}

void ns2_core1_sampler_start(void)
{
    int const alarm = hardware_alarm_claim_unused(false);
    if (alarm < 0) return;
    sampler_alarm = (uint32_t)alarm;
    uint const irq = timer_hardware_alarm_get_irq_num(timer_hw, (uint)alarm);
    irq_set_exclusive_handler(irq, ns2_core1_sampler_isr);
    // Highest priority: it must preempt the BTstack IRQ it is there to watch.
    irq_set_priority(irq, 0);
    timer_hw->intr = 1u << alarm;
    hw_set_bits(&timer_hw->inte, 1u << alarm);
    timer_hw->alarm[alarm] = timer_hw->timerawl + NS2_CORE1_SAMPLE_PERIOD_US;
    irq_set_enabled(irq, true);
}

void ns2_core_fault_format(char *out, size_t size)
{
    uint32_t const now = timer_hw->timerawl;
    uint32_t const count = sample_count;
    int n = snprintf(out, size,
                     "{\"fault\":%s,\"core\":%lu,\"pc\":\"0x%08lX\",\"lr\":\"0x%08lX\","
                     "\"xpsr\":\"0x%08lX\",\"cfsr\":\"0x%08lX\",\"hfsr\":\"0x%08lX\","
                     "\"mmfar\":\"0x%08lX\",\"bfar\":\"0x%08lX\",\"fault_age_ms\":%lu,"
                     "\"sampler\":{\"running\":%s,\"count\":%lu,\"age_ms\":%lu,\"recent\":[",
                     fault_record.valid ? "true" : "false",
                     (unsigned long)fault_record.core, (unsigned long)fault_record.pc,
                     (unsigned long)fault_record.lr, (unsigned long)fault_record.xpsr,
                     (unsigned long)fault_record.cfsr, (unsigned long)fault_record.hfsr,
                     (unsigned long)fault_record.mmfar, (unsigned long)fault_record.bfar,
                     fault_record.valid ? (unsigned long)((now - fault_record.time_us) / 1000u) : 0ul,
                     sampler_alarm != UINT32_MAX ? "true" : "false",
                     (unsigned long)count, (unsigned long)((now - sample_time_us) / 1000u));
    // Newest first.
    uint32_t const shown = count < NS2_CORE1_SAMPLE_RING ? count : NS2_CORE1_SAMPLE_RING;
    for (uint32_t i = 0; i < shown && n > 0 && (size_t)n < size; i++) {
        uint32_t const slot = (count - 1u - i) % NS2_CORE1_SAMPLE_RING;
        n += snprintf(out + n, size - (size_t)n, "%s{\"pc\":\"0x%08lX\",\"lr\":\"0x%08lX\",\"exc\":%lu}",
                      i ? "," : "", (unsigned long)sample_pc[slot], (unsigned long)sample_lr[slot],
                      (unsigned long)sample_exception[slot]);
    }
    if (n > 0 && (size_t)n < size) snprintf(out + n, size - (size_t)n, "]}}");
}
