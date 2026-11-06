// bthid_identity.h - pure helpers for provisional Bluetooth identity matching

#ifndef BTHID_IDENTITY_H
#define BTHID_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Name matching is useful before SDP/DIS resolves a device's identity, but a
// late authoritative VID/PID must be allowed to disprove that provisional
// match. An empty expected_pid list means the driver supports the whole vendor
// family; otherwise a known PID must be present in the supplied list.
bool bthid_name_fallback_allowed(uint16_t observed_vid,
                                 uint16_t observed_pid,
                                 uint16_t expected_vid,
                                 const uint16_t *expected_pids,
                                 size_t expected_pid_count);

#endif // BTHID_IDENTITY_H
