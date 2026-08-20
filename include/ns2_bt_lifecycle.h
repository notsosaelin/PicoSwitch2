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

ns2_bt_admission_t ns2_bt_admission_decide(bool pairing_lockout,
                                            bool pairing_window_open,
                                            bool trust_present);

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
