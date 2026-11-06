#include "mgmt_pairing.h"

#include <stdio.h>
#include <string.h>

bool mgmt_pairing_parse_command(const char *arg, mgmt_pairing_action_t *action)
{
    if (!arg || !action)
        return false;
    *action = MGMT_PAIRING_INVALID;
    if (strcmp(arg, "start") == 0) {
        *action = MGMT_PAIRING_START;
        return true;
    }
    if (strcmp(arg, "status") == 0) {
        *action = MGMT_PAIRING_STATUS;
        return true;
    }
    if (strcmp(arg, "cancel") == 0) {
        *action = MGMT_PAIRING_CANCEL;
        return true;
    }
    return false;
}

const char *mgmt_pairing_state_name(mgmt_pairing_state_t state)
{
    switch (state) {
    case MGMT_PAIRING_DISCOVERING:
        return "discovering";
    case MGMT_PAIRING_CONNECTING:
        return "connecting";
    case MGMT_PAIRING_PAIRED:
        return "paired";
    case MGMT_PAIRING_TIMED_OUT:
        return "timed_out";
    case MGMT_PAIRING_CANCELLED:
        return "cancelled";
    case MGMT_PAIRING_BLOCKED:
        return "blocked";
    case MGMT_PAIRING_IDLE:
    default:
        return "idle";
    }
}

const char *mgmt_pairing_reason_name(mgmt_pairing_reason_t reason)
{
    switch (reason) {
    case MGMT_PAIRING_REASON_NO_CONTROLLER:
        return "no_controller";
    case MGMT_PAIRING_REASON_MANAGEMENT_DISABLED:
        return "management_disabled";
    case MGMT_PAIRING_REASON_BUSY:
        return "busy";
    case MGMT_PAIRING_REASON_LOCKED_OUT:
        return "locked_out";
    case MGMT_PAIRING_REASON_STORAGE_FULL:
        return "storage_full";
    case MGMT_PAIRING_REASON_NONE:
    default:
        return "none";
    }
}

bool mgmt_pairing_start_allowed(bool management_enabled,
                                bool already_active,
                                bool pairing_locked_out,
                                bool security_storage_full,
                                mgmt_pairing_reason_t *reason)
{
    mgmt_pairing_reason_t why = MGMT_PAIRING_REASON_NONE;
    bool allowed = false;

    // Ordered most-fundamental first, so the reason a client is told is the one
    // it can actually act on. "Management disabled" outranks "busy" because a
    // disabled management plane could not have started the busy operation, and
    // "busy" outranks "storage full" because an operation already running is
    // the more immediate thing to resolve.
    if (!management_enabled) {
        why = MGMT_PAIRING_REASON_MANAGEMENT_DISABLED;
    } else if (pairing_locked_out) {
        why = MGMT_PAIRING_REASON_LOCKED_OUT;
    } else if (already_active) {
        why = MGMT_PAIRING_REASON_BUSY;
    } else if (security_storage_full) {
        // Refused rather than started: a window that could only end in a silent
        // eviction is worse than a refusal that names what to do about it.
        why = MGMT_PAIRING_REASON_STORAGE_FULL;
    } else {
        allowed = true;
    }

    if (reason)
        *reason = why;
    return allowed;
}

uint32_t mgmt_pairing_remaining_ms(uint32_t deadline_ms, uint32_t now_ms,
                                   bool active)
{
    if (!active)
        return 0;
    // Wrap-safe: the difference is computed in unsigned arithmetic and its top
    // bit distinguishes "deadline is ahead" from "deadline has passed". A plain
    // `deadline > now` comparison reports ~49 days remaining for the whole tick
    // after the millisecond counter wraps.
    uint32_t delta = deadline_ms - now_ms;
    if (delta == 0u || delta > 0x7FFFFFFFu)
        return 0;
    return delta;
}

size_t mgmt_pairing_format_status(const mgmt_pairing_snapshot_t *snapshot,
                                  char *output, size_t capacity)
{
    if (!output || capacity == 0)
        return 0;
    output[0] = '\0';
    if (!snapshot)
        return 0;

    // `ok` describes the COMMAND, not the pairing outcome: a status read that
    // reports "timed_out" succeeded at reading. Only an explicit refusal is
    // false, so a client can distinguish "I could not ask" from "the answer is
    // that it did not work".
    bool ok = snapshot->state != MGMT_PAIRING_BLOCKED;
    int written = snprintf(
        output, capacity,
        "{\"ok\":%s,\"op\":%lu,\"state\":\"%s\",\"reason\":\"%s\","
        "\"remaining_ms\":%lu,\"candidates\":%u}",
        ok ? "true" : "false",
        (unsigned long)snapshot->operation,
        mgmt_pairing_state_name((mgmt_pairing_state_t)snapshot->state),
        mgmt_pairing_reason_name((mgmt_pairing_reason_t)snapshot->reason),
        (unsigned long)snapshot->remaining_ms,
        (unsigned)snapshot->candidates);
    if (written < 0 || (size_t)written >= capacity) {
        output[0] = '\0';
        return 0;
    }
    return (size_t)written;
}
