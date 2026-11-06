#include "ns2_vendor_tx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t output[NS2_VENDOR_TX_MAX_RESPONSE];
    size_t output_size;
    const size_t *acceptance;
    size_t acceptance_count;
    size_t call;
} writer_fixture_t;

static size_t fixture_write(void *context, const uint8_t *data, size_t size)
{
    writer_fixture_t *fixture = context;
    size_t accepted = size;
    if (fixture->call < fixture->acceptance_count)
        accepted = fixture->acceptance[fixture->call];
    fixture->call++;
    if (accepted > size) accepted = size;
    memcpy(fixture->output + fixture->output_size, data, accepted);
    fixture->output_size += accepted;
    return accepted;
}

int main(void)
{
    uint8_t response[NS2_VENDOR_TX_MAX_RESPONSE];
    for (size_t i = 0; i < sizeof(response); ++i)
        response[i] = (uint8_t)(i ^ (i >> 8));

    ns2_vendor_tx_t tx;
    ns2_vendor_tx_init(&tx);
    assert(!ns2_vendor_tx_active(&tx));
    assert(!ns2_vendor_tx_queue(&tx, response, 0));
    assert(!ns2_vendor_tx_queue(&tx, response, sizeof(response) + 1));
    assert(ns2_vendor_tx_queue(&tx, response, sizeof(response)));
    assert(!ns2_vendor_tx_queue(&tx, response, 8));

    // Includes zero-progress callbacks, short writes, and an over-reported
    // acceptance count. The output must remain byte-exact and ordered.
    const size_t acceptance[] = {0, 17, 64, 0, 1, 127, 33, 900};
    writer_fixture_t fixture = {
        .acceptance = acceptance,
        .acceptance_count = sizeof(acceptance) / sizeof(acceptance[0]),
    };
    unsigned guard = 0;
    while (ns2_vendor_tx_active(&tx)) {
        (void)ns2_vendor_tx_pump(&tx, fixture_write, &fixture);
        assert(++guard < 32);
    }
    assert(fixture.output_size == sizeof(response));
    assert(memcmp(fixture.output, response, sizeof(response)) == 0);
    assert(ns2_vendor_tx_remaining(&tx) == 0);

    puts("ns2_vendor_tx: all tests passed");
    return 0;
}
