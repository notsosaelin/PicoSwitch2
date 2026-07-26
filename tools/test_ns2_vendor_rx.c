#include "ns2_vendor_rx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t commands[4][NS2_VENDOR_RX_MAX_COMMAND];
    size_t sizes[4];
    size_t count;
} fixture_t;

static void capture(void *context, const uint8_t *command, size_t size)
{
    fixture_t *fixture = context;
    assert(fixture->count < 4);
    assert(size <= NS2_VENDOR_RX_MAX_COMMAND);
    memcpy(fixture->commands[fixture->count], command, size);
    fixture->sizes[fixture->count] = size;
    fixture->count++;
}

static void make_command(uint8_t *command, size_t payload_size,
                         uint8_t id, uint8_t subcommand)
{
    const size_t total = 8u + payload_size;
    for (size_t i = 0; i < total; ++i)
        command[i] = (uint8_t)(i * 13u + subcommand);
    command[0] = id;
    command[1] = 0x91;
    command[2] = 0;
    command[3] = subcommand;
    command[4] = (uint8_t)(payload_size >> 8);
    command[5] = (uint8_t)payload_size;
    command[6] = 0;
    command[7] = 0;
}

int main(void)
{
    ns2_vendor_rx_t rx;
    ns2_vendor_rx_init(&rx);
    fixture_t fixture = {0};

    uint8_t write_command[88];
    make_command(write_command, 80, 0x01, 0x14);

    // This is the exact transport boundary which crashed the console: the
    // 88-byte NFC write command arrives as one 64-byte packet plus 24 bytes.
    assert(ns2_vendor_rx_feed(
               &rx, write_command, 64, capture, &fixture) == 0);
    assert(ns2_vendor_rx_pending(&rx) == 64);
    assert(ns2_vendor_rx_feed(
               &rx, write_command + 64, 24, capture, &fixture) == 1);
    assert(fixture.count == 1 && fixture.sizes[0] == sizeof(write_command));
    assert(memcmp(fixture.commands[0], write_command,
                  sizeof(write_command)) == 0);
    assert(ns2_vendor_rx_pending(&rx) == 0);

    // Arbitrary smaller fragmentation has the same byte-exact result.
    ns2_vendor_rx_init(&rx);
    memset(&fixture, 0, sizeof(fixture));
    for (size_t offset = 0; offset < sizeof(write_command); offset += 7) {
        size_t count = sizeof(write_command) - offset;
        if (count > 7) count = 7;
        ns2_vendor_rx_feed(
            &rx, write_command + offset, count, capture, &fixture);
    }
    assert(fixture.count == 1 && fixture.sizes[0] == sizeof(write_command));
    assert(memcmp(fixture.commands[0], write_command,
                  sizeof(write_command)) == 0);

    // Back-to-back commands may share one TinyUSB read.
    uint8_t pair[88 + 8];
    memcpy(pair, write_command, sizeof(write_command));
    make_command(pair + sizeof(write_command), 0, 0x01, 0x08);
    ns2_vendor_rx_init(&rx);
    memset(&fixture, 0, sizeof(fixture));
    assert(ns2_vendor_rx_feed(
               &rx, pair, sizeof(pair), capture, &fixture) == 2);
    assert(fixture.count == 2);
    assert(fixture.sizes[0] == 88 && fixture.sizes[1] == 8);
    assert(fixture.commands[1][3] == 0x08);

    // An oversized command is discarded using its declared length, after
    // which framing recovers on the following valid command.
    uint8_t oversized_header[8] =
        {0x01, 0x91, 0, 0x14, 0x00, 0x81, 0, 0};
    uint8_t oversized_tail[129] = {0};
    uint8_t commit[8];
    make_command(commit, 0, 0x01, 0x08);
    ns2_vendor_rx_init(&rx);
    memset(&fixture, 0, sizeof(fixture));
    assert(ns2_vendor_rx_feed(
               &rx, oversized_header, sizeof(oversized_header),
               capture, &fixture) == 0);
    assert(rx.rejected == 1 && rx.discard_remaining == 129);
    assert(ns2_vendor_rx_feed(
               &rx, oversized_tail, sizeof(oversized_tail),
               capture, &fixture) == 0);
    assert(ns2_vendor_rx_feed(
               &rx, commit, sizeof(commit), capture, &fixture) == 1);
    assert(fixture.count == 1 && fixture.commands[0][3] == 0x08);

    puts("ns2_vendor_rx: all tests passed");
    return 0;
}
