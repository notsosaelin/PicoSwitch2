#include "ns2_bt_lifecycle.h"

#include <string.h>

ns2_bt_admission_t ns2_bt_admission_decide(bool pairing_lockout,
                                            bool pairing_window_open,
                                            bool trust_present)
{
    if (pairing_lockout)
        return NS2_BT_ADMISSION_REJECT;
    if (trust_present)
        return NS2_BT_ADMISSION_RECONNECT;
    return pairing_window_open ? NS2_BT_ADMISSION_FRESH
                               : NS2_BT_ADMISSION_REJECT;
}

bool ns2_bt_boot_pairing_locked(bool persisted_lockout,
                                bool install_reset_performed)
{
    return persisted_lockout || install_reset_performed;
}

bool ns2_bt_forget_matches_address_type(bool match_address_type,
                                        int requested_address_type,
                                        int candidate_address_type)
{
    return !match_address_type ||
           requested_address_type == candidate_address_type;
}

int ns2_bt_find_bond_slot(ns2_bt_bond_entry_at_fn entry_at,
                          void *context,
                          int slot_count,
                          const uint8_t address[6],
                          int address_type,
                          bool match_address_type)
{
    if (!entry_at || !address || slot_count < 0)
        return -1;

    for (int slot = 0; slot < slot_count; ++slot) {
        int stored_type = -1;
        uint8_t stored_address[6] = {0};
        if (!entry_at(context, slot, &stored_type, stored_address))
            continue;
        if (match_address_type && stored_type != address_type)
            continue;
        if (memcmp(stored_address, address, sizeof(stored_address)) == 0)
            return slot;
    }
    return -1;
}
