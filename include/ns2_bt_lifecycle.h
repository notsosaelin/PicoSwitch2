#ifndef NS2_BT_LIFECYCLE_H
#define NS2_BT_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

// Pure Bluetooth trust/admission policy. Stack-specific code supplies whether
// usable trust already exists; this layer decides whether a connection may be
// attempted and whether it may create replacement trust.
typedef enum {
    NS2_BT_ADMISSION_REJECT = 0,
    NS2_BT_ADMISSION_RECONNECT,
    NS2_BT_ADMISSION_FRESH,
} ns2_bt_admission_t;

// Connection parameters the adapter asks for on the LE management link.
//
// The adapter is the LE peripheral there, so the phone picks the parameters and
// we previously never asked for anything at all. Two things justify asking:
//
//   1. an unexplained asymmetry -- the Classic link runs on a 20 s supervision
//      timeout while this one runs on whatever the phone chose, typically ~2 s,
//      and nothing in this project chose or defends that gap;
//   2. JoypadOS hit link loss under single-radio LE+Classic coexistence and
//      fixed it by moving exactly this parameter from 2 s to 6 s (efa0202).
//
// Deliberately NOT justified by the 2026-08-22 capture: the ~2.35 s SLEEP_IND
// window in that trace is routine IBS UART idle (the same log has 124 sleep
// cycles alongside 113 successful GATT round trips while healthy), not a
// measured outage. Do not reintroduce "2.35 s < 6 s" as the rationale.
//
// Our fork predates efa0202 and never inherited it.
//
// Latency is 0 so no connection event is skipped. The interval range is left
// wide on purpose -- the evidence calls for supervision margin, not a slower
// link, so the central keeps its scheduling freedom and only the timeout
// changes. The central may reject or ignore the request entirely; this is
// best-effort margin, not a guarantee.
//
// Scope, so this is not over-credited: it can only protect the LE management
// session across a short peer stall. Classic supervision is a separate 20 s
// controller-side value this firmware does not set, and nothing here survives
// the peer's Bluetooth process aborting.
typedef struct {
    uint16_t interval_min_units;         // 1.25 ms units
    uint16_t interval_max_units;         // 1.25 ms units
    uint16_t latency;                    // connection events that may be skipped
    uint16_t supervision_timeout_units;  // 10 ms units
} ns2_bt_le_link_params_t;

ns2_bt_le_link_params_t ns2_bt_mgmt_link_params(void);

// Core spec constraint: supervision_timeout > (1 + latency) * interval_max * 2.
// Encoded so a future parameter edit cannot silently produce a combination the
// controller will reject.
bool ns2_bt_le_link_params_valid(ns2_bt_le_link_params_t params);

ns2_bt_admission_t ns2_bt_admission_decide(bool pairing_lockout,
                                            bool pairing_window_open,
                                            bool trust_present);

// Classic admission trust is cross-transport.
//
// Before 2026-08-20 the Classic connection filter admitted every peer that was
// not wipe-locked. Trust gating (3cb11ce) made a stored Classic link key the
// only way in outside a pairing window. That is correct for controllers, which
// pair over Classic -- but the Android companion pairs over LE, and its
// Controller Link is a Classic HID Device connection. The pairing window's only
// opener is the physical gesture, so the companion had no way to satisfy the
// new requirement.
//
// The gap is normally hidden by cross-transport key derivation. Pinned BTstack
// 1.6.2 derives the Classic key at the very end of LE pairing and persists it
// separately from the LE bond, so the two are not atomic; upstream 232f80e60
// ("sm: derive BR/EDR link key from LTK before sending DHKey check") moved the
// derivation ahead of the DHKey Check and into sm_store_bonding_information()
// for exactly that reason. An LE bond can therefore legitimately exist with no
// Classic link key, and after trust gating that state rejects every Controller
// Link attempt for good.
//
// SECURITY CONTRACT. Both inputs must describe THE SAME PEER as the incoming
// Classic connection. This layer cannot enforce that -- it sees booleans, not
// addresses -- so the binding lives at the call site and must not be loosened
// there. The second input is deliberately NOT "an LE bond exists somewhere",
// and not even "some bond has this address": it is
// btstack_host_classic_companion_session_trust(), which requires a management
// session that is connected right now, whose peer address equals this Classic
// peer's, and which is bonded and encrypted with a full-length key.
//
// The encryption requirement is what makes the address match meaningful. Any
// device can claim a BD_ADDR; none can bring up an encrypted session as that
// identity without the LTK. Because it is live state rather than a stored
// grant, losing the management session revokes it at once.
//
// This is strictly narrower than the pre-2026-08-20 behaviour it restores
// (which admitted every peer), and never bypasses pairing_lockout.
bool ns2_bt_classic_trust_present(bool classic_link_key_present,
                                  bool companion_session_trusted);

// Who drives Classic encryption once authentication completes.
//
// BTstack queues its own HCI_Set_Connection_Encryption on EVERY successful
// Authentication Complete where the link is not already encrypted
// (hci.c:4240 -> BONDING_SEND_ENCRYPTION_REQUEST -> hci.c:7472). That is
// unconditional: it does not consider who started authentication, nor whether
// this host requires encryption at all. Pinned 1.6.2 and upstream master both
// behave this way, so there is no upstream fix to take.
//
// Both controllers report Authentication Complete for the same LMP
// authentication, so both hosts then start the LMP encryption procedure. That
// is precisely what HCI_ERR_LMP_ERR_TRANS_COLLISION (0x23) means -- the same
// procedure initiated from both ends -- and it is what the 2026-08-22 campaign
// captured 8 times: authentication succeeds, Android sends
// SET_CONNECTION_ENCRYPTION, and ~7 ms later "Encryption failure 35", after
// which Android drops the ACL and the Controller Link never comes up.
//
// Deferring is deliberately narrow. It applies only when the peer is the
// companion holding a live encrypted management session -- a phone whose HID
// Device profile requires encryption and will always drive it -- and only when
// this host does not own a FRESH PAIRING on that link.
//
// The second condition is not "did we request security at all". On an ordinary
// reconnect this host also calls gap_request_security_level(LEVEL_2), so that
// broader reading is true for essentially every companion link and would make
// the stand-down dead code. The distinction is ownership: during a fresh
// pairing we drive the whole establishment and must finish it; on a reconnect
// the peer requires and drives encryption, and the level we asked for is
// satisfied by the peer's Encryption Change through the identical code path.
//
// Physical controllers are untouched either way: they never satisfy the
// companion predicate, so they keep BTstack's stock behaviour exactly.
bool ns2_bt_defer_classic_encryption(bool peer_is_companion_session,
                                     bool we_own_fresh_pairing_security);

// Classifying the Encryption Change that follows a stand-down.
//
// Standing down is only correct if the peer actually finishes the job, so the
// two outcomes are counted separately rather than assumed. A collision is
// identified by status alone -- it can arrive on any link, including one we
// never deferred -- whereas peer-led completion is only attributable when it
// lands on the handle we stood down on, otherwise an ordinary encrypted
// reconnect would be miscounted as proof the mechanism worked.
#define NS2_BT_HCI_LMP_TRANSACTION_COLLISION 0x23u

bool ns2_bt_encryption_collision(uint8_t hci_status);
bool ns2_bt_encryption_completed_for_deferral(uint8_t hci_status,
                                              bool encryption_enabled,
                                              bool handle_matches_deferral);

// The association itself, extracted so the conjunction is pinned by tests
// rather than by convention. Every condition is load-bearing:
//
//   session_connected            - a stored bond is not a live proof
//   peer_address_known           - never compare against an unset address
//   peer_address_matches         - the SAME peer, not merely some companion
//   session_bonded_and_encrypted - what an impostor cannot forge
//
// Dropping any one of these turns a cryptographic binding into a spoofable one.
bool ns2_bt_companion_session_trust(bool session_connected,
                                    bool peer_address_known,
                                    bool peer_address_matches,
                                    bool session_bonded_and_encrypted);

// Delay before Classic inquiry restarts, in milliseconds.
//
// Idle discovery restarted inquiry the instant the previous round completed
// (INQUIRY_DURATION 5 => 6.4s rounds, back to back, alternating GIAC/LIAC),
// alongside a 50%-duty active LE scan, on one CYW43 radio. Inquiry and page
// scan are distinct Core-spec substates, so a continuously inquiring adapter
// leaves little room to answer an incoming page. That occupancy exists only
// while state == BLE_STATE_SCANNING -- precisely the zero-controller case that
// fails -- and stops once a controller attaches, which is the configuration
// physically accepted on 2026-08-21.
//
// While a pairing window is open the maintainer is actively pairing and
// discovery latency wins. Outside one, reachability wins. Evidence for the
// mechanism is Strong, not Confirmed: the effective page-scan starvation has
// not been measured on the controller, and no improvement magnitude is claimed.
#define NS2_BT_INQUIRY_IDLE_GAP_MS 2000u

uint32_t ns2_bt_inquiry_restart_delay_ms(bool pairing_window_open);

// A Classic Link Key Notification is not proof that an existing relationship
// authenticated: BTstack processes and may persist the notification before the
// later Authentication Complete event reaches the application. A replacement
// key therefore needs explicit fresh admission. The only window-closed updates
// allowed are the identical existing key or the controller's authenticated
// Change Connection Link Key result; both are still committed only after
// Authentication Complete succeeds.
bool ns2_bt_classic_key_update_admitted(bool pairing_lockout,
                                        bool pending_identity_matches,
                                        bool fresh_pairing_admitted,
                                        bool trust_present,
                                        bool same_existing_key,
                                        bool authenticated_key_change);

bool ns2_bt_classic_key_commit_allowed(bool pairing_lockout,
                                       bool authentication_succeeded,
                                       bool pending_key_present,
                                       bool key_update_admitted);

// Authentication Failure (0x05) is generic and must not destroy a previously
// usable local key. PIN_OR_KEY_MISSING (0x06) specifically establishes that
// the peer no longer has the relationship.
bool ns2_bt_classic_auth_failure_forgets_existing(uint8_t hci_status);

typedef enum {
    NS2_BT_CUSTOM_REJECT = 0,
    NS2_BT_CUSTOM_VERIFY_RECONNECT,
    NS2_BT_CUSTOM_ENCRYPTED_RECONNECT,
    NS2_BT_CUSTOM_FRESH,
} ns2_bt_custom_admission_t;

// Switch 2 custom ATT must never interpret a loose RPA candidate as identity.
// An RPA may reach SM so cryptographic identity reuse can be attempted, but it
// cannot enter the custom fresh-pairing handshake without its explicit latch.
ns2_bt_custom_admission_t ns2_bt_custom_admission_decide(
    bool pairing_lockout,
    bool encrypted_reconnect,
    bool fresh_pairing_admitted,
    bool rpa_trust_candidate);

// A firmware-install reset erases the TLV lock tag along with the bonds. Keep
// admission closed for that boot even though no persisted tag can survive the
// erase itself.
bool ns2_bt_boot_pairing_locked(bool persisted_lockout,
                                bool install_reset_performed);

// config_install_reset_performed() is a boot fact, not an HCI-state fact.
// Consume it exactly once so a later HCI restart cannot re-lock pairing after
// the user has explicitly reopened the pairing window.
bool ns2_bt_install_reset_bootstrap_take(bool install_reset_performed,
                                         bool *consumed);

// An SSP prompt belongs to the Classic connection attempt that was admitted.
// Window expiry may close admission for new candidates, but it must not revoke
// an already-admitted matching attempt. A wipe lockout still wins immediately.
bool ns2_bt_classic_ssp_response_admitted(bool pairing_lockout,
                                          bool pending_identity_matches,
                                          bool fresh_pairing_admitted);

// A management removal names an LE address type. The public address-only
// helper is intentionally cross-transport, but a typed removal must not match
// a same-address Classic relationship.
bool ns2_bt_forget_matches_address_type(bool match_address_type,
                                        int requested_address_type,
                                        int candidate_address_type);

typedef bool (*ns2_bt_bond_entry_at_fn)(void *context, int slot,
                                        int *address_type,
                                        uint8_t address[6]);

// Search the complete slot capacity, not the active-entry count. BTstack's TLV
// backend permits holes, so count() is not a safe traversal bound.
int ns2_bt_find_bond_slot(ns2_bt_bond_entry_at_fn entry_at,
                          void *context,
                          int slot_count,
                          const uint8_t address[6],
                          int address_type,
                          bool match_address_type);

#endif
