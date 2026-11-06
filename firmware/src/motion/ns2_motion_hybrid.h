#ifndef NS2_MOTION_HYBRID_H
#define NS2_MOTION_HYBRID_H

#include <stdbool.h>
#include <stdint.h>

// Diagnostic-only semantic groups for exact genuine/donor motion splicing.
// Values are shared by length-0x1E and mode-3 length-0x28 where meaningful.
#define NS2_MOTION_HYBRID_TIMING          (1u << 0)
#define NS2_MOTION_HYBRID_STATUS          (1u << 1)
#define NS2_MOTION_HYBRID_PACKING         (1u << 2)
#define NS2_MOTION_HYBRID_PREFIX          (1u << 3)
#define NS2_MOTION_HYBRID_ACCEL           (1u << 4)
#define NS2_MOTION_HYBRID_GYRO            (1u << 5)
#define NS2_MOTION_HYBRID_TAIL            (1u << 6)
#define NS2_MOTION_HYBRID_TEMPERATURE     (1u << 7)
#define NS2_MOTION_HYBRID_FLAGS_RESERVED  (1u << 8)

typedef enum {
    NS2_MOTION_HYBRID_OK = 0,
    NS2_MOTION_HYBRID_BAD_ARGUMENT,
    NS2_MOTION_HYBRID_BAD_LENGTH,
    NS2_MOTION_HYBRID_BAD_MODE,
    NS2_MOTION_HYBRID_LAYOUT_MISMATCH,
    NS2_MOTION_HYBRID_STATUS_MISMATCH,
    NS2_MOTION_HYBRID_BAD_GROUP,
    NS2_MOTION_HYBRID_OUTPUT_INVALID,
} ns2_motion_hybrid_result_t;

// Return the complete semantic partition supported for this packet. A zero
// result means the packet is not spliceable (for example a non-mode-3 0x28).
uint32_t ns2_motion_hybrid_available_groups(const uint8_t *pdu,
                                            uint8_t length);

// Copy only selected semantic groups from `donor` into `base`. Both inputs
// must have the same length and, for 0x28, the same mode-3 cadence layout and
// status. On any failure `out` is untouched. This proves structural fit only;
// live source epoch/pose alignment is a separate mandatory gate.
ns2_motion_hybrid_result_t ns2_motion_hybrid_splice(
    const uint8_t *base, const uint8_t *donor, uint8_t length,
    uint32_t groups, uint8_t *out);

const char *ns2_motion_hybrid_result_name(ns2_motion_hybrid_result_t result);

#endif  // NS2_MOTION_HYBRID_H
