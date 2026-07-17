#ifndef NS2_JOYCON2_IDENTITY_H
#define NS2_JOYCON2_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#define NS2_JOYCON2_IDENTITY_LEN 64

// Build the factory/EP0 identity block for one Joy-Con 2 side. The caller
// supplies the configurable highlight/accent RGB; all other bytes retain their
// evidence-backed per-side values.
void ns2_joycon2_build_identity(bool right, const uint8_t accent[3],
                                uint8_t out[NS2_JOYCON2_IDENTITY_LEN]);

#endif  // NS2_JOYCON2_IDENTITY_H
