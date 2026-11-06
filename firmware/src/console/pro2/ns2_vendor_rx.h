#ifndef NS2_VENDOR_RX_H
#define NS2_VENDOR_RX_H

#include <stddef.h>
#include <stdint.h>

// Largest console-to-controller command currently needed by the Switch 2
// command surface. NFC 0x14 carries an 80-byte payload and is 88 bytes total,
// so it necessarily spans two 64-byte USB bulk packets.
#define NS2_VENDOR_RX_MAX_COMMAND 128u

typedef void (*ns2_vendor_rx_dispatch_fn)(void *context,
                                          const uint8_t *command,
                                          size_t size);

typedef struct {
    uint8_t data[NS2_VENDOR_RX_MAX_COMMAND];
    uint16_t length;
    uint16_t expected;
    uint16_t discard_remaining;
    uint32_t rejected;
} ns2_vendor_rx_t;

void ns2_vendor_rx_init(ns2_vendor_rx_t *rx);

// Consume an arbitrary USB byte-stream fragment. The command header carries a
// big-endian payload length at bytes 4..5; a callback runs only after all
// 8+payload bytes are present. Multiple commands in one fragment and commands
// split across any number of fragments are both supported.
size_t ns2_vendor_rx_feed(ns2_vendor_rx_t *rx,
                          const uint8_t *data, size_t size,
                          ns2_vendor_rx_dispatch_fn dispatch,
                          void *context);

size_t ns2_vendor_rx_pending(const ns2_vendor_rx_t *rx);

#endif
