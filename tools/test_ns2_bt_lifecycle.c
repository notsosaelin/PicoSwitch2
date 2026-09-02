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

// A deferred pairing-window close that never resolves latches the pairing
// window shut for the rest of the boot -- LED stuck blinking, every later
// window a silent no-op, the BOOTSEL gesture included. Confirmed on hardware
// 2026-09-02 when a connect attempt outlived its window and the cancel produced
// no LE_CONNECTION_COMPLETE for resolve_deferred_pairing_close() to act on.
// These pin the exits that do not depend on an HCI event arriving.
static void test_pairing_deferral_cannot_latch(void)
{
    const uint32_t bound = 12000u;

    // Nothing deferred: never asks for a resolve.
    assert(!ns2_bt_pairing_deferral_resolved(false, true, 0u, 99999u, bound));
    assert(!ns2_bt_pairing_deferral_resolved(false, false, 0u, 99999u, bound));

    // Deferred while the attempt it protects is still in flight and inside the
    // bound: keep deferring. This is what the deferral exists for, and it must
    // survive the fix.
    assert(!ns2_bt_pairing_deferral_resolved(true, true, 1000u, 1000u, bound));
    assert(!ns2_bt_pairing_deferral_resolved(true, true, 1000u, 12999u, bound));

    // The attempt ended. Resolve, whether or not a completion event was seen.
    assert(ns2_bt_pairing_deferral_resolved(true, false, 1000u, 1001u, bound));

    // The attempt never leaves flight. The bound resolves it anyway -- this is
    // the case the hardware hit.
    assert(ns2_bt_pairing_deferral_resolved(true, true, 1000u, 13000u, bound));
    assert(ns2_bt_pairing_deferral_resolved(true, true, 1000u, 60000u, bound));

    // A run-loop clock wrap must not extend a deferral into effective
    // permanence, nor cut one short: unsigned arithmetic keeps the elapsed
    // time honest across the 2^32 ms boundary.
    const uint32_t before_wrap = 0xFFFFF000u;
    assert(ns2_bt_pairing_deferral_resolved(
        true, true, before_wrap, before_wrap + bound + 1000u, bound));
    assert(!ns2_bt_pairing_deferral_resolved(
        true, true, before_wrap, before_wrap + bound - 1000u, bound));
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

/*
 * A first Classic pairing must become DURABLE, whichever end drove security.
 *
 * The defect this pins: the key commit waited for HCI_Authentication_Complete,
 * which is only generated in response to this host's own
 * HCI_Authentication_Requested. BTstack's HID Host registers LEVEL_0 and this
 * firmware only requests authentication for the Wiimote family and one named
 * Classic device, so a DualSense -- which drives SSP itself -- produced Link
 * Key Notification and Encryption Change and no Authentication Complete ever.
 * The notified key was parked, BTstack's own stored copy was deleted to stop an
 * unadmitted replacement, and nothing ever committed it. The controller worked
 * for that session and could never reconnect without the pairing window.
 */
/*
 * One inquiry result must never restart a connection already under way.
 *
 * Hardware, 2026-08-29 (build a05083ec): a single DualSense pairing recorded
 * inquiry_start three times between acl_up and Encryption Change. Each of those
 * rediscoveries re-admitted the device being connected, and re-admission
 * rebuilds the candidate from the new result -- overwriting the pending name
 * (frequently empty, since an EIR need not repeat it) that the connection slot
 * and driver match are built from, clearing the parked link key, and starting a
 * second HID connection.
 *
 * The session ended with all three symptoms together: empty link-key database,
 * generic classification, and no vendor-driver initialisation -- so no
 * player-slot LED and no configured colour. They are ONE defect, not three.
 */
static void test_a_live_classic_attempt_is_not_readmitted(void)
{
    // The regression: an OUTGOING attempt in flight to this same device. The
    // old guard recognised only an incoming duplicate, so this returned false
    // and the candidate was rebuilt from scratch mid-pairing.
    assert(ns2_bt_classic_inquiry_admission_is_duplicate(true, true, false));

    // Direction is irrelevant, and so is which stage the attempt has reached:
    // before the ACL exists the pending record is the evidence, after HID open
    // clears that record the ACL is.
    assert(ns2_bt_classic_inquiry_admission_is_duplicate(false, false, true));
    assert(ns2_bt_classic_inquiry_admission_is_duplicate(true, true, true));

    // A DIFFERENT device seen while one attempt is in flight is not a duplicate;
    // suppressing it would make a second controller undiscoverable.
    assert(!ns2_bt_classic_inquiry_admission_is_duplicate(true, false, false));

    // Nothing in flight and no link: an ordinary first admission, and the retry
    // path after a failed attempt, both still proceed.
    assert(!ns2_bt_classic_inquiry_admission_is_duplicate(false, false, false));

    // A stale pending record for another device must not gate this one.
    assert(!ns2_bt_classic_inquiry_admission_is_duplicate(false, true, false));
}

static void test_classic_authentication_proof_sources(void)
{
    // Neither event observed: nothing is proven, and the key stays uncommitted.
    assert(!ns2_bt_classic_authentication_proven(false, false));
    assert(!ns2_bt_classic_key_commit_allowed(
        false, ns2_bt_classic_authentication_proven(false, false), true, true));

    // This host drove authentication: the local event proves it.
    assert(ns2_bt_classic_authentication_proven(true, false));

    // The PEER drove it: encryption enabled is equally conclusive, because a
    // Classic link cannot be encrypted except with a link key both ends hold
    // and have authenticated against. This is the case that never committed.
    assert(ns2_bt_classic_authentication_proven(false, true));
    assert(ns2_bt_classic_key_commit_allowed(
        false, ns2_bt_classic_authentication_proven(false, true), true, true));

    // Both observed is still just proven, not doubly so.
    assert(ns2_bt_classic_authentication_proven(true, true));

    // Proof is NOT admission. An unadmitted key stays uncommitted however
    // convincingly its pairing succeeded, and the post-wipe lockout still wins.
    assert(!ns2_bt_classic_key_commit_allowed(
        false, ns2_bt_classic_authentication_proven(false, true), true, false));
    assert(!ns2_bt_classic_key_commit_allowed(
        true, ns2_bt_classic_authentication_proven(true, true), true, true));

    // ...and proof without a parked key commits nothing.
    assert(!ns2_bt_classic_key_commit_allowed(
        false, ns2_bt_classic_authentication_proven(true, true), false, true));
}

/*
 * The commit must converge for every legal ordering of the two proof events and
 * the notification, because BTstack guarantees no single order here.
 */
static void test_classic_first_pairing_orderings(void)
{
    // Each case: (notified+admitted, local auth ok, encryption ok) applied in
    // some order. The commit predicate is order-free by construction -- it reads
    // accumulated state -- so what is pinned is that every legal combination
    // that includes a proof commits, and no combination without one does.
    struct {
        bool local_auth;
        bool encrypted;
        bool expect_commit;
        const char *order;
    } cases[] = {
        { true,  false, true,  "notify -> auth complete (host-driven pairing)" },
        { false, true,  true,  "notify -> encryption change (peer-driven SSP)" },
        { true,  true,  true,  "notify -> auth complete -> encryption change" },
        { false, true,  true,  "encryption change -> notify (late key change)" },
        { false, false, false, "notify only, link drops before either proof" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        bool proven = ns2_bt_classic_authentication_proven(cases[i].local_auth,
                                                           cases[i].encrypted);
        bool committed = ns2_bt_classic_key_commit_allowed(false, proven,
                                                            true, true);
        assert(committed == cases[i].expect_commit);
    }

    // A remembered Classic controller reconnecting with the pairing window
    // CLOSED is admitted on its stored key alone, and its peer-led re-encryption
    // is enough to keep that key maintained. This is the reconnect the defect
    // made impossible.
    assert(ns2_bt_admission_decide(false, false, true) ==
           NS2_BT_ADMISSION_RECONNECT);
    assert(ns2_bt_classic_key_update_admitted(false, true, false,
                                               true, true, false));
    assert(ns2_bt_classic_key_commit_allowed(
        false, ns2_bt_classic_authentication_proven(false, true), true, true));

    // An UNKNOWN Classic controller with the window closed is still refused --
    // accepting peer-led proof must not become a way in.
    assert(ns2_bt_admission_decide(false, false, false) ==
           NS2_BT_ADMISSION_REJECT);
    assert(!ns2_bt_classic_key_update_admitted(false, true, false,
                                                false, false, false));

    // ...and the pairing window is what admits a genuinely new one.
    assert(ns2_bt_admission_decide(false, true, false) ==
           NS2_BT_ADMISSION_FRESH);
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

    /*
     * NO automatic recovery path may delete a durable bond because
     * authentication failed. Not for 0x05 Authentication Failure, and -- since
     * 2026-08-28 -- not for 0x06 PIN_OR_KEY_MISSING either.
     *
     * 0x06 used to qualify, on the reasoning that it specifically means the
     * peer no longer holds the relationship. That is a correct reading of the
     * status and the wrong conclusion about what to do: it is still a report
     * from the far end of a failed radio link, and acting on it destroys a
     * pairing the user made, silently and permanently.
     *
     * An adapter holding three bonds reported `btbonds: []` in the same powered
     * session with no reflash and no power cycle. Each 0x05/0x06 recovery site
     * deletes one, and there were enough of them across LE disconnect, LE
     * re-encryption and Classic authentication to account for all three. The
     * trigger was never proven; the response did not need proving to be wrong.
     *
     * Every status, including ones the spec has not assigned, must answer false.
     */
    for (unsigned status = 0; status <= 0xFF; ++status) {
        assert(!ns2_bt_classic_auth_failure_forgets_existing((uint8_t)status));
    }
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
    // Same peer, live encrypted management session: the state that otherwise
    // rejected every Controller Link attempt for good.
    assert(ns2_bt_classic_trust_present(false, true));
    assert(ns2_bt_admission_decide(false, false,
                                   ns2_bt_classic_trust_present(false, true)) ==
           NS2_BT_ADMISSION_RECONNECT);

    // A Classic link key alone still suffices: controllers are unaffected.
    assert(ns2_bt_classic_trust_present(true, false));
    assert(ns2_bt_admission_decide(false, false,
                                   ns2_bt_classic_trust_present(true, false)) ==
           NS2_BT_ADMISSION_RECONNECT);

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
    assert(ns2_bt_admission_decide(true, true,
                                   ns2_bt_classic_trust_present(true, true)) ==
           NS2_BT_ADMISSION_REJECT);
}

/*
 * The identity binding. "A trusted LE relationship satisfies Classic reconnect
 * trust" must mean the SAME peer, proven cryptographically -- never "some
 * companion is bonded", and never a bare address match, which any device can
 * claim by setting its BD_ADDR.
 */
static void test_companion_session_trust_is_peer_bound(void)
{
    // The legitimate case: connected, address known, address matches, and the
    // session is bonded and encrypted.
    assert(ns2_bt_companion_session_trust(true, true, true, true));

    // An UNRELATED peer that happens to arrive while our companion holds a
    // perfectly good session gets nothing from it. This is the exact failure
    // the contract exists to prevent.
    assert(!ns2_bt_companion_session_trust(true, true, false, true));

    // Address match WITHOUT cryptographic proof is a spoofable claim.
    assert(!ns2_bt_companion_session_trust(true, true, true, false));

    // A stored bond is not a live proof: with no session connected there is
    // nothing to have proven anything.
    assert(!ns2_bt_companion_session_trust(false, true, true, true));

    // Never compare against an address we never captured.
    assert(!ns2_bt_companion_session_trust(true, false, true, true));

    // Every condition is load-bearing: dropping any single one must not admit.
    assert(!ns2_bt_companion_session_trust(false, false, false, false));

    // And the composition an attacker would need: an unrelated peer cannot
    // reach RECONNECT even while a genuine companion session is live.
    bool impostor = ns2_bt_companion_session_trust(true, true, false, true);
    assert(ns2_bt_admission_decide(false, false,
                                   ns2_bt_classic_trust_present(false, impostor)) ==
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
 * Regression, 2026-08-22. The adapter is the LE peripheral on the management
 * link and asked for nothing, so the phone's supervision timeout alone decided
 * how long a peer stall had to be to kill the session -- while the Classic link
 * beside it carries 20 s. This pins the margin against that asymmetry.
 */
static void test_mgmt_link_params_ride_through_peer_stalls(void)
{
    ns2_bt_le_link_params_t p = ns2_bt_mgmt_link_params();

    // The controller must accept what we ask for.
    assert(ns2_bt_le_link_params_valid(p));

    // JoypadOS moved this class from 2 s to 6 s (efa0202) under single-radio
    // LE+Classic coexistence. Dropping back toward the phone's ~2 s default
    // restores the asymmetry this exists to remove.
    assert(p.supervision_timeout_units * 10u >= 6000u);

    // Latency must stay 0: a skipped connection event is dead time added on top
    // of whatever the peer is already doing.
    assert(p.latency == 0u);

    // The interval floor stays at the spec minimum. Raising it would slow the
    // link to buy margin the timeout already buys, and would tax bulk
    // management transfers for a reason no evidence supports.
    assert(p.interval_min_units == 6u);
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

/*
 * Regression, 2026-08-22 (Type C). Captured 8 times in a 25-cycle campaign:
 * authentication succeeds, Android sends SET_CONNECTION_ENCRYPTION, and ~7 ms
 * later "Encryption failure 35" = HCI_ERR_LMP_ERR_TRANS_COLLISION, after which
 * Android drops the ACL and the Controller Link never comes up.
 *
 * Cause: BTstack queues its own encryption request on every successful
 * Authentication Complete (hci.c:4240), unconditionally. Both controllers
 * report that event, so both hosts start the same LMP procedure.
 */
static void test_defer_classic_encryption_is_narrow(void)
{
    // The captured case: the companion reconnecting with a stored link key.
    // Note the second argument is "do we own a FRESH PAIRING", not "did we
    // request security at all" -- on a reconnect this host also calls
    // gap_request_security_level(LEVEL_2), so the broader reading would be true
    // here and would make the stand-down dead code.
    assert(ns2_bt_defer_classic_encryption(true, false));

    // A fresh pairing is ours to finish: we are driving the whole security
    // establishment, so we must not hand encryption to the peer mid-way.
    assert(!ns2_bt_defer_classic_encryption(true, true));

    // Controllers are untouched. This must never become "never encrypt": every
    // non-companion peer keeps BTstack's automatic behaviour exactly as before.
    assert(!ns2_bt_defer_classic_encryption(false, false));
    assert(!ns2_bt_defer_classic_encryption(false, true));

    // The companion predicate is the same proven-session one used for
    // admission, so an impostor cannot reach the deferral either.
    bool impostor = ns2_bt_companion_session_trust(true, true, false, true);
    assert(!ns2_bt_defer_classic_encryption(impostor, false));
}

/*
 * Standing down is only correct if the peer finishes the job, so the two
 * outcomes must stay distinguishable. Getting this wrong would let an ordinary
 * encrypted reconnect masquerade as proof the mechanism worked.
 */
static void test_encryption_outcome_classification(void)
{
    // The collision itself, on any link -- it can arrive where we never deferred.
    assert(ns2_bt_encryption_collision(0x23u));
    assert(!ns2_bt_encryption_collision(0x00u));
    assert(!ns2_bt_encryption_collision(0x05u));  // auth failure is not a collision
    assert(!ns2_bt_encryption_collision(0x06u));  // nor is PIN/key missing

    // Peer-led completion, attributable only on the deferral handle.
    assert(ns2_bt_encryption_completed_for_deferral(0x00u, true, true));
    assert(!ns2_bt_encryption_completed_for_deferral(0x00u, true, false));

    // "Encryption off" is not completion, even on the right handle -- that is
    // the peer turning encryption OFF, which must never count as success.
    assert(!ns2_bt_encryption_completed_for_deferral(0x00u, false, true));

    // A failed encryption change is never completion.
    assert(!ns2_bt_encryption_completed_for_deferral(0x23u, true, true));
    assert(!ns2_bt_encryption_completed_for_deferral(0x05u, true, true));

    // A collision and a completion are mutually exclusive for any input.
    for (unsigned s = 0; s < 256u; s++) {
        for (int en = 0; en < 2; en++) {
            for (int m = 0; m < 2; m++) {
                bool coll = ns2_bt_encryption_collision((uint8_t)s);
                bool done = ns2_bt_encryption_completed_for_deferral(
                    (uint8_t)s, en != 0, m != 0);
                assert(!(coll && done));
            }
        }
    }
}

/*
 * Event-order and stale-state invariants around companion Classic security.
 * The firmware tracks two per-handle values (the handle we requested security
 * on, and the handle we stood down on); handles are reused, so neither may
 * survive its connection. This models those transitions against the real
 * policy functions.
 */
#define NO_HANDLE 0xFFFFu
static void test_classic_security_event_orders(void)
{
    unsigned requested = NO_HANDLE, deferred = NO_HANDLE;
    const unsigned H1 = 0x0005u, H2 = 0x000Bu;

    // --- Peer-led success: auth complete -> defer -> peer encryption ON ------
    bool defer = ns2_bt_defer_classic_encryption(
        ns2_bt_companion_session_trust(true, true, true, true),
        requested == H1);
    assert(defer);
    deferred = H1;
    assert(ns2_bt_encryption_completed_for_deferral(0x00u, true, deferred == H1));

    // --- Disconnect must clear per-handle state -----------------------------
    if (deferred == H1) deferred = NO_HANDLE;
    if (requested == H1) requested = NO_HANDLE;
    // A late Encryption Change for the dead handle is no longer attributable.
    assert(!ns2_bt_encryption_completed_for_deferral(0x00u, true, deferred == H1));

    // --- Handle reuse: a new connection must not inherit the old decision ---
    deferred = NO_HANDLE;
    assert(!ns2_bt_encryption_completed_for_deferral(0x00u, true, deferred == H2));

    // --- We requested security ourselves: never defer -----------------------
    requested = H2;
    assert(!ns2_bt_defer_classic_encryption(
        ns2_bt_companion_session_trust(true, true, true, true), requested == H2));

    // --- Management session dropped before Classic auth completes -----------
    // Trust is live state, so it evaluates false and we simply do not defer:
    // BTstack's own encryption request stands, which is the safe direction.
    requested = NO_HANDLE;
    assert(!ns2_bt_defer_classic_encryption(
        ns2_bt_companion_session_trust(false, false, false, false),
        requested == H1));

    // --- Wrong peer while a genuine companion session is live ---------------
    assert(!ns2_bt_defer_classic_encryption(
        ns2_bt_companion_session_trust(true, true, false, true),
        requested == H1));

    // --- Peer encryption fails: must not look like success ------------------
    deferred = H1;
    assert(!ns2_bt_encryption_completed_for_deferral(0x23u, true, deferred == H1));
    assert(ns2_bt_encryption_collision(0x23u));

    // --- Duplicate Encryption Change after attribution ----------------------
    // The firmware clears the deferral handle on the first success, so a repeat
    // cannot be counted twice.
    deferred = H1;
    assert(ns2_bt_encryption_completed_for_deferral(0x00u, true, deferred == H1));
    deferred = NO_HANDLE;
    assert(!ns2_bt_encryption_completed_for_deferral(0x00u, true, deferred == H1));
}

/*
 * Regression, 2026-08-22 (Type C, authentication half). Captured across 20
 * companion reconnects on one build: six attempts logged
 * `btm_sec_auth_complete: ... status: 35` (0x23, LMP Error Transaction
 * Collision) and recovered, and two landed the same collision on encryption,
 * where Android disconnects the ACL instead of retrying.
 *
 * Both hosts were starting the LMP authentication procedure: this host calls
 * gap_request_security_level(LEVEL_2) on an incoming link with a stored key,
 * and Android's HID Device profile starts it too. Standing down from the
 * encryption request alone could not prevent that, because the redundant
 * request that races is the authentication one.
 */
static void test_defer_classic_authentication_is_narrow(void)
{
    // The captured case: companion, stored key, not our fresh pairing.
    assert(ns2_bt_defer_classic_authentication(true, true, false));

    // No stored key means there is nothing to authenticate against, and this
    // host must drive the fresh pairing itself.
    assert(!ns2_bt_defer_classic_authentication(true, false, false));

    // A fresh pairing we own stays ours to complete.
    assert(!ns2_bt_defer_classic_authentication(true, true, true));

    // Physical controllers and unknown peers are untouched in every
    // combination -- they keep BTstack's behaviour exactly as before.
    for (int key = 0; key < 2; key++)
        for (int fresh = 0; fresh < 2; fresh++)
            assert(!ns2_bt_defer_classic_authentication(false, key != 0, fresh != 0));

    // An impostor cannot reach it either: the companion predicate is the same
    // proven-live-session one used for admission and the encryption stand-down.
    bool impostor = ns2_bt_companion_session_trust(true, true, false, true);
    assert(!ns2_bt_defer_classic_authentication(impostor, true, false));
}

/*
 * Standing down changes WHO INITIATES, never WHAT IS REQUIRED. The required key
 * size is stated once and shared with the firmware's HID-ready gate, so an edit
 * cannot quietly relax the acceptance invariant while leaving the stand-down in
 * place.
 */
static void test_required_key_size_is_not_relaxed(void)
{
    assert(NS2_BT_REQUIRED_CLASSIC_KEY_SIZE == 16u);
}

/*
 * Controller Link is not a standalone transport.
 *
 * The Android companion's Classic HID link is a facility of a live management
 * session. Admitting it without one is not merely off-architecture: the peer is
 * then unrecognisable as the companion (companion trust is live state), so it
 * falls through into the ordinary physical-controller security path where this
 * host starts the same LMP authentication Android's HID Device profile starts --
 * the captured 0x23 dual-initiation collision.
 */
static void test_controller_link_requires_management(void)
{
    // The companion with its management session live: admitted, as today.
    assert(ns2_bt_companion_classic_admission_allowed(true, true));

    // The same companion with management down: refused before an ACL exists.
    assert(!ns2_bt_companion_classic_admission_allowed(true, false));

    // Physical controllers are single-transport, so the first input is false
    // for every one of them and admission is unchanged either way. This is the
    // guarantee that the rule cannot reach a controller.
    for (int trusted = 0; trusted < 2; trusted++)
        assert(ns2_bt_companion_classic_admission_allowed(false, trusted != 0));

    // The live-session input is the same peer-bound predicate used by the
    // stand-downs, so an address-spoofing impostor cannot buy admission with it.
    bool impostor = ns2_bt_companion_session_trust(true, true, false, true);
    assert(!ns2_bt_companion_classic_admission_allowed(true, impostor));

    // And the refusal must not be reachable by an unencrypted management link:
    // bonded-but-not-encrypted is not trust.
    bool unencrypted = ns2_bt_companion_session_trust(true, true, true, false);
    assert(!ns2_bt_companion_classic_admission_allowed(true, unencrypted));
}

/*
 * Security must be judged from state that exists on the peer-led path.
 *
 * HCI_Authentication_Complete is generated in response to this host's own
 * HCI_Authentication_Requested. When the authentication stand-down declines to
 * send that command -- the intended behaviour on every companion reconnect --
 * the event never arrives. Recording that as `auth_completed_ok = false` made a
 * correct, encrypted, fully authenticated link look like an authentication
 * failure, and it would make an acceptance run demand an event that structurally
 * no longer occurs.
 *
 * Observed on hardware 2026-08-23: 50 consecutive companion links reported
 * auth.deferrals incrementing, enc.deferrals staying 0, and the old
 * auth_completed_ok field reading false on every one of them, while the links
 * were in fact encrypted and HID-ready.
 */
static void test_peer_led_security_is_judged_on_observable_state(void)
{
    // The intended reconnect path: nothing observed locally, but encryption and
    // key size were positively confirmed at the acceptance gate.
    assert(ns2_bt_companion_security_satisfied(NS2_BT_AUTH_NOT_OBSERVED, true,
                                               true, 16u, true));

    // A locally observed success is equally acceptable (fresh-pair path).
    assert(ns2_bt_companion_security_satisfied(NS2_BT_AUTH_OBSERVED_OK, true,
                                               true, 16u, true));

    // An observed FAILURE is never acceptable, however good the rest looks.
    assert(!ns2_bt_companion_security_satisfied(NS2_BT_AUTH_OBSERVED_FAILED, true,
                                                true, 16u, true));

    // Not observing authentication must not become a licence to skip the parts
    // that ARE observable.
    assert(!ns2_bt_companion_security_satisfied(NS2_BT_AUTH_NOT_OBSERVED, false,
                                                true, 16u, true));   // unencrypted
    assert(!ns2_bt_companion_security_satisfied(NS2_BT_AUTH_NOT_OBSERVED, true,
                                                true, 15u, true));   // short key
    assert(!ns2_bt_companion_security_satisfied(NS2_BT_AUTH_NOT_OBSERVED, true,
                                                true, 16u, false));  // no HID

    // An unsampled key size is not a passing key size. This is the specific
    // trap the old telemetry fell into: gap_encryption_key_size() reads 0 in the
    // Encryption Change handler because BTstack has not issued
    // HCI_Read_Encryption_Key_Size yet, so "0" meant "not asked", not "weak".
    // Treating not-asked as satisfied would silently drop the invariant.
    assert(!ns2_bt_companion_security_satisfied(NS2_BT_AUTH_NOT_OBSERVED, true,
                                                false, 0u, true));
    assert(!ns2_bt_companion_security_satisfied(NS2_BT_AUTH_NOT_OBSERVED, true,
                                                false, 16u, true));

    // The names are part of the wire format the harness parses.
    assert(strcmp(ns2_bt_auth_observation_name(NS2_BT_AUTH_NOT_OBSERVED),
                  "not_observed") == 0);
    assert(strcmp(ns2_bt_auth_observation_name(NS2_BT_AUTH_OBSERVED_OK),
                  "observed_ok") == 0);
    assert(strcmp(ns2_bt_auth_observation_name(NS2_BT_AUTH_OBSERVED_FAILED),
                  "observed_failed") == 0);
}

// BTstack 1.8.2 stopped synthesising a disconnection-complete event for an
// already-released handle. Pin which statuses still leave one in flight, so a
// caller cannot go back to assuming the event always arrives -- nor start
// converging on a handle whose event is genuinely still coming.
static void test_disconnect_convergence_after_btstack_1_8(void)
{
    // Requested now: the event is what completes the teardown.
    assert(ns2_bt_disconnect_outcome(0x00u) == NS2_BT_DISCONNECT_EVENT_PENDING);

    // Already requested or already sent. Converging here would run teardown
    // twice: once now and once when the real event lands.
    assert(ns2_bt_disconnect_outcome(NS2_BT_HCI_COMMAND_DISALLOWED) ==
           NS2_BT_DISCONNECT_EVENT_PENDING);

    // The 1.8.2 behaviour change itself. 1.6.2 answered this case with a
    // synthetic event and status 0; there is no event now.
    assert(ns2_bt_disconnect_outcome(NS2_BT_HCI_UNKNOWN_CONNECTION_IDENTIFIER) ==
           NS2_BT_DISCONNECT_CONVERGE_LOCALLY);

    // Fail safe: an unrecognised status is not a promise of an event.
    for (unsigned status = 0x01u; status <= 0xFFu; ++status) {
        if (status == NS2_BT_HCI_COMMAND_DISALLOWED)
            continue;
        assert(ns2_bt_disconnect_outcome((uint8_t)status) ==
               NS2_BT_DISCONNECT_CONVERGE_LOCALLY);
    }
}

int main(void)
{
    test_disconnect_convergence_after_btstack_1_8();
    test_peer_led_security_is_judged_on_observable_state();
    test_controller_link_requires_management();
    test_defer_classic_authentication_is_narrow();
    test_required_key_size_is_not_relaxed();
    test_encryption_outcome_classification();
    test_classic_security_event_orders();
    test_defer_classic_encryption_is_narrow();
    test_mgmt_link_params_ride_through_peer_stalls();
    test_le_link_params_validity_rules();
    test_classic_trust_is_cross_transport();
    test_companion_session_trust_is_peer_bound();
    test_inquiry_restart_gap();
    test_pairing_admission();
    test_pairing_deferral_cannot_latch();
    test_boot_lockout();
    test_install_reset_bootstrap_is_one_shot();
    test_classic_ssp_attempt_admission();
    test_a_live_classic_attempt_is_not_readmitted();
    test_classic_authentication_proof_sources();
    test_classic_first_pairing_orderings();
    test_classic_key_replacement();
    test_switch2_custom_admission();
    test_typed_forget_scope();
    test_sparse_slot_lookup();
    puts("Bluetooth lifecycle policy tests passed");
    return 0;
}
