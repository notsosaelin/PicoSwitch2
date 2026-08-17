#include "ns2_ble_reconnect.h"

#include <string.h>

ns2_ble_reconnect_decision_t ns2_ble_reconnect_select(
    const ns2_ble_reconnect_candidate_t *candidates,
    uint8_t count,
    uint32_t attempts)
{
    ns2_ble_reconnect_decision_t decision;
    memset(&decision, 0, sizeof(decision));
    decision.action = NS2_BLE_RECONNECT_IDLE;

    if (!candidates || count == 0)
        return decision;
    if (count > NS2_BLE_RECONNECT_MAX_CANDIDATES)
        count = NS2_BLE_RECONNECT_MAX_CANDIDATES;

    // Pass 1: is there anything to do at all? A bonded identity that already has
    // a live link is not a reconnect candidate -- targeting it is what tore down
    // the scan windows and stranded the peer that had actually gone away.
    const ns2_ble_reconnect_candidate_t *preferred = 0;
    bool any_absent = false;
    for (uint8_t i = 0; i < count; i++) {
        if (candidates[i].connected)
            continue;
        any_absent = true;
        // First absent preferred candidate wins; ties cannot occur in practice
        // because `last_connected` holds a single address, and taking the first
        // keeps the decision deterministic if a duplicate ever appears.
        if (candidates[i].preferred && !preferred)
            preferred = &candidates[i];
    }

    if (!any_absent)
        return decision;   // every bonded peer is live

    // Pass 2: direct-connect the preferred identity while it still has attempts
    // left. Beyond the bound, fall through to discovery so a stuck preferred
    // peer cannot monopolise the reconnect path and starve the others.
    if (preferred && attempts < NS2_BLE_RECONNECT_DIRECT_ATTEMPT_LIMIT) {
        decision.action = NS2_BLE_RECONNECT_DIRECT;
        memcpy(decision.addr, preferred->addr, sizeof(decision.addr));
        decision.addr_type = preferred->addr_type;
        return decision;
    }

    // Absent identities exist but none is directly targetable. Discovery is the
    // correct tool: the advertising report supplies the name and profile that a
    // blind connect to a bare bonded address would not have.
    decision.action = NS2_BLE_RECONNECT_SCAN;
    return decision;
}
