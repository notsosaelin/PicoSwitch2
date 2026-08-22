/*
 * Pure Bluetooth lifecycle policy coverage. Stack/radio mechanics remain
 * firmware/hardware responsibilities; these tests pin admission and sparse
 * persistent-slot traversal.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude \
 *   tools/test_ns2_bt_lifecycle.c src/ns2_bt_lifecycle.c \
 *   -o build/host-tests/test_ns2_bt_lifecycle.exe
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ns2_bt_lifecycle.h"

enum { SLOT_COUNT = 16 };

typedef struct {
    bool occupied;
    int type;
    uint8_t address[6];
} fixture_slot_t;

static bool fixture_entry_at(void *context, int slot, int *address_type,
                             uint8_t address[6])
{
    fixture_slot_t *slots = context;
    if (!slots || slot < 0 || slot >= SLOT_COUNT || !slots[slot].occupied)
        return false;
    if (address_type)
        *address_type = slots[slot].type;
    if (address)
        memcpy(address, slots[slot].address, sizeof(slots[slot].address));
    return true;
}

static void set_slot(fixture_slot_t slots[SLOT_COUNT], int slot, int type,
                     const uint8_t address[6])
{
    slots[slot].occupied = true;
    slots[slot].type = type;
    memcpy(slots[slot].address, address, sizeof(slots[slot].address));
}

static void test_pairing_admission(void)
{
    assert(ns2_bt_admission_decide(false, false, false) ==
           NS2_BT_ADMISSION_REJECT);
    assert(ns2_bt_admission_decide(false, true, false) ==
           NS2_BT_ADMISSION_FRESH);
    assert(ns2_bt_admission_decide(false, false, true) ==
           NS2_BT_ADMISSION_RECONNECT);
    assert(ns2_bt_admission_decide(true, true, true) ==
           NS2_BT_ADMISSION_REJECT);
}

static void test_boot_lockout(void)
{
    assert(!ns2_bt_boot_pairing_locked(false, false));
    assert(ns2_bt_boot_pairing_locked(true, false));
    assert(ns2_bt_boot_pairing_locked(false, true));
    assert(ns2_bt_boot_pairing_locked(true, true));
}

static void test_install_reset_bootstrap_is_one_shot(void)
{
    bool consumed = false;

    // A normal boot consumes the check without requesting a reset lockout.
    assert(!ns2_bt_install_reset_bootstrap_take(false, &consumed));
    assert(consumed);
    assert(!ns2_bt_install_reset_bootstrap_take(false, &consumed));

    // An install reset applies on the first HCI working transition only.
    consumed = false;
    assert(ns2_bt_install_reset_bootstrap_take(true, &consumed));
    assert(consumed);
    assert(!ns2_bt_install_reset_bootstrap_take(true, &consumed));

    // Once the user unlocks pairing, a later HCI restart must honor the
    // current persisted state instead of replaying the sticky install fact.
    consumed = false;
    bool pairing_locked = ns2_bt_boot_pairing_locked(
        false, ns2_bt_install_reset_bootstrap_take(true, &consumed));
    assert(pairing_locked);
    pairing_locked = false;
    pairing_locked = ns2_bt_boot_pairing_locked(
        pairing_locked,
        ns2_bt_install_reset_bootstrap_take(true, &consumed));
    assert(!pairing_locked);

    assert(!ns2_bt_install_reset_bootstrap_take(true, NULL));
}

static void test_classic_ssp_attempt_admission(void)
{
    // The per-attempt latch, not the current global window, authorizes the SSP
    // response. This is the window-expired-but-in-flight success case.
    assert(ns2_bt_classic_ssp_response_admitted(false, true, true));

    // New/stale identities and trusted reconnects without a fresh attempt do
    // not receive an SSP response outside the window.
    assert(!ns2_bt_classic_ssp_response_admitted(false, false, true));
    assert(!ns2_bt_classic_ssp_response_admitted(false, true, false));

    // Post-wipe lockout outranks a previously admitted attempt.
    assert(!ns2_bt_classic_ssp_response_admitted(true, true, true));
}

static void test_classic_key_replacement(void)
{
    // Fresh pairing admits a new key, but notification alone is never enough
    // to commit it.
    assert(ns2_bt_classic_key_update_admitted(false, true, true,
                                               false, false, false));
    assert(!ns2_bt_classic_key_commit_allowed(false, false, true, true));
    assert(ns2_bt_classic_key_commit_allowed(false, true, true, true));

    // A stale local key cannot authorize an unrelated SSP replacement. A
    // byte-identical notification and the controller's authenticated link-key
    // change mechanism remain valid reconnect maintenance operations.
    assert(!ns2_bt_classic_key_update_admitted(false, true, false,
                                                true, false, false));
    assert(ns2_bt_classic_key_update_admitted(false, true, false,
                                               true, true, false));
    assert(ns2_bt_classic_key_update_admitted(false, true, false,
                                               true, false, true));
    assert(!ns2_bt_classic_key_update_admitted(false, false, true,
                                                true, true, true));

    // Lockout outranks both existing trust and an otherwise valid fresh latch.
    assert(!ns2_bt_classic_key_update_admitted(true, true, true,
                                                true, true, true));
    assert(!ns2_bt_classic_key_commit_allowed(true, true, true, true));

    assert(!ns2_bt_classic_auth_failure_forgets_existing(0x05));
    assert(ns2_bt_classic_auth_failure_forgets_existing(0x06));
}

static void test_switch2_custom_admission(void)
{
    assert(ns2_bt_custom_admission_decide(false, true, false, false) ==
           NS2_BT_CUSTOM_ENCRYPTED_RECONNECT);
    assert(ns2_bt_custom_admission_decide(false, false, true, false) ==
           NS2_BT_CUSTOM_FRESH);
    assert(ns2_bt_custom_admission_decide(false, false, false, true) ==
           NS2_BT_CUSTOM_VERIFY_RECONNECT);
    assert(ns2_bt_custom_admission_decide(false, false, false, false) ==
           NS2_BT_CUSTOM_REJECT);

    // A loose RPA candidate is not authenticated identity and cannot become a
    // custom fresh pairing. It may only proceed to cryptographic verification.
    assert(ns2_bt_custom_admission_decide(false, false, false, true) !=
           NS2_BT_CUSTOM_FRESH);
    assert(ns2_bt_custom_admission_decide(true, true, true, true) ==
           NS2_BT_CUSTOM_REJECT);
}

static void test_typed_forget_scope(void)
{
    enum { LE_PUBLIC = 0, CLASSIC_ACL = 0xfd };
    assert(ns2_bt_forget_matches_address_type(false, LE_PUBLIC,
                                               CLASSIC_ACL));
    assert(ns2_bt_forget_matches_address_type(true, LE_PUBLIC, LE_PUBLIC));
    assert(!ns2_bt_forget_matches_address_type(true, LE_PUBLIC,
                                                CLASSIC_ACL));
}

static void test_sparse_slot_lookup(void)
{
    fixture_slot_t slots[SLOT_COUNT] = {0};
    const uint8_t low[6] = {0, 1, 2, 3, 4, 5};
    const uint8_t high[6] = {10, 11, 12, 13, 14, 15};
    const uint8_t missing[6] = {20, 21, 22, 23, 24, 25};

    // Two active entries with a hole between them reproduce the exact failure
    // mode of using count()==2 as though valid slots were necessarily 0 and 1.
    set_slot(slots, 0, 0, low);
    set_slot(slots, 15, 1, high);
    assert(ns2_bt_find_bond_slot(fixture_entry_at, slots, SLOT_COUNT,
                                 high, 1, true) == 15);
    assert(ns2_bt_find_bond_slot(fixture_entry_at, slots, SLOT_COUNT,
                                 high, 0, true) == -1);
    assert(ns2_bt_find_bond_slot(fixture_entry_at, slots, SLOT_COUNT,
                                 high, 0, false) == 15);
    assert(ns2_bt_find_bond_slot(fixture_entry_at, slots, SLOT_COUNT,
                                 missing, 0, false) == -1);
}

/*
 * Regression, 2026-08-22. Controller Link could not be admitted.
 *
 * 3cb11ce (2026-08-20) replaced the Classic connection filter's
 * "return pairing_lockout ? 0 : 1;" with trust gating. The Android companion
 * pairs over LE but arrives as a Classic HID Device, and the pairing window's
 * only opener is the physical gesture -- so it had no way to satisfy the new
 * requirement whenever pinned BTstack's non-atomic cross-transport derivation
 * left the LE bond without a matching Classic link key.
 */
static void test_classic_trust_is_cross_transport(void)
{
    // The state that rejected every Controller Link attempt for good.
    assert(ns2_bt_classic_trust_present(false, true));
    assert(ns2_bt_admission_decide(false, false,
                                   ns2_bt_classic_trust_present(false, true)) ==
           NS2_BT_ADMISSION_RECONNECT);

    // A Classic link key alone still suffices: controllers are unaffected.
    assert(ns2_bt_classic_trust_present(true, false));

    // Neither form of trust is still no trust. This must not become a blanket
    // "admit anything", which is what the trust gating existed to stop.
    assert(!ns2_bt_classic_trust_present(false, false));
    assert(ns2_bt_admission_decide(false, false,
                                   ns2_bt_classic_trust_present(false, false)) ==
           NS2_BT_ADMISSION_REJECT);

    // A wipe lockout outranks cross-transport trust.
    assert(ns2_bt_admission_decide(true, false,
                                   ns2_bt_classic_trust_present(false, true)) ==
           NS2_BT_ADMISSION_REJECT);
}

/*
 * Idle discovery restarted Classic inquiry the instant the previous round
 * ended, so the radio was inquiring essentially continuously in exactly the
 * zero-controller configuration that fails. The gap is what leaves room to
 * answer an incoming page.
 */
static void test_inquiry_restart_gap(void)
{
    assert(ns2_bt_inquiry_restart_delay_ms(false) == NS2_BT_INQUIRY_IDLE_GAP_MS);
    assert(ns2_bt_inquiry_restart_delay_ms(false) > 0u);

    // Actively pairing: discovery latency wins, restart immediately.
    assert(ns2_bt_inquiry_restart_delay_ms(true) == 0u);
}

/*
 * Regression, 2026-08-22. Captured on hardware: the tablet's Bluetooth SoC sent
 * SLEEP_IND and did not wake for ~2.35 s while two ACLs were live; both links
 * returned HCI reason 0x08. The adapter is the LE peripheral on the management
 * link and asked for nothing, so the phone's supervision timeout decided how
 * long a peer stall had to be to kill the session.
 */
static void test_mgmt_link_params_ride_through_peer_stalls(void)
{
    ns2_bt_le_link_params_t p = ns2_bt_mgmt_link_params();

    // The controller must accept what we ask for.
    assert(ns2_bt_le_link_params_valid(p));

    // The captured stall was ~2.35 s. JoypadOS moved this class from 2 s to 6 s
    // (efa0202). Anything at or under the observed stall reopens the failure.
    assert(p.supervision_timeout_units * 10u >= 6000u);

    // Latency must stay 0: a skipped connection event is dead time added on top
    // of whatever the peer is already doing.
    assert(p.latency == 0u);
}

static void test_le_link_params_validity_rules(void)
{
    ns2_bt_le_link_params_t p = ns2_bt_mgmt_link_params();

    // supervision_timeout > (1 + latency) * interval_max * 2.
    // 50 ms max interval needs > 100 ms; 90 ms must fail, 6 s must pass.
    ns2_bt_le_link_params_t tight = p;
    tight.supervision_timeout_units = 9u;   // 90 ms
    assert(!ns2_bt_le_link_params_valid(tight));

    // Latency widens the requirement, so a timeout that passed at latency 0 can
    // stop passing. This is the arithmetic a future edit is most likely to break.
    ns2_bt_le_link_params_t latent = p;
    latent.latency = 100u;
    latent.supervision_timeout_units = 100u;  // 1 s, but needs > 10.1 s
    assert(!ns2_bt_le_link_params_valid(latent));

    // Out-of-range values are rejected rather than sent to the controller.
    ns2_bt_le_link_params_t bad = p;
    bad.interval_min_units = 5u;            // below the 7.5 ms floor
    assert(!ns2_bt_le_link_params_valid(bad));

    bad = p;
    bad.interval_min_units = 100u;
    bad.interval_max_units = 40u;           // inverted range
    assert(!ns2_bt_le_link_params_valid(bad));
}

int main(void)
{
    test_mgmt_link_params_ride_through_peer_stalls();
    test_le_link_params_validity_rules();
    test_classic_trust_is_cross_transport();
    test_inquiry_restart_gap();
    test_pairing_admission();
    test_boot_lockout();
    test_install_reset_bootstrap_is_one_shot();
    test_classic_ssp_attempt_admission();
    test_classic_key_replacement();
    test_switch2_custom_admission();
    test_typed_forget_scope();
    test_sparse_slot_lookup();
    puts("Bluetooth lifecycle policy tests passed");
    return 0;
}
