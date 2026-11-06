// bthid_identity.c - pure helpers for provisional Bluetooth identity matching

#include "bthid_identity.h"

bool bthid_name_fallback_allowed(uint16_t observed_vid,
                                 uint16_t observed_pid,
                                 uint16_t expected_vid,
                                 const uint16_t *expected_pids,
                                 size_t expected_pid_count)
{
    // No VID yet: the asynchronous identity query has not supplied evidence
    // capable of disproving the early name match.
    if (observed_vid == 0) {
        return true;
    }

    if (observed_vid != expected_vid) {
        return false;
    }

    // Some drivers intentionally support every controller from one vendor.
    if (expected_pid_count == 0 || observed_pid == 0) {
        return true;
    }

    if (!expected_pids) {
        return false;
    }

    for (size_t i = 0; i < expected_pid_count; i++) {
        if (observed_pid == expected_pids[i]) {
            return true;
        }
    }

    return false;
}
