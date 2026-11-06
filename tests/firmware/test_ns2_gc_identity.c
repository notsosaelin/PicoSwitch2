#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_gc_identity.h"

int main(void) {
    static const uint8_t expected_command[NS2_GC_COMMAND_INFO_LEN] = {
        0x01, 0x01, 0x02, 0x03, 0x0C, 0x00,
        0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    const uint8_t unit_id[6] = {0x02, 0xBB, 0x5E, 0xAB, 0xA9, 0x3C};
    uint8_t ep0[NS2_GC_EP0_INFO_LEN];
    uint8_t command[NS2_GC_COMMAND_INFO_LEN];

    ns2_gc_build_ep0_info(unit_id, ep0);
    ns2_gc_build_command_info(command);

    assert(memcmp(command, expected_command, sizeof(expected_command)) == 0);
    assert(memcmp(ep0, command, 3) == 0);
    assert(ep0[6] == command[4]);
    assert(memcmp(&ep0[10], unit_id, sizeof(unit_id)) == 0);

    puts("ns2_gc_identity: all tests passed");
    return 0;
}
