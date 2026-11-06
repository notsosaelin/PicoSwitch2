#ifndef _HOST_STUB_HARDWARE_STRUCTS_SIO_H_
#define _HOST_STUB_HARDWARE_STRUCTS_SIO_H_

// Host stand-in for hardware/structs/sio.h. Only the high GPIO input register is modelled, which
// is where the QSPI CS pin (and therefore BOOTSEL) is read. Active low: bit clear == pressed.

#include <stdint.h>

#include "pico_host_hooks.h"

#define SIO_GPIO_HI_IN_QSPI_CSN_BITS 0x00000002u

typedef struct {
    volatile uint32_t gpio_hi_in;
} host_sio_hw_t;

extern host_sio_hw_t host_sio_hw_instance;
#define sio_hw (host_sio_hw_refresh())

host_sio_hw_t *host_sio_hw_refresh(void);

#endif  // _HOST_STUB_HARDWARE_STRUCTS_SIO_H_
