// The live KB/M admission path, driven through the production transitions.
//
// ns2_kbm_runtime.c was previously reachable only on hardware, so its admission
// path -- the one stage between a decoded key and a published button -- had no
// test at all. That is exactly where a keyboard that classified correctly,
// admitted correctly and showed `keyboard=true` in `kbm status` still produced
// nothing at the console.
//
// This harness stubs the four things the runtime needs from the firmware (the
// bthid device table, the source arbiter, the report seam and the clock) and
// then calls the SAME entry points bthid.c calls, in the same order:
//
//     add_device            -> a live connection at a generation
//     set_descriptor        -> ns2_kbm_runtime_note_classification()
//     bthid_on_hid_rebind   -> ns2_kbm_runtime_note_ready()
//     process_report        -> ns2_kbm_runtime_submit_keyboard()
//
// Nothing here pokes runtime internals; every assertion is made through
// ns2_kbm_runtime_status(), which is the same view `kbm status` returns.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt/bthid/bthid.h"
#include "ns2_active_input.h"
#include "ns2_kbm_runtime.h"
#include "report.h"
#include "switch_pro.h"

// ---------------------------------------------------------------------------
// The bthid device table, with the same generation rules production uses.
// ---------------------------------------------------------------------------

// Real driver objects are not linked; only their ADDRESSES matter to the
// runtime, which compares device->driver against them.
const bthid_driver_t bthid_keyboard_driver;
const bthid_driver_t bthid_mouse_driver;

static bthid_device_t s_devices[BTHID_MAX_DEVICES];
static uint32_t s_next_generation = 1u;

bthid_device_t *bthid_get_device(uint8_t conn_index) {
    for (unsigned i = 0; i < BTHID_MAX_DEVICES; ++i)
        if (s_devices[i].active && s_devices[i].conn_index == conn_index)
            return &s_devices[i];
    return NULL;
}

bthid_device_t *bthid_get_device_slot(uint8_t slot) {
    return slot < BTHID_MAX_DEVICES && s_devices[slot].active ? &s_devices[slot]
                                                              : NULL;
}

// Mirrors add_device() in bthid.c: a fresh slot gets the next generation, so a
// reused connection index gets a DIFFERENT one.
static bthid_device_t *device_connect(uint8_t conn_index, bool is_ble,
                                      const bthid_driver_t *driver) {
    for (unsigned i = 0; i < BTHID_MAX_DEVICES; ++i) {
        if (s_devices[i].active) continue;
        memset(&s_devices[i], 0, sizeof(s_devices[i]));
        s_devices[i].active = true;
        s_devices[i].conn_index = conn_index;
        s_devices[i].is_ble = is_ble;
        s_devices[i].driver = driver;
        s_devices[i].connection_generation = s_next_generation++;
        s_devices[i].bd_addr[0] = 0xAAu;
        s_devices[i].bd_addr[5] = conn_index;
        snprintf(s_devices[i].name, sizeof(s_devices[i].name), "peer%u",
                 conn_index);
        return &s_devices[i];
    }
    return NULL;
}

static void device_remove(uint8_t conn_index) {
    bthid_device_t *device = bthid_get_device(conn_index);
    if (device) memset(device, 0, sizeof(*device));
}

// ---------------------------------------------------------------------------
// Source arbiter: accept whatever the KB/M composite submits.
//
// Ownership refusal is a SEPARATE, already-counted branch (rejectedNotOwner).
// Keeping the stub permissive means a failure in these tests is an admission
// failure and nothing else.
// ---------------------------------------------------------------------------

static unsigned s_submits;
static bool s_submit_result = true;
static uint32_t s_last_group;

bool ns2_active_input_submit_group(const input_event_t *event, uint32_t group_id,
                                   ns2_input_route_decision_t *decision) {
    (void)event;
    s_submits++;
    s_last_group = group_id;
    if (decision) memset(decision, 0, sizeof(*decision));
    return s_submit_result;
}

uint32_t ns2_active_input_source_id_for(uint8_t conn_index,
                                        uint32_t connection_generation) {
    (void)connection_generation;
    return 0x100u + conn_index;
}

void ns2_active_input_reset(void) {}

// Production reaches note_ready through this; the runtime also calls it back
// when a mode change re-evaluates live peers, so keep the real shape.
void ns2_active_input_note_connection(uint8_t conn_index) {
    ns2_kbm_runtime_note_ready(conn_index);
}

// ---------------------------------------------------------------------------
// Report seam and clock.
// ---------------------------------------------------------------------------

static unsigned s_gamepad_publishes;
static switch_pro_input_t s_last_published;

void set_global_gamepad_input(uint8_t slot, const switch_pro_input_t *in) {
    (void)slot;
    if (in) s_last_published = *in;
    s_gamepad_publishes++;
}

void accumulate_global_mouse_input(uint8_t slot, const switch_pro_input_t *in) {
    set_global_gamepad_input(slot, in);
}

void set_global_device(uint8_t slot, const char *name, uint16_t vid,
                       uint16_t pid) {
    (void)slot;
    (void)name;
    (void)vid;
    (void)pid;
}

void set_global_raw_buttons(uint8_t slot, uint32_t buttons) {
    (void)slot;
    (void)buttons;
}

void report_neutralize_slot(uint8_t slot) { (void)slot; }
void ns2_native_motion_clear(void) {}

static uint32_t s_now_ms = 1000u;
uint32_t platform_time_ms(void) { return s_now_ms; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#define USAGE_B 0x05u  // HID Keyboard b, the binding exercised on hardware

static void reset_world(void) {
    memset(s_devices, 0, sizeof(s_devices));
    s_next_generation = 1u;
    s_submits = 0;
    s_submit_result = true;
    s_last_group = 0;
    s_gamepad_publishes = 0;
    memset(&s_last_published, 0, sizeof(s_last_published));
    ns2_kbm_runtime_init();
}

static ns2_kbm_runtime_status_t status(void) {
    ns2_kbm_runtime_status_t out;
    ns2_kbm_runtime_status(&out);
    return out;
}

// One decoded keyboard report, stamped exactly as bthid_keyboard.c stamps it.
static bool press(const bthid_device_t *device, unsigned usage) {
    input_event_t event;
    memset(&event, 0, sizeof(event));
    event.dev_addr = device->conn_index;
    event.connection_generation = device->connection_generation;
    event.type = INPUT_TYPE_GAMEPAD;

    uint8_t bitmap[NS2_KBM_KEY_BITMAP_BYTES];
    memset(bitmap, 0, sizeof(bitmap));
    if (usage) bitmap[usage >> 3] |= (uint8_t)(1u << (usage & 7u));
    return ns2_kbm_runtime_submit_keyboard(&event, bitmap);
}

// The BLE keyboard lifecycle as bthid.c performs it: the peer arrives on the
// generic gamepad driver, its descriptor reclassifies it onto the keyboard
// driver, the driver reports the classification, and the rebind hook then
// re-notes the connection.
static bthid_device_t *connect_ble_keyboard(uint8_t conn_index,
                                            bool has_pointer) {
    bthid_device_t *device = device_connect(conn_index, true, NULL);
    assert(device);
    device->driver = &bthid_keyboard_driver;
    ns2_kbm_runtime_note_classification(device->conn_index,
                                        device->connection_generation, true,
                                        has_pointer,
                                        /*declares_combo=*/false,
                                        /*strong_keyboard=*/true);
    ns2_active_input_note_connection(conn_index);  // bthid_on_hid_rebind()
    return device;
}

static bthid_device_t *connect_classic_mouse(uint8_t conn_index) {
    bthid_device_t *device = device_connect(conn_index, false, NULL);
    assert(device);
    device->driver = &bthid_mouse_driver;
    ns2_active_input_note_connection(conn_index);
    return device;
}


// ---------------------------------------------------------------------------
// Lifecycle orderings
// ---------------------------------------------------------------------------

// Every connection ordering the BLE and Classic lifecycles can produce, asserted
// rather than eyeballed. The hypothesis these were written to test -- that a
// live peer could hold the keyboard role while the report path still saw its
// classification as pending -- is NOT reachable: the role can only be granted
// from a classification, and both live and die with the same
// (conn_index, connection_generation) key.
static void test_lifecycle_orderings(void) {
    ns2_kbm_runtime_status_t before, after;
    bthid_device_t *kb;

    // AUTO override, not an explicitly selected Keyboard mode. This is the
    // shipped default and the configuration the hardware failure was seen in.
    reset_world();
    kb = connect_ble_keyboard(5, /*has_pointer=*/true);
    assert(press(kb, USAGE_B));
    assert(status().keyboard_reports == 1u);

    // hid_ready fires before the descriptor, so the peer is first seen on the
    // generic gamepad driver and only later reclassified.
    reset_world();
    kb = device_connect(5, true, NULL);
    ns2_active_input_note_connection(5);
    kb->driver = &bthid_keyboard_driver;
    ns2_kbm_runtime_note_classification(5, kb->connection_generation, true, true,
                                        false, true);
    ns2_active_input_note_connection(5);
    assert(press(kb, USAGE_B));

    // Reconnect on the same connection index with a new generation: the new
    // peer is admitted, and inherits nothing from the old one.
    reset_world();
    kb = connect_ble_keyboard(5, true);
    uint32_t first_generation = kb->connection_generation;
    assert(press(kb, USAGE_B));
    ns2_kbm_runtime_disconnect(5, 0, first_generation);
    device_remove(5);
    kb = connect_ble_keyboard(5, true);
    assert(kb->connection_generation != first_generation);
    assert(press(kb, USAGE_B));
    assert(status().keyboard_conn == 5);

    // A stale-generation disconnect arriving AFTER the peer came back must not
    // release the live role.
    reset_world();
    kb = connect_ble_keyboard(5, true);
    uint32_t stale = kb->connection_generation;
    device_remove(5);
    kb = connect_ble_keyboard(5, true);
    ns2_kbm_runtime_disconnect(5, 0, stale);
    assert(press(kb, USAGE_B));
    assert(status().keyboard_connected);

    // A reconnect whose disconnect never reached the runtime leaves the role
    // held by the OLD generation. That must be refused as a duplicate -- loudly,
    // and never by silently letting the new peer inherit the slot.
    reset_world();
    kb = connect_ble_keyboard(5, true);
    assert(press(kb, USAGE_B));
    device_remove(5);
    kb = connect_ble_keyboard(5, true);
    before = status();
    assert(!press(kb, USAGE_B));
    after = status();
    assert(after.rejected_duplicate == before.rejected_duplicate + 1u);
    assert(after.rejected_unclassified == before.rejected_unclassified);

    // A mode change re-evaluates live peers, and the keyboard keeps working
    // across it without needing to send anything first.
    reset_world();
    kb = connect_ble_keyboard(5, true);
    assert(press(kb, USAGE_B));
    ns2_kbm_runtime_set_mode(NS2_KBM_MODE_KEYBOARD);
    assert(press(kb, USAGE_B));

    // The ordinary composition: two separate peers, one group.
    reset_world();
    kb = connect_ble_keyboard(5, /*has_pointer=*/false);
    (void)connect_classic_mouse(6);
    after = status();
    assert(after.keyboard_connected && after.mouse_connected);
    assert(after.keyboard_conn == 5 && after.mouse_conn == 6);
    assert(after.group_id != 0u);
    assert(press(kb, USAGE_B));

    // A keyboard that also declares a pointer keeps the keyboard role and does
    // NOT take the mouse slot, so a real mouse can still join it.
    reset_world();
    kb = connect_ble_keyboard(5, /*has_pointer=*/true);
    assert(status().keyboard_connected && !status().mouse_connected);
    (void)connect_classic_mouse(6);
    after = status();
    assert(after.keyboard_conn == 5 && after.mouse_conn == 6);

    printf("  lifecycle orderings: role and classification never disagree\n");
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_ble_keyboard_first_report_is_accepted(void) {
    reset_world();
    ns2_kbm_runtime_set_mode(NS2_KBM_MODE_KEYBOARD);

    bthid_device_t *kb = connect_ble_keyboard(5, /*has_pointer=*/true);

    ns2_kbm_runtime_status_t before = status();
    printf("    after connect: keyboard=%u mouse=%u mode=%u profile=%u\n",
           before.keyboard_connected, before.mouse_connected, before.mode,
           before.profile);
    assert(before.keyboard_connected);
    assert(!before.mouse_connected);

    bool accepted = press(kb, USAGE_B);
    ns2_kbm_runtime_status_t after = status();
    printf("    after press:   accepted=%d kbReports=%lu rejMode=%lu "
           "rejDup=%lu rejOwner=%lu publishes=%lu\n",
           accepted ? 1 : 0, (unsigned long)after.keyboard_reports,
           (unsigned long)after.rejected_mode,
           (unsigned long)after.rejected_duplicate,
           (unsigned long)after.rejected_not_owner,
           (unsigned long)after.publishes);

    assert(accepted);
    assert(after.keyboard_reports == 1u);
    assert(after.rejected_mode == before.rejected_mode);
    assert(after.rejected_duplicate == before.rejected_duplicate);
    assert(after.rejected_not_owner == before.rejected_not_owner);
    assert(after.publishes > before.publishes);
    printf("  BLE keyboard: first report accepted and published\n");
}


// The whole point of the feature: a bound key must reach the console as a
// pressed button. Everything above only proves the report was accepted.
static void test_bound_key_reaches_the_published_report(void) {
    reset_world();

    ns2_kbm_source_t key_b = {NS2_KBM_SRC_KEY, USAGE_B};
    assert(ns2_kbm_runtime_set_binding(NS2_KBM_LAYOUT_KEYBOARD, key_b,
                                       NS2_DST_B));

    bthid_device_t *kb = connect_ble_keyboard(5, true);

    unsigned publishes_before = s_gamepad_publishes;
    assert(press(kb, USAGE_B));
    assert(s_gamepad_publishes > publishes_before);

    bool any_button = s_last_published.buttons[0] || s_last_published.buttons[1] ||
                      s_last_published.buttons[2];
    printf("    published buttons: %02X %02X %02X\n",
           s_last_published.buttons[0], s_last_published.buttons[1],
           s_last_published.buttons[2]);
    assert(any_button);

    // Release clears it again.
    assert(press(kb, 0));
    assert(!s_last_published.buttons[0] && !s_last_published.buttons[1] &&
           !s_last_published.buttons[2]);
    printf("  bound key reaches the published report, and releases\n");
}


// Each silent-exit counter must move only for its own branch, and an accepted
// report must move none of them. Without this the counters would be worse than
// nothing: a wrong one sends the next investigation to the wrong subsystem.
static void test_silent_exit_counters_are_specific(void) {
    ns2_kbm_runtime_status_t before, after;

    // Branch 2: a BLE keyboard-driver peer whose descriptor has not been parsed
    // yet. Nothing else may move.
    reset_world();
    bthid_device_t *kb = device_connect(5, true, NULL);
    kb->driver = &bthid_keyboard_driver;
    before = status();
    assert(!press(kb, USAGE_B));
    after = status();
    assert(after.rejected_unclassified == before.rejected_unclassified + 1u);
    assert(after.rejected_no_peer_key == before.rejected_no_peer_key);
    assert(after.rejected_no_role == before.rejected_no_role);
    assert(after.rejected_mode == before.rejected_mode);
    assert(after.rejected_duplicate == before.rejected_duplicate);
    assert(after.keyboard_reports == before.keyboard_reports);

    // ...and the same peer is accepted the moment its descriptor lands, with no
    // counter moving at all.
    ns2_kbm_runtime_note_classification(5, kb->connection_generation, true, true,
                                        false, true);
    before = status();
    assert(press(kb, USAGE_B));
    after = status();
    assert(after.keyboard_reports == before.keyboard_reports + 1u);
    assert(after.rejected_unclassified == before.rejected_unclassified);
    assert(after.rejected_no_peer_key == before.rejected_no_peer_key);
    assert(after.rejected_no_role == before.rejected_no_role);
    assert(after.rejected_not_owner == before.rejected_not_owner);
    assert(after.rejected_mode == before.rejected_mode);
    assert(after.rejected_duplicate == before.rejected_duplicate);

    // Branch 1: no device behind the connection index the report names.
    reset_world();
    kb = connect_ble_keyboard(5, true);
    input_event_t orphan;
    memset(&orphan, 0, sizeof(orphan));
    orphan.dev_addr = 9;  // never connected
    orphan.connection_generation = 77u;
    uint8_t bitmap[NS2_KBM_KEY_BITMAP_BYTES];
    memset(bitmap, 0, sizeof(bitmap));
    bitmap[USAGE_B >> 3] |= (uint8_t)(1u << (USAGE_B & 7u));
    before = status();
    assert(!ns2_kbm_runtime_submit_keyboard(&orphan, bitmap));
    after = status();
    assert(after.rejected_no_peer_key == before.rejected_no_peer_key + 1u);
    assert(after.rejected_unclassified == before.rejected_unclassified);
    assert(after.rejected_no_role == before.rejected_no_role);

    // Branch 4: a peer holding the MOUSE role sends a keyboard report. This is
    // the KERIS II shape and must stay rejected -- a macro key is not a licence
    // to take the keyboard role -- but it must no longer be silent.
    reset_world();
    bthid_device_t *mouse = device_connect(6, true, NULL);
    mouse->driver = &bthid_mouse_driver;
    ns2_kbm_runtime_note_classification(6, mouse->connection_generation,
                                        /*has_keyboard=*/true,
                                        /*has_pointer=*/true,
                                        /*declares_combo=*/false,
                                        /*strong_keyboard=*/false);
    ns2_active_input_note_connection(6);
    before = status();
    assert(!press(mouse, USAGE_B));
    after = status();
    assert(after.rejected_no_role == before.rejected_no_role + 1u);
    assert(after.rejected_unclassified == before.rejected_unclassified);
    assert(after.rejected_no_peer_key == before.rejected_no_peer_key);
    assert(after.keyboard_reports == before.keyboard_reports);
    printf("  silent-exit counters are branch-specific\n");
}

// A macro-key mouse must never take the keyboard role, and must not stop a real
// keyboard from taking it. This is the KERIS II regression, at the runtime layer.
static void test_macro_mouse_never_takes_the_keyboard_role(void) {
    reset_world();

    bthid_device_t *mouse = device_connect(6, true, NULL);
    mouse->driver = &bthid_mouse_driver;
    ns2_kbm_runtime_note_classification(6, mouse->connection_generation, true,
                                        true, false, /*strong_keyboard=*/false);
    ns2_active_input_note_connection(6);

    ns2_kbm_runtime_status_t after_mouse = status();
    assert(after_mouse.mouse_connected);
    assert(!after_mouse.keyboard_connected);

    bthid_device_t *kb = connect_ble_keyboard(5, false);
    ns2_kbm_runtime_status_t both = status();
    assert(both.keyboard_connected && both.mouse_connected);
    assert(both.keyboard_conn == 5 && both.mouse_conn == 6);
    assert(both.group_id != 0u);

    assert(press(kb, USAGE_B));
    printf("  macro-capable mouse keeps the mouse role; keyboard still joins\n");
}

int main(void) {
    test_ble_keyboard_first_report_is_accepted();
    test_lifecycle_orderings();
    test_bound_key_reaches_the_published_report();
    test_silent_exit_counters_are_specific();
    test_macro_mouse_never_takes_the_keyboard_role();
    printf("ns2_kbm_runtime lifecycle: OK\n");
    return 0;
}
