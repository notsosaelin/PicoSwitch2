// Firmware adapter for the Bluetooth Keyboard / Keyboard + Mouse input model.
//
// Everything portable lives in ns2_kbm.c. This file is the part that needs the
// firmware: role admission above the source arbiter, the cross-core
// configuration copy, publishing into report.c, and diagnostics.
//
// Only compiled into the BT_STACK_JOYPAD firmware; the model it drives is host
// testable on its own.

#include "ns2_kbm_runtime.h"

#include <string.h>

#include "bt/bthid/bthid.h"
#include "bt/bthid/devices/generic/bthid_keyboard.h"
#include "bt/bthid/devices/generic/bthid_mouse.h"
#include "ns2_active_input.h"
#include "ns2_native_motion.h"
#include "report.h"
#include "switch_pro.h"
#ifdef NS2_PRO
#include "usb.h"  // g_usb_personality: which output personality is live
#endif
#include "platform/platform.h"  // platform_time_ms()

// ---------------------------------------------------------------------------
// Cross-core configuration
// ---------------------------------------------------------------------------
// Core 0 mutates `s_config` from the management/UART parser; core 1 consumes a
// private snapshot. The seqlock is the same shape the input arbiter uses: an
// odd sequence means a write is in flight.
static ns2_kbm_config_t s_config;              // core 0 writer
static volatile uint32_t s_config_sequence;    // seqlock
static volatile uint32_t s_config_generation;  // bumped by every mutation
static volatile uint8_t s_mode = (uint8_t)NS2_KBM_MODE_CONTROLLER;

static ns2_kbm_config_t s_active;              // core 1 private snapshot
static uint32_t s_active_generation;
static bool s_active_valid;

static ns2_kbm_roles_t s_roles;
static ns2_kbm_state_t s_state;

// What each live peer can do, and which role it represents. Bounded by the
// bthid device table.
//
// Both are needed. Capability is what the descriptor and reports prove the
// connection can emit; primary is which logical role the physical device stands
// for. A gaming mouse with macro buttons has both capabilities and is still
// only a mouse.
#define KBM_PEER_SLOTS BTHID_MAX_DEVICES
static struct {
    uint8_t valid;
    uint8_t conn_index;
    uint32_t connection_generation;
    uint8_t has_keyboard;
    uint8_t has_pointer;
    uint8_t primary;  // ns2_kbm_primary_t; 0 until classified
} s_peers[KBM_PEER_SLOTS];

// Mode changes arrive on core 0 but must be applied where the source registry
// is written. Core 0 bumps the request; core 1 applies it at its next boundary.
static volatile uint32_t s_mode_change_request;
static uint32_t s_mode_change_applied;

static uint32_t s_rejected_not_owner;
// The admission outcomes that used to be silent.
//
// Three of admit_and_route()'s exits incremented nothing, so a keyboard that
// produced no console input left `kbm status` showing keyboardReports == 0 with
// no way to tell which stage dropped the report -- or whether one ever arrived.
// s_undecoded_reports sits further upstream still, in the keyboard driver: it
// counts reports that reached the driver and decoded as neither a keyboard nor
// a pointer report, which is what a mis-parsed descriptor looks like from here.
static uint32_t s_rejected_no_peer_key;
static uint32_t s_rejected_unclassified;
static uint32_t s_rejected_no_role;
static uint32_t s_undecoded_reports;
static uint32_t s_rollover_reports;
static uint32_t s_remap_neutralizations;

// The RAW previous keyboard bitmap, for switch-key edge detection.
//
// Separate from the state's key set, which holds the MASKED bitmap: a switch key
// is removed from gameplay, so the state can never tell whether it is held, and
// an edge computed from it would re-fire on every report while the key is down.
static uint8_t s_prev_keys[NS2_KBM_KEY_BITMAP_BYTES];

// Which resident slot each layout is realizing RIGHT NOW.
//
// Distinct from the persisted boot choice: a switch key changes this and nothing
// else. A companion must be able to report what the adapter is actually running
// rather than assuming it is still the profile that was last persisted.
static uint8_t s_runtime_position[NS2_KBM_LAYOUT_COUNT];
static uint32_t s_runtime_switches;
static uint32_t s_publishes;
static uint32_t s_stick_recenters;
static uint32_t s_source_id;

static uint8_t s_battery_level;
static uint8_t s_battery_valid;
static uint8_t s_battery_charging;
static char s_identity_name[BTHID_MAX_NAME_LEN];
static uint16_t s_identity_vid;
static uint16_t s_identity_pid;

// Core-1 half of a deferred mode change; defined below, used by the lifecycle
// hooks above it.
static void apply_pending_mode_change(void);

static uint32_t atomic_load_u32(const volatile uint32_t *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void config_write_begin(void) {
    __atomic_add_fetch(&s_config_sequence, 1u, __ATOMIC_ACQ_REL);
}

static void config_write_end(void) {
    __atomic_add_fetch(&s_config_generation, 1u, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&s_config_sequence, 1u, __ATOMIC_ACQ_REL);
}

static void config_snapshot(ns2_kbm_config_t *out, uint32_t *generation) {
    for (;;) {
        uint32_t before = atomic_load_u32(&s_config_sequence);
        if (before & 1u) continue;
        *out = s_config;
        uint32_t gen = atomic_load_u32(&s_config_generation);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (before == atomic_load_u32(&s_config_sequence)) {
            *generation = gen;
            return;
        }
    }
}

void ns2_kbm_runtime_init(void) {
    ns2_kbm_config_defaults(&s_config);
    s_config_sequence = 0u;
    s_config_generation = 1u;
    s_mode = s_config.mode;
    ns2_kbm_roles_init(&s_roles);
    ns2_kbm_state_init(&s_state);
    memset(s_peers, 0, sizeof(s_peers));
    s_active_valid = false;
    s_active_generation = 0u;
    s_rejected_not_owner = 0u;
    s_rejected_no_peer_key = 0u;
    s_rejected_unclassified = 0u;
    s_rejected_no_role = 0u;
    s_undecoded_reports = 0u;
    s_rollover_reports = 0u;
    s_remap_neutralizations = 0u;
    memset(s_prev_keys, 0, sizeof(s_prev_keys));
    s_runtime_switches = 0u;
    // Power-up runs the PERSISTED boot choice for each layout. config_persist
    // has already realized it into the record this snapshots from.
    for (uint8_t i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i)
        s_runtime_position[i] = s_config.boot_position[i];
    s_publishes = 0u;
    s_stick_recenters = 0u;
    s_source_id = 0u;
    s_battery_level = 0u;
    s_battery_valid = 0u;
    s_battery_charging = 0u;
    s_identity_name[0] = '\0';
    s_identity_vid = 0u;
    s_identity_pid = 0u;
}

// The PERSISTED setting, which is an override rather than the live mode.
static ns2_kbm_mode_t mode_override(void) {
    uint8_t mode = __atomic_load_n(&s_mode, __ATOMIC_ACQUIRE);
    return mode < (uint8_t)NS2_KBM_MODE_COUNT ? (ns2_kbm_mode_t)mode
                                              : NS2_KBM_MODE_AUTO;
}

// ---------------------------------------------------------------------------
// Output personality capability
// ---------------------------------------------------------------------------
// Native mouse behavior is a property of the selected Switch-facing
// personality, not of the mouse. Joy-Con 2 is the only personality with a real
// pointer today; every other one needs the stick translation.
static bool output_supports_native_mouse(void) {
#ifdef NS2_PRO
    return g_usb_personality == USB_PERSONALITY_JOYCON2_L ||
           g_usb_personality == USB_PERSONALITY_JOYCON2_R;
#else
    return false;
#endif
}

// Which roles a peer is allowed to take right now.
//
// This is where the native Joy-Con mouse exception lives. On Joy-Con 2 L/R a
// mouse ALONE must keep using the established native pointer path rather than
// bootstrapping a KB/M composite, so the mouse role is simply not offered while
// no keyboard role is held. Once a keyboard is present the composite forms
// normally and the mouse joins it -- and it still reaches the native pointer,
// because ns2_kbm_resolve() passes relative deltas straight through whenever
// the personality has one.
static ns2_kbm_role_policy_t role_policy(void) {
    ns2_kbm_role_policy_t policy = {1u, 1u};
    switch (mode_override()) {
        case NS2_KBM_MODE_CONTROLLER:
            // Explicit "Controller": KB/M is off entirely.
            policy.allow_keyboard = 0u;
            policy.allow_mouse = 0u;
            return policy;
        case NS2_KBM_MODE_KEYBOARD:
            // Explicit "Keyboard": a mouse never joins.
            policy.allow_mouse = 0u;
            return policy;
        default:
            break;  // AUTO and explicit Keyboard + Mouse both allow either role
    }
    if (mode_override() == NS2_KBM_MODE_AUTO && output_supports_native_mouse() &&
        !s_roles.keyboard.valid)
        policy.allow_mouse = 0u;
    return policy;
}

ns2_kbm_mode_t ns2_kbm_runtime_mode(void) {
    return ns2_kbm_effective_mode(mode_override(), s_roles.keyboard.valid != 0u,
                                  s_roles.mouse.valid != 0u);
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

// The profile-owned mouse settings the live layout is realizing.
//
// Mouse tuning moved into the profile with schema 14, so there is no longer one
// global block: the translator reads whatever the realized mapping carries, and
// switching profile changes the feel along with the bindings.
static const ns2_kbm_mouse_config_t *active_mouse(void) {
    ns2_kbm_layout_t layout = ns2_kbm_mode_layout(ns2_kbm_runtime_mode());
    if (layout >= NS2_KBM_LAYOUT_COUNT) layout = NS2_KBM_LAYOUT_KEYBOARD;
    return &s_active.active[layout].content.mouse;
}

static void publish_locked(void) {
    ns2_kbm_output_t out;
    bool native = output_supports_native_mouse();
    ns2_kbm_resolve(&s_state, &s_active, ns2_kbm_runtime_mode(), native, &out);

    switch_pro_input_t in;
    memset(&in, 0, sizeof(in));
    in.buttons[0] = out.buttons[0];
    in.buttons[1] = out.buttons[1];
    in.buttons[2] = out.buttons[2];
    in.extra = out.extra;
    switch_pro_pack_stick(out.left_x, out.left_y, in.left_stick);
    switch_pro_pack_stick(out.right_x, out.right_y, in.right_stick);
    in.battery_level = s_battery_level;
    in.battery_valid = s_battery_valid;
    in.battery_charging = s_battery_charging;
    in.has_mouse = out.has_mouse;
    in.mouse_delta_x = out.mouse_delta_x;
    in.mouse_delta_y = out.mouse_delta_y;
    in.mouse_delta_wheel = out.mouse_delta_wheel;

    // Publish identity before input so core 0 sees the owning peer named on the
    // same report that first carries its state, matching the ordinary seam.
    if (s_identity_name[0])
        set_global_device(0, s_identity_name, s_identity_vid, s_identity_pid);

    if (native) {
        // Relative movement must survive between USB frames, so go through the
        // accumulating publish even for a keyboard-triggered republish (whose
        // deltas are zero and therefore add nothing).
        accumulate_global_mouse_input(0, &in);
    } else {
        set_global_gamepad_input(0, &in);
    }
    // set_global_raw_buttons() carries the source's JP_BUTTON_* bitmap, which a
    // keyboard genuinely does not have. Publishing the resolved Switch-facing
    // bytes there would label them as something they are not, so publish
    // nothing: `input status` already shows the mapped result and `kbm status`
    // shows the KB/M side.
    set_global_raw_buttons(0, 0u);
    s_publishes++;
}

// Adopt a newer configuration at an explicit neutral boundary.
//
// Recomputing the output from the held-source set would already release a
// remapped destination on the next report, but a source that is held and quiet
// sends no next report. Neutralizing and dropping the held state makes the
// boundary unconditional, which is what lets a future remapping UI change a
// binding without implementing its own input cleanup.
static void sync_config(void) {
    uint32_t generation = atomic_load_u32(&s_config_generation);
    if (s_active_valid && generation == s_active_generation) return;
    config_snapshot(&s_active, &s_active_generation);
    s_active_valid = true;
    ns2_kbm_state_clear_keyboard(&s_state);
    ns2_kbm_state_clear_mouse(&s_state);
    report_neutralize_slot(0);
    ns2_native_motion_clear();
    s_remap_neutralizations++;
}

// ---------------------------------------------------------------------------
// Peer identity
// ---------------------------------------------------------------------------

static bool peer_key_for_connection(uint8_t conn_index,
                                    uint32_t connection_generation,
                                    ns2_kbm_peer_key_t *key) {
    memset(key, 0, sizeof(*key));
    const bthid_device_t *device = bthid_get_device(conn_index);
    if (!device) return false;
    key->conn_index = conn_index;
    key->connection_generation = connection_generation != 0u
                                     ? connection_generation
                                     : device->connection_generation;
    memcpy(key->addr, device->bd_addr, sizeof(key->addr));
    for (unsigned i = 0; i < sizeof(key->addr); ++i) {
        if (key->addr[i] != 0u) {
            key->addr_valid = 1u;
            break;
        }
    }
    key->valid = 1u;
    return true;
}

static int peer_slot(uint8_t conn_index, uint32_t connection_generation) {
    for (unsigned i = 0; i < KBM_PEER_SLOTS; ++i) {
        if (s_peers[i].valid && s_peers[i].conn_index == conn_index &&
            s_peers[i].connection_generation == connection_generation)
            return (int)i;
    }
    return -1;
}

static ns2_kbm_peer_caps_t caps_lookup(uint8_t conn_index,
                                       uint32_t connection_generation) {
    ns2_kbm_peer_caps_t caps = {0u, 0u};
    int slot = peer_slot(conn_index, connection_generation);
    if (slot >= 0) {
        caps.keyboard = s_peers[slot].has_keyboard;
        caps.pointer = s_peers[slot].has_pointer;
    }
    return caps;
}

static ns2_kbm_primary_t primary_lookup(uint8_t conn_index,
                                        uint32_t connection_generation) {
    int slot = peer_slot(conn_index, connection_generation);
    return slot >= 0 ? (ns2_kbm_primary_t)s_peers[slot].primary
                     : NS2_KBM_PRIMARY_NONE;
}

static void caps_record(uint8_t conn_index, uint32_t connection_generation,
                        bool has_keyboard, bool has_pointer) {
    if (connection_generation == 0u) return;

    int slot = peer_slot(conn_index, connection_generation);
    if (slot < 0) {
        // Take a free slot, or reclaim one belonging to a DEAD generation of
        // this same connection index. Transport indexes are reused, so an old
        // occupant's record must not outlive it and consume the table.
        for (unsigned i = 0; i < KBM_PEER_SLOTS && slot < 0; ++i) {
            if (!s_peers[i].valid) slot = (int)i;
        }
        for (unsigned i = 0; i < KBM_PEER_SLOTS && slot < 0; ++i) {
            if (s_peers[i].conn_index == conn_index) slot = (int)i;
        }
        if (slot < 0) return;
        memset(&s_peers[slot], 0, sizeof(s_peers[slot]));
        s_peers[slot].valid = 1u;
        s_peers[slot].conn_index = conn_index;
        s_peers[slot].connection_generation = connection_generation;
    }
    // Capabilities accumulate across one live generation: the keyboard driver
    // learns about a pointer collection only when the descriptor arrives, which
    // is after the driver bound. They are never narrowed.
    if (has_keyboard) s_peers[slot].has_keyboard = 1u;
    if (has_pointer) s_peers[slot].has_pointer = 1u;
}

// Record the role a peer REPRESENTS. Unlike capabilities this does not
// accumulate: the first classification with real evidence stands for the life of
// the connection, so a later capability discovery cannot promote a mouse into
// something that claims the keyboard role.
static void primary_record(uint8_t conn_index, uint32_t connection_generation,
                           ns2_kbm_primary_t primary) {
    if (primary == NS2_KBM_PRIMARY_NONE) return;
    int slot = peer_slot(conn_index, connection_generation);
    if (slot < 0) return;
    if (s_peers[slot].primary == (uint8_t)NS2_KBM_PRIMARY_NONE)
        s_peers[slot].primary = (uint8_t)primary;
}

void ns2_kbm_runtime_note_classification(uint8_t conn_index,
                                         uint32_t connection_generation,
                                         bool has_keyboard, bool has_pointer,
                                         bool declares_combo, bool strong_keyboard) {
    caps_record(conn_index, connection_generation, has_keyboard, has_pointer);
    ns2_kbm_peer_caps_t caps = {has_keyboard ? 1u : 0u, has_pointer ? 1u : 0u};

    // The rule itself lives in ns2_kbm.c so it can be host-tested: this file
    // cannot be compiled without the Bluetooth stack, and the KERIS II
    // regression boundary is far too important to sit somewhere no test reaches.
    primary_record(conn_index, connection_generation,
                   ns2_kbm_primary_from_evidence(caps, declares_combo, strong_keyboard));
}

// Forget exactly one connection generation.
//
// Deliberately NOT a wildcard on connection index. Transport indexes are
// reused, so a disconnect that does not name a generation would erase the
// classification of whatever LIVE peer now occupies that index -- and a peer
// whose classification is gone gets re-derived from whatever partial evidence
// arrives next. That is how the KERIS II mouse, sitting on an index the
// departed keyboard had used, came back as a "keyboard".
//
// A generation of 0 means "unspecified", which is not enough to identify an
// occupant; the stale record is instead reclaimed by caps_record() when a new
// generation needs the slot.
static void peer_forget(uint8_t conn_index, uint32_t connection_generation) {
    if (connection_generation == 0u) return;
    for (unsigned i = 0; i < KBM_PEER_SLOTS; ++i) {
        if (s_peers[i].valid && s_peers[i].conn_index == conn_index &&
            s_peers[i].connection_generation == connection_generation)
            memset(&s_peers[i], 0, sizeof(s_peers[i]));
    }
}

// Capability from the bound driver.
//
// Driver ownership is NOT the logical role. The keyboard driver is bound to
// ANYTHING whose descriptor carries keyboard usages, which includes a gaming
// mouse with macro buttons. Its narrow "keyboard, not pointer" view therefore
// must never establish primary for a BLE peer: doing so is how a mouse whose
// classification was lost re-derived itself as a keyboard.
//
// Classic is different and safe: bthid binds a Classic peer to the keyboard
// driver only when its Class of Device says keyboard or combo, which IS a
// statement by the device. BLE has no Class of Device, so a BLE keyboard-driver
// peer must wait for ns2_kbm_runtime_note_classification(), which sees both
// halves of the descriptor.
// True when a more authoritative classification is still coming for this peer.
//
// A BLE peer reaches the keyboard driver ONLY through descriptor
// reclassification, and bthid_keyboard_set_descriptor() calls
// ns2_kbm_runtime_note_classification() immediately after with the full picture
// (both halves of the descriptor). Until that lands, the only capability on
// record is the driver binding's narrow "keyboard, not pointer" view -- which
// for a gaming mouse with macro keys is simply wrong, and latching primary from
// it would permanently mislabel the device.
//
// Classic peers have no later authority to wait for: their Class of Device
// already decided which driver they got, so capability precedence may latch.
static bool primary_authority_pending(const bthid_device_t *device) {
    return device && device->is_ble && device->driver == &bthid_keyboard_driver;
}

static void record_caps_from_driver(const bthid_device_t *device,
                                    const ns2_kbm_peer_key_t *key) {
    if (!device || !device->driver) return;
    if (device->driver == &bthid_keyboard_driver) {
        caps_record(key->conn_index, key->connection_generation, true, false);
        if (!device->is_ble) {
            primary_record(key->conn_index, key->connection_generation,
                           NS2_KBM_PRIMARY_KEYBOARD);
        }
    } else if (device->driver == &bthid_mouse_driver) {
        // The generic mouse driver claims Class-of-Device pointing devices and
        // descriptors with relative X/Y and no keyboard collection. Either way
        // this peer is a mouse.
        caps_record(key->conn_index, key->connection_generation, false, true);
        primary_record(key->conn_index, key->connection_generation,
                       NS2_KBM_PRIMARY_MOUSE);
    }
}

void ns2_kbm_runtime_note_ready(uint8_t conn_index) {
    apply_pending_mode_change();

    const bthid_device_t *device = bthid_get_device(conn_index);
    if (!device) return;
    ns2_kbm_peer_key_t key;
    if (!peer_key_for_connection(conn_index, device->connection_generation, &key))
        return;
    record_caps_from_driver(device, &key);
    ns2_kbm_peer_caps_t caps = caps_lookup(key.conn_index,
                                           key.connection_generation);
    if (!caps.keyboard && !caps.pointer) return;

    // Never guess primary here. This runs on every raw report, so a guess made
    // from a partial capability view would be retried until it happened to
    // succeed -- which is exactly how a mouse acquired the keyboard role once
    // that role fell free. A peer with no classification yet simply waits for
    // one.
    ns2_kbm_primary_t primary = primary_lookup(key.conn_index,
                                               key.connection_generation);
    if (primary == NS2_KBM_PRIMARY_NONE) return;

    // Bind the role now rather than on the first report. Otherwise "is the
    // selected source complete?" -- which gates the pairing window and the
    // idle-scan rule -- would stay false until the user pressed a key, leaving
    // discovery running against a source that is already fully connected.
    // Admission is idempotent, so repeating this costs a few comparisons.
    (void)ns2_kbm_roles_admit(&s_roles, role_policy(), primary, caps, &key);
}

bool ns2_kbm_runtime_gates_connection(uint8_t conn_index) {
    const bthid_device_t *device = bthid_get_device(conn_index);
    if (!device || !device->driver) return false;

    // A peer holding (or eligible for) a KB/M role is registered by the KB/M
    // path, with the composite group id attached. Registering it here as an
    // ordinary standalone source would give it console ownership outside the
    // composite.
    bool keyboard_capable = device->driver == &bthid_keyboard_driver;
    bool pointer_capable = device->driver == &bthid_mouse_driver;
    if (!keyboard_capable && !pointer_capable) {
        // A gamepad. It must not be registered while a KB/M composite owns the
        // console, or it would compete with the source the user is building.
        return s_roles.keyboard.valid || s_roles.mouse.valid;
    }

    ns2_kbm_role_policy_t policy = role_policy();
    if (keyboard_capable && policy.allow_keyboard) return true;
    if (pointer_capable && policy.allow_mouse) return true;
    // A mouse the policy will not take -- the native Joy-Con mouse case -- must
    // stay an ordinary source so the established pointer path still works.
    return false;
}

bool ns2_kbm_runtime_wants_peripheral(bool cod_keyboard, bool cod_pointing) {
    // Keyed on free roles and the current policy, never on a mode the user had
    // to select first: a keyboard or mouse must be discoverable on an ordinary
    // pairing gesture. Stops answering as soon as the role is filled.
    ns2_kbm_role_policy_t policy = role_policy();
    return (cod_keyboard && policy.allow_keyboard && !s_roles.keyboard.valid) ||
           (cod_pointing && policy.allow_mouse && !s_roles.mouse.valid);
}

// ---------------------------------------------------------------------------
// Report ingress
// ---------------------------------------------------------------------------

// Re-evaluate every live peer after an admission-policy change. Without this, a
// mode change would wait for each connected device to send another report,
// which a quiet controller may never do.
static void reevaluate_connected_peers(void) {
    for (uint8_t slot = 0; slot < BTHID_MAX_DEVICES; ++slot) {
        const bthid_device_t *device = bthid_get_device_slot(slot);
        if (!device) continue;
        ns2_active_input_note_connection(device->conn_index);
    }
}

// Core-1 half of a mode change (see ns2_kbm_runtime_set_mode). Idempotent and
// cheap when nothing is pending, so it is safe on the fast service tick.
//
// The request is marked applied BEFORE the work, deliberately: the peer
// re-evaluation below re-enters ns2_kbm_runtime_note_ready(), which calls this
// again. Claiming the request first bounds that to one level.
static void apply_pending_mode_change(void) {
    uint32_t request = atomic_load_u32(&s_mode_change_request);
    if (request == s_mode_change_applied) return;
    s_mode_change_applied = request;

    ns2_kbm_roles_release_all(&s_roles);
    ns2_kbm_state_init(&s_state);
    s_battery_valid = 0u;
    s_battery_level = 0u;
    s_battery_charging = 0u;
    s_source_id = 0u;
    s_identity_name[0] = '\0';
    s_identity_vid = 0u;
    s_identity_pid = 0u;
    s_active_valid = false;
    ns2_active_input_reset();
    reevaluate_connected_peers();
}

// Admit the peer to the composite source and confirm it currently owns the
// console. Returns false (having counted the reason) when either gate fails.
static bool admit_and_route(const input_event_t *event, bool from_keyboard) {
    apply_pending_mode_change();

    ns2_kbm_peer_key_t key;
    if (!peer_key_for_connection(event->dev_addr, event->connection_generation,
                                 &key)) {
        s_rejected_no_peer_key++;
        return false;
    }

    // A report proves CAPABILITY -- a peer that just sent a keyboard report can
    // emit keyboard reports -- but never the primary role. A gaming mouse
    // sending a macro keystroke must not be able to talk its way into the
    // keyboard role, so primary comes only from classification.
    caps_record(key.conn_index, key.connection_generation, from_keyboard,
                !from_keyboard);
    ns2_kbm_peer_caps_t caps = caps_lookup(key.conn_index,
                                           key.connection_generation);
    ns2_kbm_primary_t primary = primary_lookup(key.conn_index,
                                               key.connection_generation);
    if (primary == NS2_KBM_PRIMARY_NONE) {
        // Wait for a classification that is still coming rather than latching a
        // partial view. A BLE peer on the keyboard driver has caps of exactly
        // {keyboard} until its descriptor is parsed; latching that would turn a
        // gaming mouse into a keyboard for the life of the connection.
        if (primary_authority_pending(bthid_get_device(event->dev_addr))) {
            s_rejected_unclassified++;
            return false;
        }
        // Genuine last resort: a peer nothing will classify later -- a Classic
        // device whose descriptor never arrived. Decided from the ACCUMULATED
        // capabilities, not this one report, so a pointer-capable peer stays a
        // mouse however it happens to be reporting at the moment.
        primary = ns2_kbm_primary_from_caps(caps);
        primary_record(key.conn_index, key.connection_generation, primary);
    }

    ns2_kbm_admit_t admit =
        ns2_kbm_roles_admit(&s_roles, role_policy(), primary, caps, &key);
    if (admit == NS2_KBM_ADMIT_REJECT_MODE ||
        admit == NS2_KBM_ADMIT_REJECT_DUPLICATE)
        return false;

    bool keyboard_role = admit == NS2_KBM_ADMIT_KEYBOARD ||
                         admit == NS2_KBM_ADMIT_BOTH;
    bool mouse_role = admit == NS2_KBM_ADMIT_MOUSE || admit == NS2_KBM_ADMIT_BOTH;
    // Admitted, but not to the role this report needs. For a keyboard report
    // that means the peer holds the mouse slot and nothing else -- correct for a
    // pointer-capable peer classified MOUSE, and a defect for anything else.
    if (from_keyboard && !keyboard_role) {
        s_rejected_no_role++;
        return false;
    }
    if (!from_keyboard && !mouse_role) {
        s_rejected_no_role++;
        return false;
    }

    ns2_input_route_decision_t decision;
    if (!ns2_active_input_submit_group(event, s_roles.group_id, &decision)) {
        s_rejected_not_owner++;
        return false;
    }
    // Only resolve the opaque source handle when it is not already known: the
    // lookup snapshots the whole registry, which is not something to repeat on
    // every report of a 125 Hz stream.
    if (s_source_id == NS2_INPUT_SOURCE_ID_NONE) {
        s_source_id = ns2_active_input_source_id_for(event->dev_addr,
                                                     key.connection_generation);
    }
    // Label the console slot with the peer that owns the logical source so the
    // live view and the management identity panel name the KB/M peer rather
    // than whatever was connected before. Cached here and published from
    // publish_locked(), which runs after the neutral boundary that would
    // otherwise clear it.
    if (from_keyboard || !s_roles.keyboard.valid) {
        const bthid_device_t *device = bthid_get_device(event->dev_addr);
        if (device) {
            strncpy(s_identity_name, device->name, sizeof(s_identity_name) - 1u);
            s_identity_name[sizeof(s_identity_name) - 1u] = '\0';
            s_identity_vid = device->vendor_id;
            s_identity_pid = device->product_id;
        }
    }

    return true;
}

static void note_battery(const input_event_t *event) {
    if (event->battery_source == INPUT_BATTERY_NONE) return;
    s_battery_level = event->battery_level;
    s_battery_valid = 1u;
    s_battery_charging = event->battery_charging ? 1u : 0u;
}

bool ns2_kbm_runtime_submit_keyboard(const input_event_t *event,
                                     const uint8_t *usage_bitmap) {
    if (!event || !usage_bitmap) return false;
    if (!admit_and_route(event, true)) return false;

    sync_config();
    note_battery(event);

    // PROFILE-SWITCH KEYS ARE RESOLVED BEFORE GAMEPLAY BINDING.
    //
    // A switch key selects a resident slot while the adapter runs standalone,
    // which is the reason the adapter stores profiles at all. It is handled
    // here, ahead of the mapping, and consumed: the raw bitmap drives the edge,
    // and a masked copy drives gameplay, so pressing the key changes profile
    // WITHOUT also firing whatever the outgoing profile mapped that usage to.
    //
    // Entirely RAM. No config save, no flash erase -- switching profiles mid-game
    // must not wear the sector. The persisted boot choice is untouched.
    // THE SAME KEY MEANS THE SAME ACTION IN BOTH LAYOUTS. The binding names a
    // POSITION (Default, 1, 2, 3); the layout derived from what is actually
    // connected decides which bank that position is read from. So one set of
    // keys serves both, and switching from keyboard-only to keyboard+mouse
    // naturally changes what they select without any reconfiguration.
    ns2_kbm_layout_t layout = ns2_kbm_mode_layout(ns2_kbm_runtime_mode());
    uint8_t action = ns2_kbm_switch_edge(&s_active, s_prev_keys, usage_bitmap);

    uint8_t gameplay[NS2_KBM_KEY_BITMAP_BYTES];
    memcpy(gameplay, usage_bitmap, sizeof(gameplay));
    ns2_kbm_switch_mask(&s_active, gameplay);
    memcpy(s_prev_keys, usage_bitmap, sizeof(s_prev_keys));

    if (action != NS2_KBM_SWITCH_NONE) {
        bool changed = false;
        uint8_t target = ns2_kbm_position_profile_id(&s_active, layout, action);
        // An empty position in THIS layout's bank is rejected safely: the key
        // simply does nothing, rather than reaching into the other layout.
        if (target != NS2_KBM_PROFILE_ID_NONE &&
            ns2_kbm_apply(&s_active, layout, target, &changed)) {
            // Neutralize BEFORE the new mapping takes effect. A key held across
            // the switch would otherwise leave the destination it drove in the
            // OLD profile latched, with no report coming to release it.
            ns2_kbm_state_clear_keyboard(&s_state);
            ns2_kbm_state_clear_mouse(&s_state);
            report_neutralize_slot(0);
            ns2_native_motion_clear();
            s_remap_neutralizations++;
            s_runtime_position[layout] = action;
            s_runtime_switches++;
            // The releasing report re-establishes whatever is genuinely held.
            memset(gameplay, 0, sizeof(gameplay));
        }
    }

    ns2_kbm_state_set_keys(&s_state, gameplay);
    s_roles.keyboard_reports++;
    // Keep the translator clock moving even on a keyboard-only report so a
    // stale mouse deflection cannot be frozen by keyboard traffic.
    ns2_kbm_state_service(&s_state, active_mouse(), platform_time_ms());
    publish_locked();
    return true;
}

bool ns2_kbm_runtime_submit_mouse(const input_event_t *event) {
    if (!event) return false;
    if (!admit_and_route(event, false)) return false;

    sync_config();
    note_battery(event);
    // Bind by HID Usage Page 0x09 button number, which is the identity a UI can
    // present and persist. The event's JP_BUTTON_* projection is deliberately
    // not used: it is lossy and its inverse would silently change meaning if
    // the mouse driver's gamepad mapping ever did.
    uint16_t buttons = event->hid_buttons;

    ns2_kbm_state_mouse_report(&s_state, buttons, event->delta_x, event->delta_y,
                               event->delta_wheel, active_mouse(),
                               platform_time_ms());
    s_roles.mouse_reports++;
    publish_locked();
    return true;
}

bool ns2_kbm_runtime_disconnect(uint8_t conn_index, int8_t instance,
                                uint32_t connection_generation) {
    (void)instance;
    ns2_kbm_peer_key_t key;
    memset(&key, 0, sizeof(key));
    key.valid = 1u;
    key.conn_index = conn_index;
    key.connection_generation = connection_generation;
    const bthid_device_t *device = bthid_get_device(conn_index);
    if (device) {
        memcpy(key.addr, device->bd_addr, sizeof(key.addr));
        for (unsigned i = 0; i < sizeof(key.addr); ++i) {
            if (key.addr[i] != 0u) {
                key.addr_valid = 1u;
                break;
            }
        }
        if (key.connection_generation == 0u)
            key.connection_generation = device->connection_generation;
    }

    bool released_keyboard = false;
    bool released_mouse = false;
    if (!ns2_kbm_roles_release(&s_roles, &key, &released_keyboard,
                               &released_mouse)) {
        peer_forget(conn_index, connection_generation);
        return false;
    }
    peer_forget(conn_index, connection_generation);

    // Clear exactly what the departed role owned; the surviving role keeps its
    // state and keeps publishing.
    if (released_keyboard) ns2_kbm_state_clear_keyboard(&s_state);
    if (released_mouse) ns2_kbm_state_clear_mouse(&s_state);
    // The arbiter may hand the owning token to the surviving member, so the
    // cached handle is no longer trustworthy; re-resolve it on the next report.
    s_source_id = NS2_INPUT_SOURCE_ID_NONE;
    if (!s_roles.keyboard.valid && !s_roles.mouse.valid) {
        s_battery_valid = 0u;
        s_battery_level = 0u;
        s_battery_charging = 0u;
        s_source_id = 0u;
        s_identity_name[0] = '\0';
        s_identity_vid = 0u;
        s_identity_pid = 0u;
    } else if (s_active_valid) {
        publish_locked();
    }
    return true;
}

void ns2_kbm_runtime_service(void) {
    // This is the only unconditional core-1 tick the KB/M feature has, so it is
    // where a deferred mode change is guaranteed to land -- including a change
    // to Controller mode, where no KB/M report will ever arrive to carry it.
    apply_pending_mode_change();

    if (ns2_kbm_runtime_mode() != NS2_KBM_MODE_KEYBOARD_MOUSE) return;
    if (!s_active_valid || !s_state.mouse_present) return;
    // Ticking is driven by the translator still owning motion state, not by the
    // deflection alone: a velocity estimate below one stick unit still has to be
    // advanced so the inactivity deadline lands and the gesture ends exactly.
    if (!ns2_kbm_state_mouse_motion_pending(&s_state)) return;
    if (output_supports_native_mouse()) return;  // no translated stick to recenter

    ns2_kbm_state_service(&s_state, active_mouse(), platform_time_ms());
    s_stick_recenters++;
    publish_locked();
}

// ---------------------------------------------------------------------------
// Configuration surface (core 0)
// ---------------------------------------------------------------------------

bool ns2_kbm_runtime_config_load(const ns2_kbm_config_t *config) {
    ns2_kbm_config_t candidate;
    if (config) {
        candidate = *config;
    } else {
        ns2_kbm_config_defaults(&candidate);
    }
    bool clean = ns2_kbm_config_sanitize(&candidate);
    config_write_begin();
    s_config = candidate;
    config_write_end();
    __atomic_store_n(&s_mode, s_config.mode, __ATOMIC_RELEASE);
    return clean;
}

void ns2_kbm_runtime_config_get(ns2_kbm_config_t *out) {
    if (!out) return;
    uint32_t generation = 0;
    config_snapshot(out, &generation);
}

uint32_t ns2_kbm_runtime_config_generation(void) {
    return atomic_load_u32(&s_config_generation);
}

bool ns2_kbm_runtime_set_mode(ns2_kbm_mode_t mode) {
    // Sets the OVERRIDE. NS2_KBM_MODE_AUTO restores inference.
    if (mode >= NS2_KBM_MODE_COUNT) return false;
    if ((ns2_kbm_mode_t)s_config.mode == mode) return true;

    config_write_begin();
    s_config.mode = (uint8_t)mode;
    config_write_end();
    __atomic_store_n(&s_mode, (uint8_t)mode, __ATOMIC_RELEASE);

    // Publish neutral immediately from here (core 0) so a held button on the
    // outgoing mode's source cannot survive the change even for the few
    // milliseconds before core 1 reaches the boundary below.
    report_neutralize_slot(0);
    ns2_native_motion_clear();

    // A mode change replaces the logical source outright: every role is
    // released and the whole source registry is rebuilt from the peers the new
    // mode admits. That work belongs to core 1, which is the registry's sole
    // writer -- doing it here would race an in-flight report.
    __atomic_add_fetch(&s_mode_change_request, 1u, __ATOMIC_ACQ_REL);
    return true;
}

// The layout a legacy per-binding command means when it names `kb` or `kbm`.
//
// Legacy commands mutate the REALIZED mapping directly, which is what their
// existing clients expect: `kbm bind kb ...` has always changed what the console
// does, immediately. That now makes the realized snapshot stop matching whatever
// saved profile produced it, and ns2_kbm_active_matches_source() reports that
// truthfully rather than letting a client keep claiming the profile is applied.
bool ns2_kbm_runtime_set_binding(ns2_kbm_layout_t layout,
                                 ns2_kbm_source_t source, uint8_t destination) {
    if (layout >= NS2_KBM_LAYOUT_COUNT) return false;
    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    if (!ns2_kbm_set_binding(&candidate.active[layout].content, layout, source,
                             destination))
        return false;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    return true;
}

bool ns2_kbm_runtime_clear_binding(ns2_kbm_layout_t layout,
                                   ns2_kbm_source_t source) {
    if (layout >= NS2_KBM_LAYOUT_COUNT) return false;
    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    if (!ns2_kbm_clear_binding(&candidate.active[layout].content, layout, source))
        return false;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    return true;
}

// Legacy reset: realize the layout's built-in Default. Same visible effect the
// command always had.
void ns2_kbm_runtime_reset_layout(ns2_kbm_layout_t layout) {
    if (layout >= NS2_KBM_LAYOUT_COUNT) return;
    config_write_begin();
    (void)ns2_kbm_apply(&s_config, layout, NS2_KBM_PROFILE_ID_DEFAULT, NULL);
    config_write_end();
}

void ns2_kbm_runtime_config_snapshot(ns2_kbm_config_t *out) {
    if (!out) return;
    uint32_t generation = 0;
    config_snapshot(out, &generation);
}

// The profile lifecycle, all in the same shape: mutate a SNAPSHOT, and publish
// it only if the model accepted the change. A rejected operation must not leave
// the live configuration half-edited, and core 1 may be resolving against it.
uint8_t ns2_kbm_runtime_profile_create_at(ns2_kbm_layout_t layout,
                                          uint8_t position, const char *name,
                                          const ns2_kbm_content_t *content) {
    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    uint8_t id = ns2_kbm_profile_create_at(&candidate, layout, position, name,
                                           content);
    if (id == NS2_KBM_PROFILE_ID_NONE) return (uint8_t)NS2_KBM_PROFILE_ID_NONE;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    return id;
}

uint8_t ns2_kbm_runtime_profile_create(ns2_kbm_layout_t layout,
                                       const char *name,
                                       const ns2_kbm_content_t *content) {
    return ns2_kbm_runtime_profile_create_at(layout, 0u, name, content);
}

uint16_t ns2_kbm_runtime_profile_save(uint8_t profile_id,
                                      uint16_t expected_revision,
                                      const char *name,
                                      const ns2_kbm_content_t *content) {
    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    uint16_t revision = ns2_kbm_profile_save(&candidate, profile_id,
                                             expected_revision, name, content);
    if (revision == 0u) return 0u;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    return revision;
}

bool ns2_kbm_runtime_profile_rename(uint8_t profile_id, const char *name) {
    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    if (!ns2_kbm_profile_rename(&candidate, profile_id, name)) return false;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    return true;
}

bool ns2_kbm_runtime_profile_delete(uint8_t profile_id) {
    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    if (!ns2_kbm_profile_delete(&candidate, profile_id)) return false;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    return true;
}

bool ns2_kbm_runtime_apply(ns2_kbm_layout_t layout, uint8_t profile_id,
                           bool *changed) {
    if (changed) *changed = false;
    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    bool did_change = false;
    if (!ns2_kbm_apply(&candidate, layout, profile_id, &did_change))
        return false;
    // Already realized: report success and leave the configuration generation
    // alone, so an Apply of content the adapter is already running costs no
    // flash erase at all.
    if (!did_change) return true;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    if (changed) *changed = true;
    return true;
}

bool ns2_kbm_runtime_set_boot_position(ns2_kbm_layout_t layout,
                                       uint8_t position, bool *changed) {
    if (changed) *changed = false;
    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    bool did_change = false;
    if (!ns2_kbm_set_boot_position(&candidate, layout, position, &did_change))
        return false;
    // Unchanged: no generation bump and therefore no flash write. Choosing the
    // boot profile that is already the boot profile must cost nothing.
    if (!did_change) return true;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    if (changed) *changed = true;
    return true;
}

bool ns2_kbm_runtime_position_clear(ns2_kbm_layout_t layout, uint8_t position) {
    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    if (!ns2_kbm_position_clear(&candidate, layout, position)) return false;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    // The runtime position follows the same fallback the config just took, so a
    // client reading it back never sees a selection that no longer exists.
    if (layout < NS2_KBM_LAYOUT_COUNT && s_runtime_position[layout] == position)
        s_runtime_position[layout] = (uint8_t)NS2_KBM_POSITION_DEFAULT;
    return true;
}

bool ns2_kbm_runtime_switch_bind(ns2_kbm_source_t source, uint8_t position) {
    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    if (!ns2_kbm_switch_bind(&candidate, source, position)) return false;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    return true;
}

uint8_t ns2_kbm_runtime_active_position(ns2_kbm_layout_t layout) {
    if (layout >= NS2_KBM_LAYOUT_COUNT) return NS2_KBM_POSITION_DEFAULT;
    // The RUNTIME position, which a profile-switch key moves without touching
    // the stored configuration at all.
    return s_runtime_position[layout];
}

uint32_t ns2_kbm_runtime_switch_count(void) { return s_runtime_switches; }

void ns2_kbm_runtime_reset_all(void) {
    // Mapping reset is mapping-only: the selected input mode is a separate
    // user choice and unrelated adapter settings are not touched at all.
    config_write_begin();
    ns2_kbm_mode_t mode = (ns2_kbm_mode_t)s_config.mode;
    ns2_kbm_config_defaults(&s_config);
    s_config.mode = (uint8_t)mode;
    config_write_end();
}

bool ns2_kbm_runtime_set_mouse(const ns2_kbm_mouse_config_t *mouse) {
    if (!mouse) return false;
    ns2_kbm_mouse_config_t verify = *mouse;
    // Reject rather than silently clamp: a management client that sent an
    // out-of-range value must be told, not quietly given a different one.
    if (!ns2_kbm_mouse_sanitize(&verify)) return false;

    ns2_kbm_config_t candidate;
    uint32_t generation = 0;
    config_snapshot(&candidate, &generation);
    // Mouse settings are profile-owned, so the legacy command writes them into
    // the REALIZED mappings -- both layouts, matching the global setting it used
    // to be. Same visible behaviour; the divergence it creates is now reported.
    for (uint8_t i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i)
        candidate.active[i].content.mouse = *mouse;
    config_write_begin();
    s_config = candidate;
    config_write_end();
    return true;
}

void ns2_kbm_runtime_get_mouse(ns2_kbm_mouse_config_t *out) {
    if (!out) return;
    ns2_kbm_config_t snapshot;
    uint32_t generation = 0;
    config_snapshot(&snapshot, &generation);
    // The live layout's realized settings are the ones actually in effect.
    ns2_kbm_layout_t layout = ns2_kbm_mode_layout(ns2_kbm_runtime_mode());
    *out = snapshot.active[layout].content.mouse;
}

void ns2_kbm_runtime_status(ns2_kbm_runtime_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    ns2_kbm_mode_t mode = ns2_kbm_runtime_mode();
    out->mode = (uint8_t)mode;
    out->mode_override = (uint8_t)mode_override();
    out->profile = (uint8_t)ns2_kbm_mode_layout(mode);
    out->keyboard_connected = s_roles.keyboard.valid;
    out->mouse_connected = s_roles.mouse.valid;
    out->native_mouse_output = output_supports_native_mouse() ? 1u : 0u;
    out->keyboard_conn = s_roles.keyboard.conn_index;
    out->mouse_conn = s_roles.mouse.conn_index;
    out->group_id = s_roles.group_id;
    out->source_id = s_source_id;
    out->keyboard_reports = s_roles.keyboard_reports;
    out->mouse_reports = s_roles.mouse_reports;
    out->rejected_mode = s_roles.rejected_mode;
    out->rejected_duplicate = s_roles.rejected_duplicate;
    // What the console is REALLY running, and whether that still matches the
    // profile it came from. A client must never infer "active" from an id alone:
    // the id survives a save that was not applied, and the whole point of the
    // snapshot model is that those two states are different.
    ns2_kbm_layout_t layout = ns2_kbm_mode_layout(mode);
    const ns2_kbm_active_t *active = &s_config.active[layout];
    out->active_profile = active->source_id;
    out->active_revision = active->source_revision;
    out->active_fingerprint =
        ns2_kbm_content_fingerprint(&active->content, layout);
    out->active_matches_source =
        ns2_kbm_active_matches_source(&s_config, layout) ? 1u : 0u;
    if (active->source_id == NS2_KBM_PROFILE_ID_DEFAULT) {
        strncpy(out->active_profile_name, "Default",
                sizeof(out->active_profile_name) - 1u);
    } else {
        const ns2_kbm_profile_slot_t *slot =
            ns2_kbm_profile_find(&s_config, active->source_id);
        if (slot)
            memcpy(out->active_profile_name, slot->name,
                   sizeof(out->active_profile_name));
    }
    out->rejected_not_owner = s_rejected_not_owner;
    out->rejected_no_peer_key = s_rejected_no_peer_key;
    out->rejected_unclassified = s_rejected_unclassified;
    out->rejected_no_role = s_rejected_no_role;
    out->undecoded_reports = s_undecoded_reports;
    out->rollover_reports = s_rollover_reports;
    out->role_losses = s_roles.role_losses;
    out->config_generation = atomic_load_u32(&s_config_generation);
    out->remap_neutralizations = s_remap_neutralizations;
    out->publishes = s_publishes;
    out->stick_recenters = s_stick_recenters;
}

// A rollover report carries no usable key set, so the previous held state is
// retained. Count it: a keyboard that rolls over constantly is a real hardware
// limitation a user needs to be able to see.
void ns2_kbm_runtime_note_rollover(void) { s_rollover_reports++; }

// A report reached the keyboard driver and decoded as neither a keyboard report
// nor a pointer report.
//
// This is the counter that makes a mis-parsed descriptor visible. Before it, a
// keyboard whose descriptor parsed well enough to classify but not well enough
// to decode was indistinguishable from a keyboard that was sending nothing at
// all: no counter anywhere moved.
void ns2_kbm_runtime_note_undecoded(void) { s_undecoded_reports++; }
