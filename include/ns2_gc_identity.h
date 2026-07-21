#ifndef NS2_GC_IDENTITY_H
#define NS2_GC_IDENTITY_H

#include <stdint.h>

#define NS2_GC_EP0_INFO_LEN 16
#define NS2_GC_COMMAND_INFO_LEN 12

// Build both version-bearing surfaces from the same live-hardware-confirmed
// NSO GameCube identity: controller 1.1.2, type 3, Bluetooth 12.0.0, and no
// DSP firmware (encoded as FF FF FF FF in the native command reply).
void ns2_gc_build_ep0_info(const uint8_t unit_id[6],
                           uint8_t out[NS2_GC_EP0_INFO_LEN]);
void ns2_gc_build_command_info(uint8_t out[NS2_GC_COMMAND_INFO_LEN]);

#endif  // NS2_GC_IDENTITY_H
