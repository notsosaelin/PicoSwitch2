// Bonded BLE reconnect candidate selection.
//
// WHY THIS EXISTS
//
// The BLE host stores exactly one reconnect identity (`hid_state.last_connected_*`),
// a single-peer-era abstraction. Every reconnect site targeted that slot
// unconditionally, which is wrong as soon as two bonded peers can be live at
// once (Keyboard + Mouse is one logical source built from two BLE peers):
//
//   * `last_connected` names whichever peer connected MOST RECENTLY, which is
//     not necessarily the one that went away. When the keyboard connects second
//     and the mouse then powers off, the disconnect handler fired a reconnect
//     at the keyboard -- a peer that is still connected.
//   * `btstack_host_connect_ble()` calls `btstack_host_stop_scan()` at the top
//     of every attempt, and the failure path retries up to 5 times. So targeting
//     an already-connected peer does not merely waste an attempt: it tears down
//     the very scan windows in which the absent peer's advertisements would have
//     been seen. The absent peer can then never be found.
//
// BTstack's LE device DB already stores per-peer bonds (capacity 16), so the
// list of known identities exists; only the scheduler was single-slot. This
// module turns that list plus liveness into a decision. It is deliberately pure
// (no BTstack, no Pico SDK) so the policy is unit-testable on the host.
//
// The selector never returns an identity that is already connected, which is
// the invariant that fixes the failure above.
#ifndef NS2_BLE_RECONNECT_H
#define NS2_BLE_RECONNECT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Matches MAX_NR_LE_DEVICE_DB_ENTRIES; the selector is bounded by the caller's
// count regardless.
#define NS2_BLE_RECONNECT_MAX_CANDIDATES 16

// How many direct-connect attempts a preferred candidate gets before the policy
// stops chasing it and falls back to discovery. Mirrors the host's existing
// 5-attempt reconnect cascade bound.
#define NS2_BLE_RECONNECT_DIRECT_ATTEMPT_LIMIT 5

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    // A live link to this identity exists right now. Such a candidate is never
    // selected -- see the header comment.
    uint8_t connected;
    // This identity matches the stored `last_connected` record, so the host
    // holds its name/profile/VID/PID and can direct-connect it. Candidates
    // without that metadata must be reached via discovery instead, because a
    // nameless BLE link would defeat name-driven quirk identification.
    uint8_t preferred;
} ns2_ble_reconnect_candidate_t;

typedef enum {
    // Nothing bonded needs attention: no bonds, or every bonded identity is
    // already connected. Do not issue a connect request.
    NS2_BLE_RECONNECT_IDLE = 0,
    // Direct gap_connect to the returned address. Only ever a preferred
    // candidate, so the caller has the metadata the link needs.
    NS2_BLE_RECONNECT_DIRECT,
    // Bonded identities are absent but none can be direct-connected. Keep
    // discovery running: the advertising path carries the peer's name and
    // profile, which a direct connect to an unknown identity would lack.
    NS2_BLE_RECONNECT_SCAN,
} ns2_ble_reconnect_action_t;

typedef struct {
    ns2_ble_reconnect_action_t action;
    uint8_t addr[6];       // valid when action == NS2_BLE_RECONNECT_DIRECT
    uint8_t addr_type;     // valid when action == NS2_BLE_RECONNECT_DIRECT
} ns2_ble_reconnect_decision_t;

// Choose what the host should do about bonded peers that are not connected.
//
// `attempts` is how many consecutive direct attempts the preferred candidate has
// already consumed; past NS2_BLE_RECONNECT_DIRECT_ATTEMPT_LIMIT the policy stops
// hammering it and yields SCAN so other absent peers still get a chance.
//
// Deterministic: the same inputs always produce the same decision.
ns2_ble_reconnect_decision_t ns2_ble_reconnect_select(
    const ns2_ble_reconnect_candidate_t *candidates,
    uint8_t count,
    uint32_t attempts);

#ifdef __cplusplus
}
#endif

#endif // NS2_BLE_RECONNECT_H
