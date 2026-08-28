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

// Whether to stand down from initiating Classic AUTHENTICATION as well.
//
// Same principle as ns2_bt_defer_classic_encryption(), one procedure earlier.
// On an incoming Classic connection that already has a stored link key, this
// host calls gap_request_security_level(LEVEL_2), which sets
// BONDING_SEND_AUTHENTICATE_REQUEST and sends HCI_Authentication_Requested.
// Android sends it too, because its HID Device profile's L2CAP security
// requires it. Both Link Managers then run the same LMP procedure.
//
// Captured on hardware 2026-08-22 across 20 companion reconnects on one build:
// six attempts logged `btm_sec_auth_complete: ... status: 35` (0x23, LMP Error
// Transaction Collision) and recovered, and two landed the same collision on
// the encryption step instead, where Android does not recover -- it disconnects
// the ACL and the Controller Link fails. The encryption-side stand-down alone
// could not prevent those, because the redundant request that races is the
// authentication one.
//
// SECURITY NOTE. This changes WHO INITIATES, never WHAT IS REQUIRED. The link
// must still reach Authentication Complete, encryption, and the required key
// size before HID is usable; see btstack_host's HID-ready gate. Standing down
// is only safe because the peer is the companion, whose HID Device profile is
// guaranteed to drive security -- and if it somehow does not, the link fails
// closed rather than running unauthenticated.
// Classic encryption key size the companion link must reach before HID is
// allowed to become usable. BTstack's own gap_required_encryption_key_size
// governs its internal security level; this is the acceptance invariant for the
// Controller Link specifically, kept explicit so a stand-down can never be
// mistaken for a relaxation of what security is required.
#define NS2_BT_REQUIRED_CLASSIC_KEY_SIZE 16u

bool ns2_bt_defer_classic_authentication(bool peer_is_companion_session,
                                         bool stored_classic_key_present,
                                         bool we_own_fresh_pairing_security);

// Classifying the Encryption Change that follows a stand-down.
//
// Standing down is only correct if the peer actually finishes the job, so the
// two outcomes are counted separately rather than assumed. A collision is
// identified by status alone -- it can arrive on any link, including one we
// never deferred -- whereas peer-led completion is only attributable when it
// lands on the handle we stood down on, otherwise an ordinary encrypted
// reconnect would be miscounted as proof the mechanism worked.
// What this host actually OBSERVED about Classic authentication on a link.
//
// Three states, not a bool, because two of them are not failures.
// HCI_Authentication_Complete is generated in response to this host's own
// HCI_Authentication_Requested. On the peer-led path -- the intended path
// whenever ns2_bt_defer_classic_authentication() returns true -- that command
// is never sent, so the event never arrives and there is nothing to observe.
// The peer's authentication reaches us as Link Key Request followed by
// Encryption Change instead.
//
// Recording that as `auth_completed_ok = false` made a correct, secure link
// look like an authentication failure, and would make an acceptance harness
// demand an event that structurally no longer occurs. Security is judged from
// state that does exist: encryption enabled, key size at the acceptance gate,
// and HID readiness.
typedef enum {
    NS2_BT_AUTH_NOT_OBSERVED = 0,   // no local Authentication Complete (expected when deferred)
    NS2_BT_AUTH_OBSERVED_OK,        // local Authentication Complete, status 0
    NS2_BT_AUTH_OBSERVED_FAILED,    // local Authentication Complete, non-zero status
} ns2_bt_auth_observation_t;

const char *ns2_bt_auth_observation_name(ns2_bt_auth_observation_t observation);

// Is the Classic security invariant satisfied for a companion Controller Link?
//
// Stated in terms of what is observable on the PEER-LED path. Notably it does
// not require a local Authentication Complete, because deferring initiation is
// the intended behaviour and that event does not occur; but it never accepts an
// observed authentication FAILURE, and it always requires encryption and the
// full key size to have been positively observed at the acceptance gate.
bool ns2_bt_companion_security_satisfied(ns2_bt_auth_observation_t auth_outcome,
                                         bool encrypted_ok,
                                         bool key_size_valid,
                                         uint8_t key_size,
                                         bool hid_ready);

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

// Controller Link is not a standalone transport.
//
// PRODUCT INVARIANT. The Android companion's Controller Link is a facility of a
// live management relationship, not an independent way to be a controller. BLE
// management may run alone for as long as it likes; Controller Link may not.
// Concretely: it may be established only while that same peer holds a connected,
// bonded, encrypted management session, and it must be torn down when that
// session is genuinely lost.
//
// This is a product rule, but it also closes a real failure path. A companion
// that reaches Classic admission on its stored link key alone is NOT recognised
// as the companion -- companion trust is live state -- so it falls through into
// the ordinary physical-controller security path, where this host calls
// gap_request_security_level(LEVEL_2) while Android's HID Device profile starts
// the identical LMP authentication. That is the dual-initiation collision
// (0x23) captured on 2026-08-22; see ns2_bt_defer_classic_authentication().
// Refusing the connection outright is strictly cleaner than admitting it and
// then racing over who owns security on a link that must not exist.
//
// The first input means "this identity exists on both transports". The call
// site derives it from the LE bond database, because the companion is the only
// peer this firmware creates that way: it bonds over LE for management, and its
// Classic key is cross-transport-derived from that same LE bond. Physical
// controllers are single-transport -- Classic controllers hold no LE bond, and
// BLE controllers never arrive on Classic -- so none of them can satisfy it and
// none of them change behaviour. If a future controller ever pairs over BOTH
// transports under one identity address, this predicate would start refusing
// its Classic reconnects while management is down, and the call site (not this
// layer) is where that would have to be narrowed.
//
// Physical controllers are unaffected in the other direction too: a false first
// input makes this the identity function on admission.
bool ns2_bt_companion_classic_admission_allowed(
    bool peer_is_cross_transport_companion, bool companion_session_trusted);

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

/*
 * Has the Classic authentication behind a notified link key actually succeeded?
 *
 * TWO events prove it and NEITHER is guaranteed to arrive.
 *
 * HCI_Authentication_Complete is generated in response to this host's own
 * HCI_Authentication_Requested (Core spec Vol 4 Part E 7.1.15). This firmware
 * sends that command only for the Wiimote family and the one Classic name that
 * needs early SSP; BTstack's HID Host registers its L2CAP services at LEVEL_0
 * (see gap_set_security_level() before hid_host_init()), so it never asks
 * either. Every other Classic controller -- DualSense included -- drives SSP
 * itself, and its authentication reaches this host as Link Key Notification
 * plus Encryption Change with NO Authentication Complete at all. Waiting only
 * for the local event means waiting forever.
 *
 * Encryption Change reporting encryption ENABLED is equally conclusive and is
 * the event peer-led pairings do produce: a Classic link cannot be encrypted
 * except with a link key both ends already hold and authenticated against.
 *
 * This does NOT relax admission. The key being proven here was already admitted
 * by ns2_bt_classic_key_update_admitted(); this answers only "did the pairing
 * that produced it succeed", and accepting the peer-led proof is what makes a
 * first pairing durable instead of good for one session.
 */
bool ns2_bt_classic_authentication_proven(bool local_auth_complete_ok,
                                          bool encryption_enabled_ok);

/*
 * May an automatic recovery path delete a durable bond because authentication
 * failed with this HCI status?
 *
 * **No, for every status.** This is now a constant `false`, kept as a function
 * so the rule has one name, one place, and one test.
 *
 * It used to return true for 0x06 PIN_OR_KEY_MISSING, on the reasoning that the
 * status specifically means the peer no longer holds the relationship. The
 * reasoning is sound about the PEER and wrong about the consequence: 0x06 is
 * still a report from the other end of a radio link, arriving on a connection
 * that has already failed, and treating it as authority to mutate persistent
 * storage means one bad handshake permanently destroys a pairing the user made.
 *
 * Observed 2026-08-28: an adapter that had three bonds earlier in the same
 * powered session reported `btbonds: []` with no reflash and no power cycle.
 * Each 0x05/0x06 recovery site deletes one bond, and there were enough sites
 * across LE disconnect, LE re-encryption and Classic authentication to account
 * for all of them. The exact trigger was never proven; the destructive response
 * to it did not need proving to be wrong.
 *
 * The replacement policy: a failed authentication drops the LINK and leaves the
 * CREDENTIAL alone. If the bond really is stale the next attempt fails the same
 * way -- bounded, visible, and recoverable -- and the user resolves it with an
 * explicit action. Destroying durable state is reserved for things the user
 * asked for: selective forget, `bonds remove`, the BOOTSEL wipe, and the
 * install reset.
 *
 * Retained rather than deleted so that reintroducing automatic deletion has to
 * come past this comment and its test.
 */
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

// Does a gap_disconnect() call still guarantee a disconnection-complete event?
//
// BTstack 1.6.2 synthesised one when handed a handle the controller had already
// released:
//
//     uint8_t gap_disconnect(hci_con_handle_t handle){
//         hci_connection_t * conn = hci_connection_for_handle(handle);
//         if (!conn){ hci_emit_disconnection_complete(handle, 0); return 0; }
//
// (hci.c:9076). Every teardown in this firmware converges its own record from
// HCI_EVENT_DISCONNECTION_COMPLETE, so a stale handle still cleaned itself up.
// BTstack 1.8.2 returns ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER and emits
// nothing (hci.c:9834). A caller that keeps waiting for the event now waits
// forever, and the record it owns -- a BLE controller slot, the management
// handle, an init state machine -- stays occupied for the rest of the boot.
//
// Classified here rather than at each call site so the rule is pinned by tests
// and stated once. Note that COMMAND_DISALLOWED is NOT a local-convergence
// case: it means a disconnect is already requested or sent on that handle, so
// the event is still coming and converging early would tear down twice.
// Anything unrecognised converges locally, because only the two statuses above
// are documented to leave an event in flight.
typedef enum {
    NS2_BT_DISCONNECT_EVENT_PENDING = 0,  // wait for HCI_EVENT_DISCONNECTION_COMPLETE
    NS2_BT_DISCONNECT_CONVERGE_LOCALLY,   // no event is coming; release the record now
} ns2_bt_disconnect_outcome_t;

#define NS2_BT_HCI_UNKNOWN_CONNECTION_IDENTIFIER 0x02u
#define NS2_BT_HCI_COMMAND_DISALLOWED            0x0Cu

ns2_bt_disconnect_outcome_t ns2_bt_disconnect_outcome(uint8_t gap_disconnect_status);

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
