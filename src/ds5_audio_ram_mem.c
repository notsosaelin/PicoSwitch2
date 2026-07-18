#include <stddef.h>
#include <stdint.h>

#ifdef NS2_DS5_AUDIO_LIVE_OPUS

#include "pico.h"

// The live Opus hot path is deliberately SRAM-resident. Keep its common memory
// primitives there too; otherwise every encode still jumps back into newlib's
// XIP-flash memcpy/memset implementations. This follows the optimization used
// by DS5Dongle, with simple aligned word loops suitable for the RP2350.
//
// This translation unit is built with builtin/pattern recognition disabled in
// CMake so the compiler cannot lower these loops into recursive calls.

void *__not_in_flash_func(memcpy)(void *restrict dst,
                                  const void *restrict src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0u) {
        uint32_t *dw = (uint32_t *)d;
        const uint32_t *sw = (const uint32_t *)s;
        for (size_t words = n >> 2; words != 0; --words) *dw++ = *sw++;
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
        n &= 3u;
    }
    while (n-- != 0) *d++ = *s++;
    return dst;
}

void *__not_in_flash_func(memset)(void *dst, int value, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t const byte = (uint8_t)value;
    if (((uintptr_t)d & 3u) == 0u) {
        uint32_t const word = (uint32_t)byte * 0x01010101u;
        uint32_t *dw = (uint32_t *)d;
        for (size_t words = n >> 2; words != 0; --words) *dw++ = word;
        d = (uint8_t *)dw;
        n &= 3u;
    }
    while (n-- != 0) *d++ = byte;
    return dst;
}

void *__not_in_flash_func(memmove)(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;

    if (d < s) {
        if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0u) {
            uint32_t *dw = (uint32_t *)d;
            const uint32_t *sw = (const uint32_t *)s;
            for (size_t words = n >> 2; words != 0; --words) *dw++ = *sw++;
            d = (uint8_t *)dw;
            s = (const uint8_t *)sw;
            n &= 3u;
        }
        while (n-- != 0) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n-- != 0) *--d = *--s;
    }
    return dst;
}

#endif
