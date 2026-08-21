/*
 * In-band BLE management ACCESS-CONTROL state machine (production).
 *
 * These pure decision functions are the canonical contract for the in-band
 * management path: when it may advertise, accept a connection, accept a NEW
 * bond, allow a command write, and when it must drop its client. They were
 * developed test-first in tools/test_mgmt_access.c and are lifted here verbatim;
 * that test now links this header so the exhaustive 256-state invariant proof
 * guards the real code.
 *
 * Every field of mgmt_state_t maps to an existing firmware signal, so the caller
 * side is a mechanical snapshot (see docs/bluetooth/in-band-management-plan.md
 * §2b/§3). The functions are side-effect free and depend only on the wireless
 * allowlist (config_wireless_command_allowed) for the write decision, so they
 * are host-testable without any BTstack or USB state.
 */
#ifndef MGMT_ACCESS_H
#define MGMT_ACCESS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// A snapshot of the signals the access decisions depend on. Populated by the
// caller (btstack_host.c) from live firmware state before each decision.
typedef struct {
    bool enabled;             // g_mgmt_enabled (production default on)
    bool console_awake;       // !tud_suspended()
    bool wake_active;         // wake_adv.active or a pending wake burst
    bool scanning;            // observation only; controller discovery coexists with advertising
    bool pairing_window_open; // BOOTSEL double-tap opened the pairing window
    bool client_connected;    // a management LE-peripheral client is linked
    bool client_bonded;       // that client has an established LE bond
    bool client_encrypted;    // current ATT link has a valid encryption key
} mgmt_state_t;

// (1) Advertise connectably only while enabled, the console is awake, wake does
//     NOT need the advertiser, and no client is already connected (single
//     client; advertising stops on connect). Controller scan/inquiry may run
//     concurrently: suppressing the advertiser during discovery caused the
//     hardware-observed reconnect starvation fixed on 2026-08-13.
bool mgmt_should_advertise(const mgmt_state_t *s);

// (2) Accept an incoming management connection only while enabled, awake, and
//     with no existing client.
bool mgmt_accept_connection(const mgmt_state_t *s);

// (3) Accept a NEW bond only while the feature is enabled AND inside the
//     deliberate double-tap pairing window. Requiring `enabled` (not just the
//     window) prevents a phone from forming a management bond during a window
//     opened only to add a CONTROLLER while management is off.
bool mgmt_accept_bonding(const mgmt_state_t *s);

// (3b) Per-attempt form of (3). The pairing window authorizes an ATTEMPT, and
//      the caller latches mgmt_accept_bonding()'s answer at the moment the
//      management connection is ACCEPTED. SM confirmation then consults the
//      latch instead of re-reading the live window.
//
//      Why: the window is a fixed 30 s (PAIRING_WINDOW_MS) from a physical
//      BOOTSEL gesture, but an Android fresh bond interposes the phone's own
//      pairing dialog between the LE connection and SMP confirmation. Re-reading
//      the live window there made a user's already-given authorization expire
//      mid-procedure. Controller BLE candidates never had this problem because
//      they latch at connection complete (conn->fresh_pairing_admitted); this
//      makes management match that established rule.
//
//      This does not widen WHO may bond: an attempt that was not admitted when
//      it connected can never become admitted later, and `enabled` is re-read so
//      the `mgmt off` escape hatch still revokes a latched attempt. The latch is
//      bounded by its own connection -- the caller clears it on disconnect and
//      on HCI loss.
bool mgmt_accept_latched_bonding(bool enabled, bool attempt_admitted);

// A management ATT session is usable only after the link is encrypted with a
// stored bond. No-display Android Just Works cannot provide MITM
// authentication, so the firmware must not claim ATT_SECURITY_AUTHENTICATED.
bool mgmt_session_authorized(const mgmt_state_t *s);

// (4) Allow a command write only from a connected, BONDED, encrypted client
//     while enabled and only for an allowlisted command.
bool mgmt_allow_write(const mgmt_state_t *s, const char *command);

// (5) Drop the management client when it must yield the radio: feature off,
//     console asleep, or wake needs the advertiser. Guarantees wake-from-sleep
//     is never blocked by a lingering management link.
bool mgmt_should_drop_client(const mgmt_state_t *s);

// A management link is trusted only after BTstack resolves it to a durable LE
// bond and encryption is active with the 16-byte key required by the ATT
// database. Just Works provides
// encryption/bonding but not MITM authentication; this predicate deliberately
// does not mislabel it as authenticated.
bool mgmt_link_is_trusted(bool bonded, unsigned encryption_key_size);

#ifdef __cplusplus
}
#endif

#endif  // MGMT_ACCESS_H
