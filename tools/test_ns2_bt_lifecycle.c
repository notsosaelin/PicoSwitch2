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

int main(void)
{
    test_pairing_admission();
    test_boot_lockout();
    test_classic_key_replacement();
    test_switch2_custom_admission();
    test_typed_forget_scope();
    test_sparse_slot_lookup();
    puts("Bluetooth lifecycle policy tests passed");
    return 0;
}
