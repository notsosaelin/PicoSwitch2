/*
 * Host test for the in-band BLE management ACCESS-CONTROL state machine.
 *
 * The decision functions (mgmt_*) are the canonical contract for the in-band
 * management path: when it may advertise, accept a connection, accept a NEW
 * bond, allow a command write, and when it must drop its client. They were
 * developed test-first and now live in the PRODUCTION module src/mgmt_access.c;
 * this test links the real header (mgmt_access.h) so the exhaustive invariant
 * proof below guards the shipping code directly.
 *
 * Scope: this models the `enabled` path only. The legacy USB-CDC Config path
 * (default-off, deprecated) keeps its existing, separately-tested behavior and
 * is deliberately NOT modeled here -- mixing the two made the invariants fuzzy.
 * Everything below is keyed on a single `enabled` gate.
 *
 * The write decision links config_wireless_command_allowed() (the real wireless
 * allowlist), so it is tested against the REAL security boundary.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude -Itools \
 *   tools/test_mgmt_access.c src/mgmt_access.c src/config_wireless_bridge.c \
 *   -o build/host-tests/test_mgmt_access.exe
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config_wireless_bridge.h"  // config_wireless_command_allowed (real)
#include "mgmt_access.h"             // the production access-control decisions

// ---------------------------------------------------------------------------
// Hand-picked scenario tests (readable intent)
// ---------------------------------------------------------------------------
static const mgmt_state_t IDLE_ENABLED = {
    .enabled = true, .console_awake = true, .wake_active = false,
    .scanning = false, .pairing_window_open = false,
    .client_connected = false, .client_bonded = false,
    .client_encrypted = false,
};

static void test_disabled_is_invisible(void) {
    mgmt_state_t s = IDLE_ENABLED; s.enabled = false;
    assert(!mgmt_should_advertise(&s));
    assert(!mgmt_accept_connection(&s));
    assert(!mgmt_accept_bonding(&s));                 // even with a window (below)
    s.pairing_window_open = true;
    assert(!mgmt_accept_bonding(&s));                 // window for a CONTROLLER, mgmt off
    assert(!mgmt_allow_write(&s, "amiibo status"));
    s.client_connected = true;
    assert(mgmt_should_drop_client(&s));
}

static void test_advertises_only_when_safe(void) {
    assert(mgmt_should_advertise(&IDLE_ENABLED));
    mgmt_state_t s;
    s = IDLE_ENABLED; s.wake_active = true;       assert(!mgmt_should_advertise(&s));
    s = IDLE_ENABLED; s.console_awake = false;    assert(!mgmt_should_advertise(&s));
    // Controller discovery and the peripheral-role advertiser are deliberately
    // concurrent. Gating one on the other caused the hardware-observed
    // management/controller reconnect starvation fixed on 2026-08-13.
    s = IDLE_ENABLED; s.scanning = true;          assert(mgmt_should_advertise(&s));
    s = IDLE_ENABLED; s.client_connected = true;  assert(!mgmt_should_advertise(&s));
}

static void test_wake_is_never_broken(void) {
    mgmt_state_t s = IDLE_ENABLED; s.client_connected = true;
    assert(!mgmt_should_drop_client(&s));
    s.console_awake = false; assert(mgmt_should_drop_client(&s));
    s = IDLE_ENABLED; s.client_connected = true; s.wake_active = true;
    assert(mgmt_should_drop_client(&s));
}

static void test_bond_only_in_window_when_enabled(void) {
    mgmt_state_t s = IDLE_ENABLED;
    assert(!mgmt_accept_bonding(&s));                 // no window
    s.pairing_window_open = true;
    assert(mgmt_accept_bonding(&s));                  // enabled + window
    // Already-bonded reconnect: a connection is still accepted outside a window;
    // it just cannot form a NEW bond.
    s = IDLE_ENABLED; assert(mgmt_accept_connection(&s));
    assert(!mgmt_accept_bonding(&s));
}

static void test_writes_require_bond_and_allowlist(void) {
    mgmt_state_t s = IDLE_ENABLED;
    s.client_connected = true;
    s.client_bonded = true;
    s.client_encrypted = true;
    assert(mgmt_allow_write(&s, "amiibo select save1"));
    assert(mgmt_allow_write(&s, "save"));
    assert(!mgmt_allow_write(&s, "imu"));             // diagnostic: allowlist rejects
    assert(!mgmt_allow_write(&s, "state"));
    s.client_encrypted = false;                       // stale bond, plaintext link
    assert(!mgmt_allow_write(&s, "save"));
    s.client_encrypted = true;
    s.client_bonded = false;                          // connected but not bonded
    assert(!mgmt_allow_write(&s, "amiibo select save1"));  // anti-hijack
    s.client_bonded = true; s.enabled = false;
    assert(!mgmt_allow_write(&s, "save"));            // disabled overrides
}

static void test_single_client(void) {
    mgmt_state_t s = IDLE_ENABLED; s.client_connected = true;
    assert(!mgmt_accept_connection(&s));
}

static void test_link_trust_requires_bond_and_active_ble_encryption(void) {
    assert(!mgmt_link_is_trusted(false, 0));
    assert(!mgmt_link_is_trusted(true, 0));
    assert(!mgmt_link_is_trusted(false, 16));
    assert(!mgmt_link_is_trusted(true, 6));
    assert(!mgmt_link_is_trusted(true, 7));
    assert(!mgmt_link_is_trusted(true, 15));
    assert(mgmt_link_is_trusted(true, 16));
    assert(!mgmt_link_is_trusted(true, 17));
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE invariant proof: enumerate all 2^8 states and assert the
// cross-cutting safety properties hold in EVERY combination. This is the
// "weirdest case a user can reach" guard -- no state, however odd, may violate
// wake priority, the bond gate, or the write gate.
// ---------------------------------------------------------------------------
static void test_invariants_exhaustive(void) {
    const char *allowed = "amiibo present";     // in the allowlist
    const char *denied  = "imu";                // not in the allowlist
    assert(config_wireless_command_allowed(allowed));
    assert(config_wireless_command_allowed("input active 1"));
    assert(!config_wireless_command_allowed(denied));

    unsigned checked = 0;
    for (unsigned bits = 0; bits < (1u << 8); ++bits) {
        mgmt_state_t s = {
            .enabled             = (bits >> 0) & 1u,
            .console_awake       = (bits >> 1) & 1u,
            .wake_active         = (bits >> 2) & 1u,
            .scanning            = (bits >> 3) & 1u,
            .pairing_window_open = (bits >> 4) & 1u,
            .client_connected    = (bits >> 5) & 1u,
            .client_bonded       = (bits >> 6) & 1u,
            .client_encrypted    = (bits >> 7) & 1u,
        };
        bool adv  = mgmt_should_advertise(&s);
        bool acc  = mgmt_accept_connection(&s);
        bool bond = mgmt_accept_bonding(&s);
        bool wA   = mgmt_allow_write(&s, allowed);
        bool wD   = mgmt_allow_write(&s, denied);
        bool drop = mgmt_should_drop_client(&s);

        // INV1 disabled => fully inert (and any live client is dropped).
        if (!s.enabled) {
            assert(!adv && !acc && !bond && !wA && !wD);
            if (s.client_connected) assert(drop);
        }
        // INV2 wake priority => never advertise; drop any client (never break wake).
        if (s.wake_active) {
            assert(!adv);
            if (s.client_connected) assert(drop);
        }
        // INV3 asleep => silent + no accept; drop any client.
        if (!s.console_awake) {
            assert(!adv && !acc);
            if (s.client_connected) assert(drop);
        }
        // INV4 a write implies enabled + connected + bonded + encrypted + allowlisted.
        if (wA) assert(s.enabled && s.client_connected && s.client_bonded &&
                       s.client_encrypted);
        assert(!wD);  // a denied command is NEVER writable in ANY state.
        // INV5 a bond implies enabled + window.
        if (bond) assert(s.enabled && s.pairing_window_open);
        // INV6 advertising implies single client + all safe conditions.
        if (adv) {
            assert(!s.client_connected);
            assert(s.enabled && s.console_awake && !s.wake_active);
        }
        // INV7 accept-connection implies enabled + awake + no client.
        if (acc) assert(s.enabled && s.console_awake && !s.client_connected);
        // INV8 mutual exclusion: never advertise while a client is connected.
        assert(!(adv && s.client_connected));
        // INV9 an unbonded connected client can never write (anti-hijack), even
        //      inside a pairing window.
        if (s.client_connected && !s.client_bonded) assert(!wA);
        // INV10 a bonded but unencrypted current link is not authorized.
        if (s.client_connected && !s.client_encrypted) assert(!wA);
        checked++;
    }
    assert(checked == 256u);
    printf("  exhaustive: %u/256 states satisfy all 10 invariants\n", checked);
}

int main(void) {
    test_disabled_is_invisible();
    test_advertises_only_when_safe();
    test_wake_is_never_broken();
    test_bond_only_in_window_when_enabled();
    test_writes_require_bond_and_allowlist();
    test_single_client();
    test_link_trust_requires_bond_and_active_ble_encryption();
    test_invariants_exhaustive();
    puts("mgmt access-control tests passed");
    return 0;
}
