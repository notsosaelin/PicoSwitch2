#include "ns2_vendor_tx.h"

#include <string.h>

void ns2_vendor_tx_init(ns2_vendor_tx_t *tx)
{
    if (tx) memset(tx, 0, sizeof(*tx));
}

bool ns2_vendor_tx_queue(ns2_vendor_tx_t *tx,
                         const uint8_t *data, size_t size)
{
    if (!tx || (!data && size != 0) ||
        size == 0 || size > sizeof(tx->data) || tx->active)
        return false;
    memcpy(tx->data, data, size);
    tx->length = (uint16_t)size;
    tx->offset = 0;
    tx->active = true;
    return true;
}

size_t ns2_vendor_tx_pump(ns2_vendor_tx_t *tx,
                          ns2_vendor_tx_write_fn writer,
                          void *context)
{
    if (!tx || !writer || !tx->active) return 0;

    const size_t remaining = tx->length - tx->offset;
    size_t accepted = writer(context, tx->data + tx->offset, remaining);
    if (accepted > remaining) accepted = remaining;
    tx->offset = (uint16_t)(tx->offset + accepted);
    if (tx->offset == tx->length) {
        tx->active = false;
        tx->offset = 0;
        tx->length = 0;
    }
    return accepted;
}

bool ns2_vendor_tx_active(const ns2_vendor_tx_t *tx)
{
    return tx && tx->active;
}

size_t ns2_vendor_tx_remaining(const ns2_vendor_tx_t *tx)
{
    return tx && tx->active ? (size_t)(tx->length - tx->offset) : 0;
}
