/*
 * Spec + host test for the in-band BLE management ACCESS-CONTROL state machine.
 *
 * This is TEST-FIRST design for docs/bluetooth/in-band-management-plan.md. The
 * decision functions below (mgmt_*) are the canonical contract: when management
 * may advertise, accept a connection, accept a NEW bond, allow a command write,
 * and when it must drop its client. No production firmware calls them yet. When
 * the feature is implemented, src/mgmt_access.{c,h} lifts these functions
 * verbatim and this test links the real header instead of the local copy.
 *
 * The only production symbol linked today is config_wireless_command_allowed()
 * (the existing wireless allowlist), so the write decision is tested against the
 * REAL security boundary, not a stub.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude -Itools \
 *   tools/test_mgmt_access.c src/config_wireless_bridge.c \
 *   -o build/host-tests/test_mgmt_access.exe
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config_wireless_bridge.h"  // config_wireless_command_allowed (real)

// ---------------------------------------------------------------------------
// The state the decisions read. Every field maps to an existing firmware
// signal (named in the comment) so the production lift is mechanical.
// ---------------------------------------------------------------------------
typedef struct {
    bool enabled;             // g_mgmt_enabled (feature flag, default off)
    bool in_config;           // g_usb_config_mode (legacy CDC config path)
    bool console_awake;       // !tud_suspended()
    bool wake_active;         // wake_adv.active or a pending wake burst
    bool scanning;            // controller scan/inquiry in progress
    bool pairing_window_open; // BOOTSEL double-tap opened the pairing window
    bool client_connected;    // a management LE-peripheral client is linked
    bool client_bonded;       // that client has an established bond (encrypted)
} mgmt_state_t;

// A shorthand: management is "live" when the feature flag OR the legacy config
// personality is active. Everything else is gated behind this.
static bool mgmt_live(const mgmt_state_t *s) {
    return s->enabled || s->in_config;
}

// (1) Advertise connectably only while live, the console is awake, wake does NOT
//     need the radio, no controller scan/inquiry is in flight, and no client is
//     already connected (single client; advertising stops on connect).
static bool mgmt_should_advertise(const mgmt_state_t *s) {
    return mgmt_live(s) && s->console_awake && !s->wake_active &&
           !s->scanning && !s->client_connected;
}

// (2) Accept an incoming management connection only while live, awake, and with
//     no existing client. (Advertising already implies these, but accept is
//     checked defensively at the connection event too.)
static bool mgmt_accept_connection(const mgmt_state_t *s) {
    return mgmt_live(s) && s->console_awake && !s->client_connected;
}

// (3) Accept a NEW bond only inside the deliberate double-tap pairing window.
//     This is the sole first-bond gate: outside the window an unbonded phone may
//     connect but can never establish the bond that write access requires.
static bool mgmt_accept_bonding(const mgmt_state_t *s) {
    return s->pairing_window_open;
}

// (4) Allow a command write only from a connected, BONDED client while live and
//     only for an allowlisted command. Unbonded => never; disabled => never;
//     diagnostic command => never.
static bool mgmt_allow_write(const mgmt_state_t *s, const char *command) {
    return s->client_connected && s->client_bonded && mgmt_live(s) &&
           config_wireless_command_allowed(command);
}

// (5) Drop the management client when it must yield the radio: feature turned
//     off, console went to sleep, or wake needs the advertiser. This is what
//     guarantees wake-from-sleep is never blocked by a lingering mgmt link.
static bool mgmt_should_drop_client(const mgmt_state_t *s) {
    return s->client_connected &&
           (!mgmt_live(s) || !s->console_awake || s->wake_active);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static const mgmt_state_t IDLE_ENABLED = {
    .enabled = true, .in_config = false, .console_awake = true,
    .wake_active = false, .scanning = false, .pairing_window_open = false,
    .client_connected = false, .client_bonded = false,
};

static void test_disabled_is_invisible(void) {
    mgmt_state_t s = IDLE_ENABLED;
    s.enabled = false; s.in_config = false;
    // The whole point of "zero cost when disabled": nothing advertises, accepts,
    // or writes; nothing to drop.
    assert(!mgmt_should_advertise(&s));
    assert(!mgmt_accept_connection(&s));
    assert(!mgmt_allow_write(&s, "amiibo status"));
    s.client_connected = true;               // even if somehow linked,
    assert(mgmt_should_drop_client(&s));      // it is dropped when disabled
}

static void test_advertises_only_when_safe(void) {
    assert(mgmt_should_advertise(&IDLE_ENABLED));            // baseline: yes

    mgmt_state_t s = IDLE_ENABLED; s.wake_active = true;
    assert(!mgmt_should_advertise(&s));                      // wake outranks mgmt

    s = IDLE_ENABLED; s.console_awake = false;
    assert(!mgmt_should_advertise(&s));                      // asleep -> silent

    s = IDLE_ENABLED; s.scanning = true;
    assert(!mgmt_should_advertise(&s));                      // controller radio first

    s = IDLE_ENABLED; s.client_connected = true;
    assert(!mgmt_should_advertise(&s));                      // single client

    // Legacy config path still advertises (backward compatible).
    s = IDLE_ENABLED; s.enabled = false; s.in_config = true;
    assert(mgmt_should_advertise(&s));
}

static void test_wake_is_never_broken(void) {
    // With a client connected and the console going to sleep OR wake firing, the
    // client is dropped so the single LE advertiser is free for wake.
    mgmt_state_t s = IDLE_ENABLED; s.client_connected = true;
    assert(!mgmt_should_drop_client(&s));                    // steady state: keep
    s.console_awake = false;
    assert(mgmt_should_drop_client(&s));                     // sleep -> drop
    s = IDLE_ENABLED; s.client_connected = true; s.wake_active = true;
    assert(mgmt_should_drop_client(&s));                     // wake -> drop
}

static void test_bond_only_in_pairing_window(void) {
    mgmt_state_t s = IDLE_ENABLED;
    assert(!mgmt_accept_bonding(&s));                        // no window -> no new bond
    s.pairing_window_open = true;
    assert(mgmt_accept_bonding(&s));                         // double-tap window -> ok
    // A connection may still be accepted outside the window (to let an ALREADY
    // bonded phone reconnect); it just cannot form a NEW bond.
    s = IDLE_ENABLED; s.pairing_window_open = false;
    assert(mgmt_accept_connection(&s));
    assert(!mgmt_accept_bonding(&s));
}

static void test_writes_require_bond_and_allowlist(void) {
    mgmt_state_t s = IDLE_ENABLED;
    s.client_connected = true; s.client_bonded = true;

    // Bonded + allowlisted user commands: allowed.
    assert(mgmt_allow_write(&s, "amiibo select save1"));
    assert(mgmt_allow_write(&s, "amiibo present"));
    assert(mgmt_allow_write(&s, "body 10 20 30"));
    assert(mgmt_allow_write(&s, "save"));
    assert(mgmt_allow_write(&s, "get"));

    // Bonded but diagnostic/unlisted command: rejected by the allowlist.
    assert(!mgmt_allow_write(&s, "imu"));
    assert(!mgmt_allow_write(&s, "sw2cap start"));
    assert(!mgmt_allow_write(&s, "state"));
    assert(!mgmt_allow_write(&s, "setns2map 0 1 2"));

    // Connected but NOT bonded: never, even for an allowlisted command. This is
    // the core anti-hijack property -- a random nearby phone that connected
    // (e.g. during a pairing window it didn't complete) cannot issue commands.
    s.client_bonded = false;
    assert(!mgmt_allow_write(&s, "amiibo select save1"));
    assert(!mgmt_allow_write(&s, "save"));

    // Disabled feature: never, even bonded.
    s.client_bonded = true; s.enabled = false; s.in_config = false;
    assert(!mgmt_allow_write(&s, "amiibo select save1"));
}

static void test_single_client(void) {
    mgmt_state_t s = IDLE_ENABLED; s.client_connected = true;
    assert(!mgmt_accept_connection(&s));   // a second connection is refused
}

int main(void) {
    test_disabled_is_invisible();
    test_advertises_only_when_safe();
    test_wake_is_never_broken();
    test_bond_only_in_pairing_window();
    test_writes_require_bond_and_allowlist();
    test_single_client();
    puts("mgmt access-control tests passed");
    return 0;
}
