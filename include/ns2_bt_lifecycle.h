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

// A firmware-install reset erases the TLV lock tag along with the bonds. Keep
// admission closed for that boot even though no persisted tag can survive the
// erase itself.
bool ns2_bt_boot_pairing_locked(bool persisted_lockout,
                                bool install_reset_performed);

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
