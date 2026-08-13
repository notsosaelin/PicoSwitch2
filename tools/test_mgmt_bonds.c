/*
 * Host coverage for bounded, versioned bond-list JSON serialization.
 * This is intentionally independent of BTstack: the firmware supplies the
 * same slot callback from its BTstack thread, while this fixture exercises
 * holes, pagination progress, exact-size bounds, and fail-closed overflow.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude \
 *   tools/test_mgmt_bonds.c src/mgmt_bonds.c \
 *   -o build/host-tests/test_mgmt_bonds.exe
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mgmt_bonds.h"

enum { SLOT_COUNT = 32 };

static bool fixture_entry_at(void *context, int slot, mgmt_bond_entry_t *entry)
{
    bool *present = context;
    if (!present || !entry || slot < 0 || slot >= SLOT_COUNT || !present[slot])
        return false;
    entry->index = slot;
    entry->type = slot & 3;
    for (unsigned i = 0; i < sizeof(entry->address); ++i)
        entry->address[i] = (uint8_t)(slot * 7 + (int)i);
    return true;
}

static int count_present(const bool *present)
{
    int count = 0;
    for (int i = 0; i < SLOT_COUNT; ++i)
        count += present[i] ? 1 : 0;
    return count;
}

static void test_legacy_complete_envelope(void)
{
    bool present[SLOT_COUNT] = {false};
    char output[MGMT_BONDS_RESPONSE_CAPACITY];
    bool complete = false;
    present[0] = true;
    present[3] = true;
    present[31] = true;
    size_t length = mgmt_bonds_format_legacy(
        fixture_entry_at, present, SLOT_COUNT, output, sizeof(output), &complete);
    assert(length == strlen(output));
    assert(length <= MGMT_BONDS_RESPONSE_CAPACITY - 1u);
    assert(complete);
    assert(strstr(output, "\"v\":2") != NULL);
    assert(strstr(output, "\"total\":3") != NULL);
    assert(strstr(output, "\"next\":null") != NULL);
    assert(strstr(output, "\"i\":31") != NULL);
}

static void test_pages_cover_every_slot_once(void)
{
    bool present[SLOT_COUNT] = {false};
    char output[MGMT_BONDS_RESPONSE_CAPACITY];
    bool seen[SLOT_COUNT] = {false};
    int cursor = 0;
    int expected_total;
    int pages = 0;

    // Include holes to prove that `next` is a DB-slot cursor, not an array
    // offset that can repeat or skip an entry.
    for (int i = 0; i < SLOT_COUNT; ++i)
        present[i] = (i % 3) != 1;
    expected_total = count_present(present);

    for (;;) {
        mgmt_bonds_page_info_t info;
        size_t length = mgmt_bonds_format_page(
            fixture_entry_at, present, SLOT_COUNT, cursor,
            output, sizeof(output), &info);
        assert(length > 0);
        assert(length == strlen(output));
        assert(length <= MGMT_BONDS_RESPONSE_CAPACITY - 1u);
        assert(info.total == expected_total);
        assert(strstr(output, "\"v\":2") != NULL);
        assert(strstr(output, "\"bonds\":[") != NULL);
        for (int slot = cursor; slot < SLOT_COUNT; ++slot) {
            char marker[24];
            (void)snprintf(marker, sizeof(marker), "\"i\":%d,\"", slot);
            if (strstr(output, marker) != NULL) {
                assert(present[slot]);
                assert(!seen[slot]);
                seen[slot] = true;
            }
        }
        ++pages;
        if (info.complete) {
            assert(strstr(output, "\"next\":null") != NULL);
            break;
        }
        assert(info.next > cursor);
        assert(info.next < SLOT_COUNT);
        cursor = info.next;
        assert(pages < SLOT_COUNT);
    }
    assert(pages > 1);
    for (int slot = 0; slot < SLOT_COUNT; ++slot)
        assert(seen[slot] == present[slot]);
}

static void test_legacy_overflow_fails_closed(void)
{
    bool present[SLOT_COUNT];
    char output[96];
    bool complete = true;
    memset(present, 1, sizeof(present));
    assert(mgmt_bonds_format_legacy(
               fixture_entry_at, present, SLOT_COUNT,
               output, sizeof(output), &complete) == 0);
    assert(!complete);
    assert(output[0] == '\0');
}

static void test_page_tiny_buffer_cannot_claim_progress(void)
{
    bool present[SLOT_COUNT] = {false};
    char output[16];
    mgmt_bonds_page_info_t info;
    present[0] = true;
    // The production capacity is 512; a deliberately tiny test buffer must
    // fail rather than emit a page whose cursor cannot advance.
    assert(mgmt_bonds_format_page(
               fixture_entry_at, present, SLOT_COUNT, 0,
               output, sizeof(output), &info) == 0);
    assert(!info.complete);
    assert(output[0] == '\0');
}

int main(void)
{
    test_legacy_complete_envelope();
    test_pages_cover_every_slot_once();
    test_legacy_overflow_fails_closed();
    test_page_tiny_buffer_cannot_claim_progress();
    puts("management bond serialization tests passed");
    return 0;
}
