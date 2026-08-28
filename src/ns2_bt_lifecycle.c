#include "ns2_bt_lifecycle.h"

#include <string.h>

ns2_bt_le_link_params_t ns2_bt_mgmt_link_params(void)
{
    // The supervision timeout is the entire point of this request. The interval
    // range is deliberately PERMISSIVE rather than JoypadOS's 30--50 ms: the
    // captured evidence requires margin, not a slower link, and narrowing the
    // interval would tax bulk management transfers for no evidenced reason.
    // Leaving the floor at the 7.5 ms spec minimum lets the central keep
    // whatever interval it already chose and change only the timeout.
    ns2_bt_le_link_params_t params = {
        .interval_min_units = 6u,           // 7.5 ms, spec minimum
        .interval_max_units = 40u,          // 50 ms
        .latency = 0u,
        .supervision_timeout_units = 600u,  // 6 s -- see the header
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
                                  bool companion_session_trusted)
{
    return classic_link_key_present || companion_session_trusted;
}

bool ns2_bt_defer_classic_encryption(bool peer_is_companion_session,
                                     bool we_own_fresh_pairing_security)
{
    return peer_is_companion_session && !we_own_fresh_pairing_security;
}

bool ns2_bt_defer_classic_authentication(bool peer_is_companion_session,
                                         bool stored_classic_key_present,
                                         bool we_own_fresh_pairing_security)
{
    // A stored key is required: without one there is nothing to authenticate
    // against and this host must drive the fresh pairing itself.
    return peer_is_companion_session && stored_classic_key_present &&
           !we_own_fresh_pairing_security;
}

const char *ns2_bt_auth_observation_name(ns2_bt_auth_observation_t observation)
{
    switch (observation) {
        case NS2_BT_AUTH_OBSERVED_OK:     return "observed_ok";
        case NS2_BT_AUTH_OBSERVED_FAILED: return "observed_failed";
        case NS2_BT_AUTH_NOT_OBSERVED:
        default:                          return "not_observed";
    }
}

bool ns2_bt_companion_security_satisfied(ns2_bt_auth_observation_t auth_outcome,
                                         bool encrypted_ok,
                                         bool key_size_valid,
                                         uint8_t key_size,
                                         bool hid_ready)
{
    if (auth_outcome == NS2_BT_AUTH_OBSERVED_FAILED) return false;
    return encrypted_ok && key_size_valid &&
           key_size >= NS2_BT_REQUIRED_CLASSIC_KEY_SIZE && hid_ready;
}

bool ns2_bt_encryption_collision(uint8_t hci_status)
{
    return hci_status == NS2_BT_HCI_LMP_TRANSACTION_COLLISION;
}

bool ns2_bt_encryption_completed_for_deferral(uint8_t hci_status,
                                              bool encryption_enabled,
                                              bool handle_matches_deferral)
{
    return hci_status == 0u && encryption_enabled && handle_matches_deferral;
}

bool ns2_bt_companion_session_trust(bool session_connected,
                                    bool peer_address_known,
                                    bool peer_address_matches,
                                    bool session_bonded_and_encrypted)
{
    return session_connected && peer_address_known && peer_address_matches &&
           session_bonded_and_encrypted;
}

bool ns2_bt_companion_classic_admission_allowed(
    bool peer_is_cross_transport_companion, bool companion_session_trusted)
{
    return !peer_is_cross_transport_companion || companion_session_trusted;
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
    // No status qualifies. See the header for why 0x06 no longer does.
    (void)hci_status;
    return false;
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

ns2_bt_disconnect_outcome_t ns2_bt_disconnect_outcome(uint8_t gap_disconnect_status)
{
    switch (gap_disconnect_status) {
        case 0x00u:  // ERROR_CODE_SUCCESS: disconnect requested, or already in
                     // RECEIVED_DISCONNECTION_COMPLETE with the event pending
        case NS2_BT_HCI_COMMAND_DISALLOWED:  // already requested/sent
            return NS2_BT_DISCONNECT_EVENT_PENDING;
        default:
            return NS2_BT_DISCONNECT_CONVERGE_LOCALLY;
    }
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
