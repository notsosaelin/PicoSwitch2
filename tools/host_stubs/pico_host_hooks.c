#include "pico_host_hooks.h"

#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"

volatile uint32_t pico_host_now_us;
volatile bool pico_host_irqs_disabled;
volatile uint32_t pico_host_cs_hiz_count;
void (*pico_host_on_cs_override)(bool hi_z);
volatile bool pico_host_qspi_cs_level = true;   // released

host_ioqspi_hw_t host_ioqspi_hw_instance;
host_sio_hw_t host_sio_hw_instance;

host_sio_hw_t *host_sio_hw_refresh(void)
{
    // Active low, exactly as the pad reads on hardware: CS pulled low == BOOTSEL pressed.
    host_sio_hw_instance.gpio_hi_in = pico_host_qspi_cs_level
        ? SIO_GPIO_HI_IN_QSPI_CSN_BITS : 0u;
    return &host_sio_hw_instance;
}

void pico_host_hooks_reset(void)
{
    pico_host_now_us = 0;
    pico_host_irqs_disabled = false;
    pico_host_cs_hiz_count = 0;
    pico_host_on_cs_override = 0;
    pico_host_qspi_cs_level = true;
    host_ioqspi_hw_instance.io[1].ctrl = 0;
}
