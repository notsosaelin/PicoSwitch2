#ifndef _HOST_STUB_HARDWARE_STRUCTS_IOQSPI_H_
#define _HOST_STUB_HARDWARE_STRUCTS_IOQSPI_H_

// Host stand-in for hardware/structs/ioqspi.h plus the hw_write_masked() helper that normally
// arrives via hardware/address_mapped.h.
//
// The CS override write is the one hardware effect that matters to a host test: driving OEOVER
// low tri-states flash chip select, so XIP is unavailable and any core executing from flash will
// fault. The stub reports each transition through pico_host_on_cs_override() so a test can assert
// the other core really was parked at that moment.

#include <stdint.h>

#include "hardware/gpio.h"
#include "pico_host_hooks.h"

#define IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB 12u
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS 0x00003000u

typedef struct {
    volatile uint32_t status;
    volatile uint32_t ctrl;
} host_ioqspi_io_hw_t;

typedef struct {
    host_ioqspi_io_hw_t io[6];
} host_ioqspi_hw_t;

extern host_ioqspi_hw_t host_ioqspi_hw_instance;
#define ioqspi_hw (&host_ioqspi_hw_instance)

static inline void hw_write_masked(volatile uint32_t *addr, uint32_t values, uint32_t write_mask)
{
    uint32_t next = (*addr & ~write_mask) | (values & write_mask);
    *addr = next;

    if (write_mask & IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS) {
        uint32_t oeover = (next & IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS) >>
                          IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB;
        bool hi_z = (oeover == (uint32_t)GPIO_OVERRIDE_LOW);
        if (hi_z) {
            ++pico_host_cs_hiz_count;
        }
        if (pico_host_on_cs_override) {
            pico_host_on_cs_override(hi_z);
        }
    }
}

#endif  // _HOST_STUB_HARDWARE_STRUCTS_IOQSPI_H_
