#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_joycon2_identity.h"

int main(void) {
    uint8_t identity[NS2_JOYCON2_IDENTITY_LEN];

    const uint8_t left[3] = {0x9B, 0xE1, 0xE6};
    ns2_joycon2_build_identity(false, left, identity);
    assert(identity[2] == 'H' && identity[3] == 'B');
    assert(identity[18] == 0x7E && identity[19] == 0x05);
    assert(identity[20] == 0x67 && identity[21] == 0x20);
    assert(identity[0x19] == 0x32 && identity[0x1C] == 0xAA);
    assert(identity[0x1F] == 0x9B && identity[0x20] == 0xE1 && identity[0x21] == 0xE6);
    assert(identity[0x22] == 0x32 && identity[0x23] == 0x32 && identity[0x24] == 0x32);

    const uint8_t right_custom[3] = {1, 2, 3};
    ns2_joycon2_build_identity(true, right_custom, identity);
    assert(identity[2] == 'H' && identity[3] == 'C');
    assert(identity[20] == 0x66 && identity[21] == 0x20);
    assert(identity[0x1F] == 1 && identity[0x20] == 2 && identity[0x21] == 3);

    const uint8_t unit_id[6] = {0x02, 0xBB, 0x5E, 0xAB, 0xA9, 0x3C};
    uint8_t ep0[NS2_JOYCON2_EP0_INFO_LEN];
    uint8_t command[NS2_JOYCON2_COMMAND_INFO_LEN];
    ns2_joycon2_build_ep0_info(unit_id, ep0);
    ns2_joycon2_build_command_info(false, command);
    assert(ep0[0] == 2 && ep0[1] == 1 && ep0[2] == 4);
    assert(ep0[6] == 12);
    assert(memcmp(&ep0[10], unit_id, sizeof(unit_id)) == 0);
    assert(memcmp(ep0, command, 3) == 0);
    assert(command[3] == 0);
    assert(command[4] == 12 && command[5] == 0 && command[6] == 0);
    assert(command[8] == 0 && command[9] == 0 && command[10] == 0);

    ns2_joycon2_build_command_info(true, command);
    assert(command[3] == 1);

    puts("ns2_joycon2_identity: all tests passed");
    return 0;
}
