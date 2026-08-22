#include "ns2_bt_lifecycle.h"

#include <string.h>

ns2_bt_le_link_params_t ns2_bt_mgmt_link_params(void)
{
    // 15--50 ms interval, 6 s supervision timeout. The timeout is the point:
    // see the header for the captured ~2.35 s peer sleep that dropped both
    // links, and for the JoypadOS commit that resolved the same class.
    ns2_bt_le_link_params_t params = {
        .interval_min_units = 12u,          // 15 ms
        .interval_max_units = 40u,          // 50 ms
        .latency = 0u,
        .supervision_timeout_units = 600u,  // 6 s
    };
    return params;
}

bool ns2_bt_le_link_params_valid(ns2_bt_le_link_params_t params)
{
    // Core spec ranges.
    if (params.interval_min_units < 6u || params.interval_max_units > 3200u)
        return false;
    if (params.interval_min_units > params.interval_max_units)
        return false;
    if (params.supervision_timeout_units < 10u ||
        params.supervision_timeout_units > 3200u)
        return false;
    if (params.latency > 499u)
        return false;

    // supervision_timeout > (1 + latency) * interval_max * 2, all in ms.
    // interval units are 1.25 ms and timeout units are 10 ms, so compare in
    // quarter-milliseconds to stay in integers without losing the 1.25 ms step.
    uint32_t interval_max_qms = (uint32_t)params.interval_max_units * 5u;
    uint32_t required_qms =
        ((uint32_t)params.latency + 1u) * interval_max_qms * 2u;
    uint32_t timeout_qms = (uint32_t)params.supervision_timeout_units * 40u;
    return timeout_qms > required_qms;
}

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

bool ns2_bt_classic_trust_present(bool classic_link_key_present,
                                  bool le_bond_present)
{
    return classic_link_key_present || le_bond_present;
}

uint32_t ns2_bt_inquiry_restart_delay_ms(bool pairing_window_open)
{
    return pairing_window_open ? 0u : NS2_BT_INQUIRY_IDLE_GAP_MS;
}

bool ns2_bt_classic_key_update_admitted(bool pairing_lockout,
                                        bool pending_identity_matches,
                                        bool fresh_pairing_admitted,
                                        bool trust_present,
                                        bool same_existing_key,
                                        bool authenticated_key_change)
{
    if (pairing_lockout || !pending_identity_matches)
        return false;
    if (fresh_pairing_admitted)
        return true;
    return trust_present && (same_existing_key || authenticated_key_change);
}

bool ns2_bt_classic_key_commit_allowed(bool pairing_lockout,
                                       bool authentication_succeeded,
                                       bool pending_key_present,
                                       bool key_update_admitted)
{
    return !pairing_lockout && authentication_succeeded &&
           pending_key_present && key_update_admitted;
}

bool ns2_bt_classic_auth_failure_forgets_existing(uint8_t hci_status)
{
    return hci_status == 0x06;
}

ns2_bt_custom_admission_t ns2_bt_custom_admission_decide(
    bool pairing_lockout,
    bool encrypted_reconnect,
    bool fresh_pairing_admitted,
    bool rpa_trust_candidate)
{
    if (pairing_lockout)
        return NS2_BT_CUSTOM_REJECT;
    if (encrypted_reconnect)
        return NS2_BT_CUSTOM_ENCRYPTED_RECONNECT;
    if (fresh_pairing_admitted)
        return NS2_BT_CUSTOM_FRESH;
    if (rpa_trust_candidate)
        return NS2_BT_CUSTOM_VERIFY_RECONNECT;
    return NS2_BT_CUSTOM_REJECT;
}

bool ns2_bt_boot_pairing_locked(bool persisted_lockout,
                                bool install_reset_performed)
{
    return persisted_lockout || install_reset_performed;
}

bool ns2_bt_install_reset_bootstrap_take(bool install_reset_performed,
                                         bool *consumed)
{
    if (!consumed || *consumed)
        return false;
    *consumed = true;
    return install_reset_performed;
}

bool ns2_bt_classic_ssp_response_admitted(bool pairing_lockout,
                                          bool pending_identity_matches,
                                          bool fresh_pairing_admitted)
{
    return !pairing_lockout && pending_identity_matches &&
           fresh_pairing_admitted;
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
