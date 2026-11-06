#include "ns2_gc_identity.h"

#include <string.h>

enum {
    NS2_GC_FW_MAJOR = 1,
    NS2_GC_FW_MINOR = 1,
    NS2_GC_FW_MICRO = 2,
    NS2_GC_TYPE = 3,
    NS2_GC_BT_MAJOR = 12,
};

void ns2_gc_build_ep0_info(const uint8_t unit_id[6],
                           uint8_t out[NS2_GC_EP0_INFO_LEN]) {
    memset(out, 0, NS2_GC_EP0_INFO_LEN);
    out[0] = NS2_GC_FW_MAJOR;
    out[1] = NS2_GC_FW_MINOR;
    out[2] = NS2_GC_FW_MICRO;
    out[6] = NS2_GC_BT_MAJOR;
    memcpy(&out[10], unit_id, 6);
}

void ns2_gc_build_command_info(uint8_t out[NS2_GC_COMMAND_INFO_LEN]) {
    memset(out, 0, NS2_GC_COMMAND_INFO_LEN);
    out[0] = NS2_GC_FW_MAJOR;
    out[1] = NS2_GC_FW_MINOR;
    out[2] = NS2_GC_FW_MICRO;
    out[3] = NS2_GC_TYPE;
    out[4] = NS2_GC_BT_MAJOR;
    memset(&out[8], 0xFF, 4);
}
