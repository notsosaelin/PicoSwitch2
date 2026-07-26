#ifndef NS2_VENDOR_TX_H
#define NS2_VENDOR_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Largest currently modelled Switch 2 NFC response:
// 8-byte vendor envelope + 622-byte NFC payload.
#define NS2_VENDOR_TX_MAX_RESPONSE 630u

typedef size_t (*ns2_vendor_tx_write_fn)(void *context,
                                         const uint8_t *data,
                                         size_t size);

typedef struct {
    uint8_t data[NS2_VENDOR_TX_MAX_RESPONSE];
    uint16_t length;
    uint16_t offset;
    bool active;
} ns2_vendor_tx_t;

void ns2_vendor_tx_init(ns2_vendor_tx_t *tx);
bool ns2_vendor_tx_queue(ns2_vendor_tx_t *tx,
                         const uint8_t *data, size_t size);
size_t ns2_vendor_tx_pump(ns2_vendor_tx_t *tx,
                          ns2_vendor_tx_write_fn writer,
                          void *context);
bool ns2_vendor_tx_active(const ns2_vendor_tx_t *tx);
size_t ns2_vendor_tx_remaining(const ns2_vendor_tx_t *tx);

#endif
