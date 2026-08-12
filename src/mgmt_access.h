/*
 * In-band BLE management ACCESS-CONTROL state machine (production).
 *
 * These pure decision functions are the canonical contract for the in-band
 * management path: when it may advertise, accept a connection, accept a NEW
 * bond, allow a command write, and when it must drop its client. They were
 * developed test-first in tools/test_mgmt_access.c and are lifted here verbatim;
 * that test now links this header so the exhaustive 128-state invariant proof
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
    bool enabled;             // g_mgmt_enabled (feature flag, default off)
    bool console_awake;       // !tud_suspended()
    bool wake_active;         // wake_adv.active or a pending wake burst
    bool scanning;            // controller scan/inquiry in progress
    bool pairing_window_open; // BOOTSEL double-tap opened the pairing window
    bool client_connected;    // a management LE-peripheral client is linked
    bool client_bonded;       // that client has an established bond (encrypted)
} mgmt_state_t;

// (1) Advertise connectably only while enabled, the console is awake, wake does
//     NOT need the radio, no controller scan/inquiry is in flight, and no client
//     is already connected (single client; advertising stops on connect).
bool mgmt_should_advertise(const mgmt_state_t *s);

// (2) Accept an incoming management connection only while enabled, awake, and
//     with no existing client.
bool mgmt_accept_connection(const mgmt_state_t *s);

// (3) Accept a NEW bond only while the feature is enabled AND inside the
//     deliberate double-tap pairing window. Requiring `enabled` (not just the
//     window) prevents a phone from forming a management bond during a window
//     opened only to add a CONTROLLER while management is off.
bool mgmt_accept_bonding(const mgmt_state_t *s);

// (4) Allow a command write only from a connected, BONDED client while enabled
//     and only for an allowlisted command.
bool mgmt_allow_write(const mgmt_state_t *s, const char *command);

// (5) Drop the management client when it must yield the radio: feature off,
//     console asleep, or wake needs the advertiser. Guarantees wake-from-sleep
//     is never blocked by a lingering management link.
bool mgmt_should_drop_client(const mgmt_state_t *s);

#ifdef __cplusplus
}
#endif

#endif  // MGMT_ACCESS_H
