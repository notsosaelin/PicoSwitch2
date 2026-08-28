#ifndef _HOST_STUB_PICO_STDLIB_H_
#define _HOST_STUB_PICO_STDLIB_H_

// Minimal host stand-in for pico/stdlib.h. Only what firmware sources compiled by
// tools/run_host_tests.ps1 actually use; add here rather than widening a test's own header so
// every host test sees one consistent stub.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pico_host_hooks.h"

#ifndef PICO_RP2040
#define PICO_RP2040 1
#endif

typedef unsigned int uint;

// Section placement attributes are meaningless on the host, but the names must still resolve or
// every function that declares itself RAM-resident silently becomes an implicit-int declaration.
#ifndef __no_inline_not_in_flash_func
#define __no_inline_not_in_flash_func(fn) fn
#endif
#ifndef __not_in_flash_func
#define __not_in_flash_func(fn) fn
#endif
#ifndef __time_critical_func
#define __time_critical_func(fn) fn
#endif

static inline uint32_t time_us_32(void)
{
    return pico_host_now_us;
}

#endif  // _HOST_STUB_PICO_STDLIB_H_
