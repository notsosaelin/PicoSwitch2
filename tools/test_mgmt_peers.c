/*
 * Host coverage for the logical peer inventory: role ordering, cross-transport
 * merging, name sanitisation, address-stable pagination, and the absence of any
 * secret material on the wire.
 *
 * Independent of BTstack on purpose. The firmware glue reads the two security
 * databases and hands this module non-secret identity; everything that can be
 * got wrong after that point is decided here, so it can be pinned without a
 * radio.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude \
 *   tools/test_mgmt_peers.c src/mgmt_peers.c \
 *   -o build/host-tests/test_mgmt_peers.exe
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mgmt_peers.h"

static mgmt_peer_bond_t classic_bond(uint8_t last, uint8_t key_type)
{
    mgmt_peer_bond_t bond;
    memset(&bond, 0, sizeof(bond));
    bond.address[0] = 0xAA;
    bond.address[5] = last;
    bond.transport = MGMT_PEER_TRANSPORT_BREDR;
    bond.le_slot = -1;
    bond.classic_key_type = key_type;
    return bond;
}

static mgmt_peer_bond_t le_bond(uint8_t last, int16_t slot, uint8_t addr_type)
{
    mgmt_peer_bond_t bond;
    memset(&bond, 0, sizeof(bond));
    bond.address[0] = 0xAA;
    bond.address[5] = last;
    bond.transport = MGMT_PEER_TRANSPORT_LE;
    bond.le_slot = slot;
    bond.le_address_type = addr_type;
    return bond;
}

static mgmt_peer_observation_t observation(uint8_t last, const char *name)
{
    mgmt_peer_observation_t o;
    memset(&o, 0, sizeof(o));
    o.address[0] = 0xAA;
    o.address[5] = last;
    if (name)
        snprintf(o.name, sizeof(o.name), "%s", name);
    return o;
}

/* ------------------------------------------------------------------ roles */

static void test_role_order_puts_management_first(void)
{
    // The case this ordering exists for: one phone that is simultaneously the
    // management companion and the Controller Link peer. Calling it a
    // controller is the specific misclassification the gate forbids.
    mgmt_peer_observation_t both = observation(0x01, "Phone");
    both.is_management_client = 1;
    both.is_bridge_source = 1;
    both.is_direct_source = 1;
    assert(mgmt_peers_classify(&both) == MGMT_PEER_ROLE_MANAGEMENT_COMPANION);

    mgmt_peer_observation_t bridge = observation(0x01, "Phone");
    bridge.is_bridge_source = 1;
    bridge.is_direct_source = 1;
    assert(mgmt_peers_classify(&bridge) == MGMT_PEER_ROLE_CONTROLLER_LINK);

    mgmt_peer_observation_t pad = observation(0x01, "DualSense");
    pad.is_direct_source = 1;
    assert(mgmt_peers_classify(&pad) == MGMT_PEER_ROLE_PHYSICAL_CONTROLLER);

    mgmt_peer_observation_t remembered = observation(0x01, "DualSense");
    remembered.is_last_connected = 1;
    assert(mgmt_peers_classify(&remembered) == MGMT_PEER_ROLE_PHYSICAL_CONTROLLER);
}

static void test_unseen_peer_is_unknown_not_guessed(void)
{
    // A stored bond whose owner has not been seen since boot. The adapter has
    // no persistent role metadata, so the honest answer is "unknown" -- not
    // "controller" because most bonds are controllers.
    mgmt_peer_observation_t nothing = observation(0x01, NULL);
    assert(mgmt_peers_classify(&nothing) == MGMT_PEER_ROLE_UNKNOWN);
    assert(mgmt_peers_classify(NULL) == MGMT_PEER_ROLE_UNKNOWN);

    mgmt_peer_bond_t bonds[] = { classic_bond(0x01, 4) };
    mgmt_peer_t peers[4];
    size_t count = mgmt_peers_merge(bonds, 1, NULL, 0, peers, 4);
    assert(count == 1);
    assert(peers[0].role == MGMT_PEER_ROLE_UNKNOWN);
    assert(peers[0].connected == 0);
}

/* ------------------------------------------------------- cross-transport */

static void test_one_device_with_two_key_records_is_one_peer(void)
{
    // The project builds with cross-transport key derivation, and the
    // management phone routinely holds both a Classic and an LE record.
    // Presenting the records would show one device twice.
    mgmt_peer_bond_t bonds[] = {
        classic_bond(0x01, 4),
        le_bond(0x01, 3, 0),
    };
    mgmt_peer_t peers[4];
    size_t count = mgmt_peers_merge(bonds, 2, NULL, 0, peers, 4);
    assert(count == 1);
    assert(peers[0].transport == (MGMT_PEER_TRANSPORT_BREDR | MGMT_PEER_TRANSPORT_LE));
    assert(peers[0].le_slot == 3);
    assert(peers[0].classic_key_type == 4);
}

static void test_distinct_devices_stay_distinct(void)
{
    mgmt_peer_bond_t bonds[] = {
        classic_bond(0x02, 4),
        le_bond(0x01, 0, 0),
        classic_bond(0x03, 5),
    };
    mgmt_peer_t peers[8];
    size_t count = mgmt_peers_merge(bonds, 3, NULL, 0, peers, 8);
    assert(count == 3);
    // Sorted by address so a cursor means the same thing between calls.
    assert(peers[0].address[5] == 0x01);
    assert(peers[1].address[5] == 0x02);
    assert(peers[2].address[5] == 0x03);
}

static void test_sort_is_independent_of_enumeration_order(void)
{
    mgmt_peer_bond_t forward[] = {
        classic_bond(0x01, 4), classic_bond(0x02, 4), classic_bond(0x03, 4),
    };
    mgmt_peer_bond_t reverse[] = {
        classic_bond(0x03, 4), classic_bond(0x02, 4), classic_bond(0x01, 4),
    };
    mgmt_peer_t a[4];
    mgmt_peer_t b[4];
    assert(mgmt_peers_merge(forward, 3, NULL, 0, a, 4) == 3);
    assert(mgmt_peers_merge(reverse, 3, NULL, 0, b, 4) == 3);
    for (size_t i = 0; i < 3; ++i)
        assert(memcmp(a[i].address, b[i].address, 6) == 0);
}

static void test_connected_peer_without_a_key_is_still_reported(void)
{
    // A controller mid-pairing has no stored key yet. Hiding it would make the
    // inventory disagree with what the adapter is actually doing.
    mgmt_peer_observation_t live = observation(0x09, "New Pad");
    live.is_direct_source = 1;
    live.connected = 1;
    mgmt_peer_t peers[4];
    size_t count = mgmt_peers_merge(NULL, 0, &live, 1, peers, 4);
    assert(count == 1);
    assert(peers[0].transport == 0);
    assert(peers[0].connected == 1);
    assert(peers[0].role == MGMT_PEER_ROLE_PHYSICAL_CONTROLLER);
}

static void test_live_evidence_names_and_marks_a_stored_peer(void)
{
    mgmt_peer_bond_t bonds[] = { classic_bond(0x01, 4) };
    mgmt_peer_observation_t live = observation(0x01, "DualSense Wireless Controller");
    live.is_direct_source = 1;
    live.connected = 1;
    mgmt_peer_t peers[4];
    assert(mgmt_peers_merge(bonds, 1, &live, 1, peers, 4) == 1);
    assert(peers[0].role == MGMT_PEER_ROLE_PHYSICAL_CONTROLLER);
    assert(peers[0].connected == 1);
    assert(strncmp(peers[0].name, "DualSense", 9) == 0);
}

static void test_a_weaker_observation_cannot_demote_a_stronger_one(void)
{
    mgmt_peer_observation_t seen[2];
    seen[0] = observation(0x01, "Phone");
    seen[0].is_management_client = 1;
    seen[0].connected = 1;
    seen[1] = observation(0x01, "Phone");
    seen[1].is_direct_source = 1;
    mgmt_peer_t peers[4];
    assert(mgmt_peers_merge(NULL, 0, seen, 2, peers, 4) == 1);
    assert(peers[0].role == MGMT_PEER_ROLE_MANAGEMENT_COMPANION);
}

/* --------------------------------------------------- classification (Phase 4) */

static void test_classification_and_identifiers_survive_the_merge(void)
{
    mgmt_peer_bond_t bonds[] = { classic_bond(0x01, 4) };
    mgmt_peer_observation_t live = observation(0x01, "Wireless Controller");
    live.is_direct_source = 1;
    live.connected = 1;
    snprintf(live.classification, sizeof(live.classification), "Sony DualSense");
    live.vendor_id = 0x054C;
    live.product_id = 0x0CE6;
    mgmt_peer_t peers[4];
    assert(mgmt_peers_merge(bonds, 1, &live, 1, peers, 4) == 1);
    assert(strcmp(peers[0].classification, "Sony DualSense") == 0);
    assert(peers[0].vendor_id == 0x054C && peers[0].product_id == 0x0CE6);
}

static void test_a_later_observation_cannot_erase_a_known_classification(void)
{
    // Two live records for one device -- the case a composite source or a
    // reconnect mid-enumeration produces. Only one of them may have had a
    // driver bound, and the answer must not depend on which arrives last.
    mgmt_peer_observation_t seen[2];
    seen[0] = observation(0x01, "Pad");
    seen[0].is_direct_source = 1;
    snprintf(seen[0].classification, sizeof(seen[0].classification), "Sony DualSense");
    seen[0].vendor_id = 0x054C;
    seen[0].product_id = 0x0CE6;
    seen[1] = observation(0x01, "Pad");
    seen[1].is_direct_source = 1;
    mgmt_peer_t peers[4];
    assert(mgmt_peers_merge(NULL, 0, seen, 2, peers, 4) == 1);
    assert(strcmp(peers[0].classification, "Sony DualSense") == 0);
    assert(peers[0].vendor_id == 0x054C);
}

static void test_an_unclassified_peer_reports_nothing_rather_than_empty(void)
{
    mgmt_peer_bond_t bonds[] = { classic_bond(0x01, 4) };
    mgmt_peer_t peers[2];
    assert(mgmt_peers_merge(bonds, 1, NULL, 0, peers, 2) == 1);
    assert(peers[0].classification[0] == '\0');
    char out[MGMT_PEERS_RESPONSE_CAPACITY];
    mgmt_peers_page_info_t info;
    assert(mgmt_peers_format_page(peers, 1, 0, out, sizeof(out), &info) > 0);
    // Absent, not empty: the client must be able to tell "the adapter cannot
    // say" from "the adapter says it is called nothing".
    assert(strstr(out, "\"class\"") == NULL);
    assert(strstr(out, "\"vid\"") == NULL);
}

static void test_a_classification_is_sanitised_like_a_remote_name(void)
{
    // A driver name is firmware-controlled today, but it reaches the same JSON
    // string as an untrusted remote name and is treated identically so that a
    // future driver name sourced from a device cannot break the envelope.
    mgmt_peer_observation_t live = observation(0x01, "Pad");
    live.is_direct_source = 1;
    snprintf(live.classification, sizeof(live.classification), "Ev\"il\\pad");
    mgmt_peer_t peers[2];
    assert(mgmt_peers_merge(NULL, 0, &live, 1, peers, 2) == 1);
    assert(strchr(peers[0].classification, '"') == NULL);
    assert(strchr(peers[0].classification, '\\') == NULL);
    char out[MGMT_PEERS_RESPONSE_CAPACITY];
    mgmt_peers_page_info_t info;
    size_t length = mgmt_peers_format_page(peers, 1, 0, out, sizeof(out), &info);
    assert(length > 0 && info.complete);
}

static void test_a_widest_possible_row_still_fits_a_page(void)
{
    // The widest row the formatter can be asked to emit: a full-length name, a
    // full-length classification and both identifiers. If this cannot make
    // progress the pager fails closed and the whole inventory becomes
    // unreadable, so the row budget is pinned here rather than assumed.
    mgmt_peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.address[0] = 0xAA;
    peer.address[5] = 0x01;
    peer.transport = MGMT_PEER_TRANSPORT_BREDR | MGMT_PEER_TRANSPORT_LE;
    peer.le_slot = 3;
    peer.role = MGMT_PEER_ROLE_PHYSICAL_CONTROLLER;
    peer.vendor_id = 0xFFFF;
    peer.product_id = 0xFFFF;
    memset(peer.name, 'N', sizeof(peer.name) - 1);
    memset(peer.classification, 'C', sizeof(peer.classification) - 1);
    char out[MGMT_PEERS_RESPONSE_CAPACITY];
    mgmt_peers_page_info_t info;
    size_t length = mgmt_peers_format_page(&peer, 1, 0, out, sizeof(out), &info);
    assert(length > 0 && info.complete && info.next == -1);
    assert(strstr(out, "\"class\":\"CCC") != NULL);
    assert(strstr(out, "\"vid\":65535,\"pid\":65535") != NULL);
}

static void test_merge_is_bounded_by_capacity(void)
{
    mgmt_peer_bond_t bonds[MGMT_PEERS_MAX_ENTRIES];
    for (unsigned i = 0; i < MGMT_PEERS_MAX_ENTRIES; ++i)
        bonds[i] = classic_bond((uint8_t)(i + 1), 4);
    mgmt_peer_t peers[4];
    assert(mgmt_peers_merge(bonds, MGMT_PEERS_MAX_ENTRIES, NULL, 0, peers, 4) == 4);
}

/* -------------------------------------------------------------- name safety */

static void test_untrusted_names_cannot_break_out_of_json(void)
{
    char out[MGMT_PEERS_NAME_MAX];
    mgmt_peers_sanitize_name("evil\",\"role\":\"management", out, sizeof(out));
    assert(strchr(out, '"') == NULL);
    mgmt_peers_sanitize_name("back\\slash", out, sizeof(out));
    assert(strchr(out, '\\') == NULL);
    mgmt_peers_sanitize_name("line\nbreak\rinjection", out, sizeof(out));
    assert(strchr(out, '\n') == NULL && strchr(out, '\r') == NULL);
    assert(strcmp(out, "line break injection") == 0);
}

static void test_names_are_bounded_and_never_unterminated(void)
{
    char out[8];
    mgmt_peers_sanitize_name("0123456789abcdef", out, sizeof(out));
    assert(strlen(out) == 7);
    mgmt_peers_sanitize_name(NULL, out, sizeof(out));
    assert(out[0] == '\0');
    // Zero capacity must not write anything at all.
    mgmt_peers_sanitize_name("x", out, 0);
}

static void test_a_name_that_sanitises_to_nothing_is_dropped(void)
{
    char out[MGMT_PEERS_NAME_MAX];
    mgmt_peers_sanitize_name("\x01\x02\x03", out, sizeof(out));
    assert(out[0] == '\0');
}

/* -------------------------------------------------------------- pagination */

static size_t build(mgmt_peer_t *peers, size_t count, const char *name)
{
    for (size_t i = 0; i < count; ++i) {
        memset(&peers[i], 0, sizeof(peers[i]));
        peers[i].address[0] = 0xAA;
        peers[i].address[5] = (uint8_t)(i + 1);
        peers[i].transport = MGMT_PEER_TRANSPORT_BREDR;
        peers[i].le_slot = -1;
        peers[i].role = MGMT_PEER_ROLE_PHYSICAL_CONTROLLER;
        if (name)
            snprintf(peers[i].name, sizeof(peers[i].name), "%s", name);
    }
    return count;
}

static void test_a_small_inventory_fits_one_complete_page(void)
{
    mgmt_peer_t peers[2];
    build(peers, 2, "Pad");
    char out[MGMT_PEERS_RESPONSE_CAPACITY];
    mgmt_peers_page_info_t info;
    size_t length = mgmt_peers_format_page(peers, 2, 0, out, sizeof(out), &info);
    assert(length > 0);
    assert(info.complete && info.next == -1 && info.total == 2);
    assert(strstr(out, "\"v\":1") != NULL);
    assert(strstr(out, "\"total\":2") != NULL);
    assert(strstr(out, "\"next\":null") != NULL);
    assert(strstr(out, "\"role\":\"controller\"") != NULL);
}

static void test_a_full_inventory_pages_and_every_peer_appears_once(void)
{
    mgmt_peer_t peers[MGMT_PEERS_MAX_ENTRIES];
    size_t count = build(peers, MGMT_PEERS_MAX_ENTRIES,
                         "A Very Long Controller Name Here");
    char out[MGMT_PEERS_RESPONSE_CAPACITY];
    mgmt_peers_page_info_t info;

    int seen[MGMT_PEERS_MAX_ENTRIES + 1] = { 0 };
    int cursor = 0;
    int pages = 0;
    while (1) {
        size_t length = mgmt_peers_format_page(peers, count, cursor, out,
                                               sizeof(out), &info);
        assert(length > 0);
        assert(length < sizeof(out));
        assert(info.total == (int)count);
        ++pages;
        // Every page must make progress or a client loops on the cursor.
        assert(info.complete || info.next > cursor);
        for (size_t i = 0; i < count; ++i) {
            char id[16];
            mgmt_peers_format_id(peers[i].address, id, sizeof(id));
            char needle[24];
            snprintf(needle, sizeof(needle), "\"%s\"", id);
            if (strstr(out, needle))
                seen[i] += 1;
        }
        if (info.complete)
            break;
        cursor = info.next;
        assert(pages < 64);
    }
    for (size_t i = 0; i < count; ++i)
        assert(seen[i] == 1);
}

static void test_a_page_that_cannot_progress_fails_closed(void)
{
    mgmt_peer_t peers[1];
    build(peers, 1, "Pad");
    char tiny[48];
    mgmt_peers_page_info_t info;
    // Not even one peer fits: an empty successful page would leave a client
    // retrying the same cursor forever.
    assert(mgmt_peers_format_page(peers, 1, 0, tiny, sizeof(tiny), &info) == 0);
    assert(!info.complete);
}

static void test_an_out_of_range_cursor_is_rejected(void)
{
    mgmt_peer_t peers[2];
    build(peers, 2, NULL);
    char out[MGMT_PEERS_RESPONSE_CAPACITY];
    mgmt_peers_page_info_t info;
    assert(mgmt_peers_format_page(peers, 2, -1, out, sizeof(out), &info) == 0);
    assert(mgmt_peers_format_page(peers, 2, 3, out, sizeof(out), &info) == 0);
    // A cursor exactly at the end is an empty but complete page, not an error.
    assert(mgmt_peers_format_page(peers, 2, 2, out, sizeof(out), &info) > 0);
    assert(info.complete);
}

static void test_an_empty_inventory_is_a_complete_page(void)
{
    char out[MGMT_PEERS_RESPONSE_CAPACITY];
    mgmt_peers_page_info_t info;
    assert(mgmt_peers_format_page(NULL, 0, 0, out, sizeof(out), &info) > 0);
    assert(info.complete && info.total == 0);
    assert(strstr(out, "\"peers\":[]") != NULL);
}

/* -------------------------------------------------------------- identity */

static void test_peer_ids_are_stable_opaque_and_not_slot_indices(void)
{
    uint8_t a[6] = { 0xAA, 0, 0, 0, 0, 0x01 };
    uint8_t b[6] = { 0xAA, 0, 0, 0, 0, 0x02 };
    char first[16];
    char again[16];
    char other[16];
    mgmt_peers_format_id(a, first, sizeof(first));
    mgmt_peers_format_id(a, again, sizeof(again));
    mgmt_peers_format_id(b, other, sizeof(other));
    assert(strcmp(first, again) == 0);
    assert(strcmp(first, other) != 0);
    assert(strncmp(first, "p_", 2) == 0);
    assert(strlen(first) == 10);
    // Too small to hold an id: must produce an empty string, never a partial one.
    char tiny[4];
    mgmt_peers_format_id(a, tiny, sizeof(tiny));
    assert(tiny[0] == '\0');
}

/* ---------------------------------------------------------------- command */

static void test_command_parsing(void)
{
    mgmt_peers_action_t action;
    int cursor = -1;

    assert(mgmt_peers_parse_command("list", &action, &cursor));
    assert(action == MGMT_PEERS_LIST && cursor == 0);
    assert(mgmt_peers_parse_command("list 7", &action, &cursor));
    assert(action == MGMT_PEERS_LIST && cursor == 7);

    assert(!mgmt_peers_parse_command("", &action, &cursor));
    assert(!mgmt_peers_parse_command("list -1", &action, &cursor));
    assert(!mgmt_peers_parse_command("list x", &action, &cursor));
    assert(!mgmt_peers_parse_command("list 999999999999", &action, &cursor));
    assert(!mgmt_peers_parse_command("forget 1", &action, &cursor));
    assert(!mgmt_peers_parse_command(NULL, &action, &cursor));
}

/* ------------------------------------------------------------ no secrets */

static void test_no_page_can_contain_key_material(void)
{
    // Structural, not textual: mgmt_peer_t has nowhere to put a key, so a page
    // cannot carry one. This asserts the shape that makes that true, so a
    // future field addition has to come past this test.
    assert(sizeof(((mgmt_peer_t *)0)->address) == 6);
    mgmt_peer_t peers[2];
    build(peers, 2, "Pad");
    char out[MGMT_PEERS_RESPONSE_CAPACITY];
    mgmt_peers_page_info_t info;
    assert(mgmt_peers_format_page(peers, 2, 0, out, sizeof(out), &info) > 0);
    // The only key-adjacent value on the wire is a link-key TYPE, and even that
    // is not emitted by this page format.
    assert(strstr(out, "key") == NULL);
    assert(strstr(out, "ltk") == NULL);
    assert(strstr(out, "irk") == NULL);
}

int main(void)
{
    test_role_order_puts_management_first();
    test_unseen_peer_is_unknown_not_guessed();
    test_one_device_with_two_key_records_is_one_peer();
    test_distinct_devices_stay_distinct();
    test_sort_is_independent_of_enumeration_order();
    test_connected_peer_without_a_key_is_still_reported();
    test_live_evidence_names_and_marks_a_stored_peer();
    test_a_weaker_observation_cannot_demote_a_stronger_one();
    test_classification_and_identifiers_survive_the_merge();
    test_a_later_observation_cannot_erase_a_known_classification();
    test_an_unclassified_peer_reports_nothing_rather_than_empty();
    test_a_classification_is_sanitised_like_a_remote_name();
    test_a_widest_possible_row_still_fits_a_page();
    test_merge_is_bounded_by_capacity();
    test_untrusted_names_cannot_break_out_of_json();
    test_names_are_bounded_and_never_unterminated();
    test_a_name_that_sanitises_to_nothing_is_dropped();
    test_a_small_inventory_fits_one_complete_page();
    test_a_full_inventory_pages_and_every_peer_appears_once();
    test_a_page_that_cannot_progress_fails_closed();
    test_an_out_of_range_cursor_is_rejected();
    test_an_empty_inventory_is_a_complete_page();
    test_peer_ids_are_stable_opaque_and_not_slot_indices();
    test_command_parsing();
    test_no_page_can_contain_key_material();
    printf("mgmt_peers: all tests passed\n");
    return 0;
}
