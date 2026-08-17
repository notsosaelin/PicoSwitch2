/*
 * Bonded BLE reconnect candidate selection.
 *
 * Regression coverage for a confirmed multi-peer defect. Every reconnect site in
 * the BLE host targeted `hid_state.last_connected` unconditionally. That slot
 * holds whichever peer connected MOST RECENTLY, not the one that went away, so
 * with a Keyboard + Mouse source (two bonded BLE peers, one logical source) the
 * host reconnected at a peer that was still connected. Worse, every attempt
 * calls btstack_host_stop_scan(), so the retry cascade destroyed the scan
 * windows in which the departed peer's advertisements would have been seen --
 * the absent peer could then never be found, in either direction.
 *
 * The invariant that fixes it, and the one these tests pin: a candidate that is
 * already connected is NEVER selected.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ns2_ble_reconnect.h"

static const uint8_t ADDR_A[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t ADDR_B[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

static ns2_ble_reconnect_candidate_t make(const uint8_t addr[6], uint8_t type,
                                          bool connected, bool preferred) {
    ns2_ble_reconnect_candidate_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.addr, addr, sizeof(c.addr));
    c.addr_type = type;
    c.connected = connected ? 1u : 0u;
    c.preferred = preferred ? 1u : 0u;
    return c;
}

static bool addr_is(const ns2_ble_reconnect_decision_t *d, const uint8_t addr[6]) {
    return memcmp(d->addr, addr, 6) == 0;
}

// Both bonded peers live: nothing to reconnect. This is the steady state, and
// issuing a connect here is exactly what tore down scan windows.
static void test_both_live(void) {
    ns2_ble_reconnect_candidate_t c[2] = {
        make(ADDR_A, 1, true, true),
        make(ADDR_B, 1, true, false),
    };
    ns2_ble_reconnect_decision_t d = ns2_ble_reconnect_select(c, 2, 0, false);
    assert(d.action == NS2_BLE_RECONNECT_IDLE);
    printf("OK:   both bonded peers live -> no reconnect request\n");
}

// A live and preferred, B absent. The defect: A owns `last_connected`, so the
// old code targeted A -- a connected peer -- and never looked for B.
static void test_a_live_preferred_b_absent(void) {
    ns2_ble_reconnect_candidate_t c[2] = {
        make(ADDR_A, 1, true, true),
        make(ADDR_B, 1, false, false),
    };
    ns2_ble_reconnect_decision_t d = ns2_ble_reconnect_select(c, 2, 0, false);
    assert(d.action == NS2_BLE_RECONNECT_SCAN);
    printf("OK:   live preferred peer is never targeted; absent peer yields SCAN\n");
}

// Symmetric direction: B live, A absent and preferred -> direct-connect A.
static void test_b_live_a_absent_preferred(void) {
    ns2_ble_reconnect_candidate_t c[2] = {
        make(ADDR_A, 1, false, true),
        make(ADDR_B, 1, true, false),
    };
    ns2_ble_reconnect_decision_t d = ns2_ble_reconnect_select(c, 2, 0, false);
    assert(d.action == NS2_BLE_RECONNECT_DIRECT);
    assert(addr_is(&d, ADDR_A));
    printf("OK:   absent preferred peer is direct-connected while partner stays live\n");
}

// Neither live: deterministic and bounded. The preferred one is direct-connected;
// the other is left to discovery, which supplies the name/profile a bare bonded
// address does not carry.
static void test_neither_live(void) {
    ns2_ble_reconnect_candidate_t c[2] = {
        make(ADDR_A, 1, false, true),
        make(ADDR_B, 1, false, false),
    };
    ns2_ble_reconnect_decision_t first = ns2_ble_reconnect_select(c, 2, 0, false);
    assert(first.action == NS2_BLE_RECONNECT_DIRECT);
    assert(addr_is(&first, ADDR_A));

    // Same inputs must give the same answer -- no hidden internal cursor.
    ns2_ble_reconnect_decision_t again = ns2_ble_reconnect_select(c, 2, 0, false);
    assert(again.action == first.action);
    assert(addr_is(&again, ADDR_A));

    // Once A connects, B is the only absent one and has no stored metadata.
    c[0].connected = 1u;
    ns2_ble_reconnect_decision_t then = ns2_ble_reconnect_select(c, 2, 0, false);
    assert(then.action == NS2_BLE_RECONNECT_SCAN);
    printf("OK:   neither live -> deterministic bounded policy, then SCAN for the rest\n");
}

// The preferred peer must not monopolise the reconnect path forever.
static void test_direct_attempts_bounded(void) {
    ns2_ble_reconnect_candidate_t c[2] = {
        make(ADDR_A, 1, false, true),
        make(ADDR_B, 1, false, false),
    };
    ns2_ble_reconnect_decision_t under = ns2_ble_reconnect_select(
        c, 2, NS2_BLE_RECONNECT_DIRECT_ATTEMPT_LIMIT - 1u, false);
    assert(under.action == NS2_BLE_RECONNECT_DIRECT);

    ns2_ble_reconnect_decision_t at = ns2_ble_reconnect_select(
        c, 2, NS2_BLE_RECONNECT_DIRECT_ATTEMPT_LIMIT, false);
    assert(at.action == NS2_BLE_RECONNECT_SCAN);

    ns2_ble_reconnect_decision_t over = ns2_ble_reconnect_select(c, 2, 999u, false);
    assert(over.action == NS2_BLE_RECONNECT_SCAN);
    printf("OK:   direct attempts are bounded, then fall back to discovery\n");
}

// A removed bond is simply absent from the candidate list, so it can never be
// selected. Pins that removal is honoured by construction.
static void test_removed_bond_excluded(void) {
    ns2_ble_reconnect_candidate_t only = make(ADDR_B, 1, true, false);
    ns2_ble_reconnect_decision_t d = ns2_ble_reconnect_select(&only, 1, 0, false);
    assert(d.action == NS2_BLE_RECONNECT_IDLE);

    // And with no bonds at all there is nothing to chase.
    ns2_ble_reconnect_decision_t none = ns2_ble_reconnect_select(&only, 0, 0, false);
    assert(none.action == NS2_BLE_RECONNECT_IDLE);
    printf("OK:   removed bond is not a candidate; empty bond DB yields IDLE\n");
}

// Legacy single-controller behaviour must be untouched: one bond, absent,
// preferred -> direct connect, exactly as before the multi-peer change.
static void test_legacy_single_controller(void) {
    ns2_ble_reconnect_candidate_t one = make(ADDR_A, 0, false, true);
    ns2_ble_reconnect_decision_t d = ns2_ble_reconnect_select(&one, 1, 0, false);
    assert(d.action == NS2_BLE_RECONNECT_DIRECT);
    assert(addr_is(&d, ADDR_A));
    assert(d.addr_type == 0);

    // And when that single controller is connected, no request is issued.
    one.connected = 1u;
    ns2_ble_reconnect_decision_t live = ns2_ble_reconnect_select(&one, 1, 0, false);
    assert(live.action == NS2_BLE_RECONNECT_IDLE);
    printf("OK:   legacy single-controller reconnect behaviour preserved\n");
}

// A bonded peer with no preferred flag anywhere (e.g. last_connected was never
// restored) must still be pursued -- via discovery, not a nameless connect.
static void test_absent_without_preferred(void) {
    ns2_ble_reconnect_candidate_t c[2] = {
        make(ADDR_A, 1, false, false),
        make(ADDR_B, 1, false, false),
    };
    ns2_ble_reconnect_decision_t d = ns2_ble_reconnect_select(c, 2, 0, false);
    assert(d.action == NS2_BLE_RECONNECT_SCAN);
    printf("OK:   absent peers without stored metadata are reached by discovery\n");
}

// The selected address must be exactly the candidate's, never a partner's --
// the precise failure mode that stranded the departed peer.
static void test_never_returns_connected_address(void) {
    ns2_ble_reconnect_candidate_t c[2] = {
        make(ADDR_A, 1, true, true),
        make(ADDR_B, 1, false, true),   // both flagged preferred: pathological
    };
    ns2_ble_reconnect_decision_t d = ns2_ble_reconnect_select(c, 2, 0, false);
    assert(d.action == NS2_BLE_RECONNECT_DIRECT);
    assert(addr_is(&d, ADDR_B));        // the ABSENT one, not the live one
    assert(!addr_is(&d, ADDR_A));
    printf("OK:   a connected identity is never returned, even if flagged preferred\n");
}

// The LE device DB is shared by every LE bond, including the bonded
// management/companion client (peripheral role). Such an identity is never
// `preferred`, because last_connected is only ever written from the central-role
// connection table. Pin that no arrangement of non-preferred candidates can
// produce a DIRECT connect: unrelated bonds must never be dialled.
static void test_non_preferred_never_direct(void) {
    for (unsigned mask = 0; mask < 4u; mask++) {
        for (uint32_t attempts = 0; attempts < 8u; attempts++) {
            ns2_ble_reconnect_candidate_t c[2] = {
                make(ADDR_A, 1, (mask & 1u) != 0u, false),
                make(ADDR_B, 1, (mask & 2u) != 0u, false),
            };
            ns2_ble_reconnect_decision_t d = ns2_ble_reconnect_select(c, 2, attempts, false);
            assert(d.action != NS2_BLE_RECONNECT_DIRECT);
        }
    }
    printf("OK:   no arrangement of non-preferred bonds can yield a direct connect\n");
}

// An open pairing window outranks a speculative direct connect.
//
// This is the regression the bounded completion window exposed. Sequence: a
// partial KB/M source settles, so discovery idles and btstack_host_stop_scan()
// clears hid_state.scan_start_time. The user then opens pairing, which calls
// btstack_host_start_scan(); because scan_start_time is 0 and a bonded target
// exists, the host takes its "first scan with a bonded device" fast path and
// backdates scan_start_time so the periodic reconnect becomes eligible ~3 s in.
// That reconnect DIRECT-targeted the absent peer, and btstack_host_connect_ble()
// stops the scan for the whole attempt (10 s timeout, then retries) -- while
// nothing re-arms discovery, because the app-layer re-arm is gated on
// `pairing_until_ms == 0`. The user's pairing window was consumed with the radio
// not scanning, so the second peripheral could never be seen.
static void test_pairing_window_outranks_direct(void) {
    // The absent peer is also the stored target: without the guard this is the
    // DIRECT case that tore down the pairing scan.
    ns2_ble_reconnect_candidate_t c[2] = {
        make(ADDR_A, 1, true, false),    // keyboard still live
        make(ADDR_B, 1, false, true),    // mouse absent AND the stored target
    };

    // Outside a pairing window, direct reconnect is unchanged -- peers that stop
    // advertising after bonding still get dialled.
    ns2_ble_reconnect_decision_t background = ns2_ble_reconnect_select(c, 2, 0, false);
    assert(background.action == NS2_BLE_RECONNECT_DIRECT);
    assert(addr_is(&background, ADDR_B));

    // With the window open, discovery wins: never DIRECT, at any attempt count.
    for (uint32_t attempts = 0; attempts < 8u; attempts++) {
        ns2_ble_reconnect_decision_t paired = ns2_ble_reconnect_select(c, 2, attempts, true);
        assert(paired.action == NS2_BLE_RECONNECT_SCAN);
    }

    // Mirrored: mouse live, keyboard absent and stored.
    ns2_ble_reconnect_candidate_t m[2] = {
        make(ADDR_A, 1, false, true),    // keyboard absent AND stored target
        make(ADDR_B, 1, true, false),    // mouse still live
    };
    assert(ns2_ble_reconnect_select(m, 2, 0, false).action == NS2_BLE_RECONNECT_DIRECT);
    assert(ns2_ble_reconnect_select(m, 2, 0, true).action == NS2_BLE_RECONNECT_SCAN);

    // A complete pair still issues nothing, window or not.
    ns2_ble_reconnect_candidate_t both[2] = {
        make(ADDR_A, 1, true, true),
        make(ADDR_B, 1, true, false),
    };
    assert(ns2_ble_reconnect_select(both, 2, 0, true).action == NS2_BLE_RECONNECT_IDLE);
    printf("OK:   an open pairing window never yields DIRECT, so its scan survives\n");
}

// Defensive: NULL list and oversized counts must not read out of bounds.
static void test_bounds_and_null(void) {
    ns2_ble_reconnect_decision_t d = ns2_ble_reconnect_select(NULL, 4, 0, false);
    assert(d.action == NS2_BLE_RECONNECT_IDLE);

    ns2_ble_reconnect_candidate_t c[NS2_BLE_RECONNECT_MAX_CANDIDATES];
    for (uint8_t i = 0; i < NS2_BLE_RECONNECT_MAX_CANDIDATES; i++)
        c[i] = make(ADDR_B, 1, true, false);
    // Claim more entries than the array holds; the selector must clamp.
    ns2_ble_reconnect_decision_t clamped = ns2_ble_reconnect_select(c, 255u, 0, false);
    assert(clamped.action == NS2_BLE_RECONNECT_IDLE);
    printf("OK:   NULL list and oversized count are handled safely\n");
}

int main(void) {
    test_both_live();
    test_a_live_preferred_b_absent();
    test_b_live_a_absent_preferred();
    test_neither_live();
    test_direct_attempts_bounded();
    test_removed_bond_excluded();
    test_legacy_single_controller();
    test_absent_without_preferred();
    test_never_returns_connected_address();
    test_non_preferred_never_direct();
    test_pairing_window_outranks_direct();
    test_bounds_and_null();
    printf("ns2_ble_reconnect: all tests passed\n");
    return 0;
}
