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
// we previously never asked for anything else. On 2026-08-22 a captured failure
// showed why that matters: the tablet's Bluetooth SoC entered its vendor sleep
// state (SLEEP_IND) and did not wake for ~2.35 s while two ACLs were live, and
// both links came back as HCI reason 0x08 (connection timeout). A supervision
// timeout shorter than a peer's radio stall turns a transient stall into a
// dropped session.
//
// JoypadOS hit the same class on a single-radio dongle running LE and Classic
// together and fixed it the same way (efa0202, "coexistence-safe params"):
// it moved off a 2 s supervision timeout to 6 s so the link "rides through
// contention". Our fork predates that commit and never inherited it.
//
// Latency is 0 so no connection event is skipped, and the interval range stays
// wide enough that the central keeps its scheduling freedom. The central may
// reject or ignore the request; this is best-effort margin, not a guarantee.
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
// An LE bond is proof the maintainer admitted this identity through a physical
// pairing window. Honouring it for Classic is strictly narrower than the
// pre-2026-08-20 behaviour it restores, and never bypasses pairing_lockout.
bool ns2_bt_classic_trust_present(bool classic_link_key_present,
                                  bool le_bond_present);

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
