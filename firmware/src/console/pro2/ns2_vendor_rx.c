#include "ns2_vendor_rx.h"

#include <string.h>

void ns2_vendor_rx_init(ns2_vendor_rx_t *rx)
{
    if (rx) memset(rx, 0, sizeof(*rx));
}

static void reset_command(ns2_vendor_rx_t *rx)
{
    rx->length = 0;
    rx->expected = 0;
}

size_t ns2_vendor_rx_feed(ns2_vendor_rx_t *rx,
                          const uint8_t *data, size_t size,
                          ns2_vendor_rx_dispatch_fn dispatch,
                          void *context)
{
    if (!rx || (!data && size != 0) || !dispatch) return 0;

    size_t dispatched = 0;
    size_t offset = 0;
    while (offset < size) {
        if (rx->discard_remaining != 0) {
            size_t discard = size - offset;
            if (discard > rx->discard_remaining)
                discard = rx->discard_remaining;
            offset += discard;
            rx->discard_remaining =
                (uint16_t)(rx->discard_remaining - discard);
            continue;
        }

        size_t target = rx->expected ? rx->expected : 8u;
        size_t needed = target - rx->length;
        size_t count = size - offset;
        if (count > needed) count = needed;
        memcpy(rx->data + rx->length, data + offset, count);
        rx->length = (uint16_t)(rx->length + count);
        offset += count;

        if (rx->expected == 0 && rx->length == 8u) {
            const uint16_t payload_size =
                (uint16_t)((uint16_t)rx->data[4] << 8) | rx->data[5];
            const uint32_t total_size = 8u + payload_size;
            if (rx->data[1] != 0x91u ||
                total_size > NS2_VENDOR_RX_MAX_COMMAND) {
                rx->rejected++;
                if (rx->data[1] == 0x91u &&
                    total_size > NS2_VENDOR_RX_MAX_COMMAND)
                    rx->discard_remaining = payload_size;
                reset_command(rx);
                continue;
            }
            rx->expected = (uint16_t)total_size;
        }

        if (rx->expected != 0 && rx->length == rx->expected) {
            dispatch(context, rx->data, rx->length);
            dispatched++;
            reset_command(rx);
        }
    }
    return dispatched;
}

size_t ns2_vendor_rx_pending(const ns2_vendor_rx_t *rx)
{
    return rx ? rx->length : 0;
}
