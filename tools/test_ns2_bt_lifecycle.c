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
    test_sparse_slot_lookup();
    puts("Bluetooth lifecycle policy tests passed");
    return 0;
}
