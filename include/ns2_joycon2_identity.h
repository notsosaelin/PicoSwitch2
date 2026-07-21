#ifndef NS2_JOYCON2_IDENTITY_H
#define NS2_JOYCON2_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#define NS2_JOYCON2_IDENTITY_LEN 64
#define NS2_JOYCON2_EP0_INFO_LEN 16
#define NS2_JOYCON2_COMMAND_INFO_LEN 12

// Build the factory/EP0 identity block for one Joy-Con 2 side. The caller
// supplies the configurable highlight/accent RGB; all other bytes retain their
// evidence-backed per-side values.
void ns2_joycon2_build_identity(bool right, const uint8_t accent[3],
                                uint8_t out[NS2_JOYCON2_IDENTITY_LEN]);

// Build the two version-bearing surfaces from the same current Joy-Con 2
// identity. Command type is 0 for Left and 1 for Right; Joy-Cons report no DSP
// firmware as a zero triplet on current firmware.
void ns2_joycon2_build_ep0_info(const uint8_t unit_id[6],
                                uint8_t out[NS2_JOYCON2_EP0_INFO_LEN]);
void ns2_joycon2_build_command_info(bool right,
                                    uint8_t out[NS2_JOYCON2_COMMAND_INFO_LEN]);

#endif  // NS2_JOYCON2_IDENTITY_H
