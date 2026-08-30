#ifndef _NS2_KBM_H_
#define _NS2_KBM_H_

// Bluetooth Keyboard / Keyboard + Mouse input model.
//
// This module is deliberately free of Pico SDK, BTstack, bthid, and report.c
// dependencies so the whole mapping/merge/translation contract is host
// testable. The firmware adapter lives in src/bt_hid/ns2_kbm_runtime.c.
//
// Layering (see docs/bluetooth/keyboard-mouse-input.md):
//
//   Bluetooth HID report -> bthid_keyboard/bthid_mouse (what is held)
//                        -> ns2_kbm_state_t            (source-owned state)
//                        -> ns2_kbm_config_t           (what it means)
//                        -> ns2_kbm_output_t           (normalized controller)
//
// The output is recomputed from the complete held-source set on every publish.
// Nothing is incrementally latched, so a duplicate binding cannot produce a
// false release and a mapping change cannot leave a destination stuck.

#include <stdbool.h>
#include <stdint.h>

#include "ns2_remap.h"

// ---------------------------------------------------------------------------
// Input mode
// ---------------------------------------------------------------------------
// The selected mode is authoritative: it decides which source kinds may own the
// console stream and which mapping profile is applied. It is never inferred
// from which peers happen to be connected.
typedef enum {
    NS2_KBM_MODE_CONTROLLER = 0,      // unchanged legacy behavior
    NS2_KBM_MODE_KEYBOARD = 1,        // one keyboard peer is the whole source
    NS2_KBM_MODE_KEYBOARD_MOUSE = 2,  // keyboard + mouse form one logical source
    // Infer the mode from what is actually admitted. This is the default and
    // the normal case: pairing an ordinary HID device must not require the user
    // to predict and pre-select a mode. The three values above remain valid as
    // explicit OVERRIDES; see ns2_kbm_effective_mode().
    NS2_KBM_MODE_AUTO = 3,
    NS2_KBM_MODE_COUNT
} ns2_kbm_mode_t;

// The persisted setting is an override, not the live mode. The live mode is
// derived from the admitted role composition:
//
//   override CONTROLLER          -> Controller (KB/M refused entirely)
//   keyboard role + mouse role   -> Keyboard + Mouse
//   keyboard role only           -> Keyboard   (or Keyboard + Mouse if the
//                                               override says so, so the KB/M
//                                               profile survives a mouse that
//                                               is merely absent)
//   mouse role only              -> Keyboard + Mouse (partial source)
//   no KB/M role                 -> Controller
ns2_kbm_mode_t ns2_kbm_effective_mode(ns2_kbm_mode_t override_mode,
                                      bool keyboard_present,
                                      bool mouse_present);

// Is the logical input source fully assembled?
//
// This drives whether Bluetooth discovery may idle. It exists as a pure
// function because getting it wrong is not cosmetic: a BLE HID peer that
// reaches ready stops the scan unconditionally (the 1-dongle-1-controller
// rule), and the host's idle safety-net cannot restore it while any link is up
// -- its final term counts BLE connections too. So whoever answers "complete?"
// with a premature `true` permanently strands the missing peer, which is how a
// keyboard connecting first made an already-powered mouse undiscoverable.
//
//   any KB/M role held -> a composite is assembled one role at a time, so it is
//                         complete only with BOTH. A partial source must keep
//                         discovery available for the role still missing.
//   no KB/M role held  -> the pre-existing "a controller is HID-ready" test,
//                         byte-for-byte: one controller completes the source and
//                         discovery may idle.
bool ns2_kbm_logical_source_complete(bool keyboard_connected,
                                     bool mouse_connected,
                                     bool controller_connected);

// ---------------------------------------------------------------------------
// Partial-source completion window
// ---------------------------------------------------------------------------
// A partial KB/M source (exactly one role held) keeps discovery available so the
// complementary role can join -- that is what makes "power both on, whichever
// connects first, the other joins" work, and what lets a power-cycled peripheral
// rejoin without re-pairing.
//
// But keyboard-only and mouse-only are legitimate, and a partial source is
// indistinguishable from an intentional one. Scanning forever on that basis
// would leave the radio aggressively discovering for as long as someone uses a
// keyboard. So a partial source gets a BOUNDED window: discovery stays available
// until it expires, then the source is treated as intentional and discovery may
// idle.
//
// Expiry changes DISCOVERY POLICY ONLY. It never neutralizes input, changes the
// effective mode, alters source ownership, or disconnects anything -- and it does
// NOT lock the topology: an explicit pairing request re-opens discovery at any
// time, and the complementary role may still join then.
#define NS2_KBM_COMPLETION_WINDOW_MS 10000u

// Which KB/M roles are held. Tracked so a change of *which* role is held counts
// as a new partial state: keyboard-only becoming mouse-only is a genuinely new
// source, not a continuation of the old window.
typedef enum {
    NS2_KBM_HELD_NONE = 0,
    NS2_KBM_HELD_KEYBOARD = 1u << 0,
    NS2_KBM_HELD_MOUSE = 1u << 1,
} ns2_kbm_held_t;

typedef struct {
    uint8_t held;        // ns2_kbm_held_t bitmask seen on the previous update
    uint8_t windowing;   // a completion window is currently running
    uint32_t started_ms; // when the current partial state was entered
} ns2_kbm_completion_t;

typedef enum {
    // Discovery may idle: the source is complete, or a partial source's window
    // has expired and is now treated as intentional.
    NS2_KBM_DISCOVERY_IDLE = 0,
    // Discovery should be running: nothing is connected, or a partial KB/M
    // source is still inside its completion window.
    NS2_KBM_DISCOVERY_SEEK,
} ns2_kbm_discovery_t;

// Advance the completion window and decide whether discovery should be running.
//
// Call on a service tick with the CURRENT role state. The window is keyed
// strictly to logical-source transitions, never to report traffic, so an
// actively used keyboard cannot hold discovery open: `now_ms` only ever decides
// whether an already-started window has expired.
//
// `state` must be zero-initialized before first use.
ns2_kbm_discovery_t ns2_kbm_completion_update(ns2_kbm_completion_t *state,
                                              bool keyboard_connected,
                                              bool mouse_connected,
                                              bool controller_connected,
                                              uint32_t now_ms);

// ---------------------------------------------------------------------------
// Discovery ownership
// ---------------------------------------------------------------------------
// Discovery must be RUNNING, not merely "not stopped". Every BLE HID peer that
// reaches ready calls btstack_host_stop_scan() unconditionally (the legacy
// 1-dongle-1-controller rule), and the host's idle safety-net cannot restore it
// while any link is up. So whatever wants discovery has to re-assert it on every
// service tick; asking "who stopped the scan?" is the wrong question, because
// any number of paths legitimately do.
//
// Two INDEPENDENT reasons discovery may be required, and neither may suppress
// the other -- the defect this encodes was the bounded completion window being
// evaluated only while no pairing window was open, so the first peer to finish
// connecting inside an explicit pairing window stopped the scan and nothing
// re-armed it for the rest of that window.
typedef enum {
    // Re-assert discovery now (idempotent; a no-op if already scanning).
    NS2_KBM_DISCOVERY_ARM = 0,
    // Discovery may be retired.
    NS2_KBM_DISCOVERY_RETIRE,
    // Leave scan state alone. Used while an explicit pairing window owns
    // discovery and the source is already complete: the window closes itself,
    // and retiring here would break controller replacement, which deliberately
    // keeps scanning with a controller still connected.
    NS2_KBM_DISCOVERY_LEAVE,
} ns2_kbm_discovery_action_t;

// `timed` is the completion-window verdict from ns2_kbm_completion_update().
// An explicit pairing window is authoritative and outranks it.
ns2_kbm_discovery_action_t ns2_kbm_discovery_policy(bool pairing_window_open,
                                                    bool source_complete,
                                                    ns2_kbm_discovery_t timed);

// ---------------------------------------------------------------------------
// Mapping layouts
// ---------------------------------------------------------------------------
// The SHAPE of a mapping: which source inputs exist and which canonical
// defaults apply. Derived from the admitted roles and never chosen -- which
// roles are filled is a fact, and letting a user assert Keyboard + Mouse with no
// mouse would silently drop the right stick.
//
// Deliberately NOT called a profile. A profile is a named set of overrides the
// user selects WITHIN a layout; conflating the two is what let a binding be
// saved into a mapping the adapter was not resolving, report success, and do
// nothing at the console.
// See docs/architecture/kbm-profile-system-hld.md.
typedef enum {
    NS2_KBM_LAYOUT_KEYBOARD = 0,
    NS2_KBM_LAYOUT_KEYBOARD_MOUSE = 1,
    NS2_KBM_LAYOUT_COUNT
} ns2_kbm_layout_t;

// ---------------------------------------------------------------------------
// Stable source-input identity
// ---------------------------------------------------------------------------
// Keyboard inputs are identified by their HID Usage Page 0x07 usage id, not by
// a translated character and not by a report byte offset: the usage is stable
// across layouts, modifier state, report formats, persistence, management
// readback, and the later UX editor. Modifier keys are ordinary usages
// (0xE0..0xE7) in that same space, so there is exactly one keyboard identity
// domain.
//
// Mouse inputs are identified by HID Usage Page 0x09 button number (1-based).
#define NS2_KBM_SRC_NONE 0u
#define NS2_KBM_SRC_KEY 1u    // code = HID usage page 0x07 usage id
#define NS2_KBM_SRC_MOUSE 2u  // code = mouse button number, 1..NS2_KBM_MOUSE_BUTTONS

#define NS2_KBM_KEY_USAGE_MAX 0xE7u  // highest Usage Page 0x07 id this build accepts
#define NS2_KBM_KEY_BITMAP_BYTES 32u // covers usages 0x00..0xFF
#define NS2_KBM_MOUSE_BUTTONS 5u     // primary, secondary, middle, back, forward

typedef struct {
    uint8_t kind;  // NS2_KBM_SRC_*
    uint8_t code;
} ns2_kbm_source_t;

static inline bool ns2_kbm_source_equal(ns2_kbm_source_t a, ns2_kbm_source_t b) {
    return a.kind == b.kind && a.code == b.code;
}

// True when this identity names a control this build actually supports.
bool ns2_kbm_source_valid(ns2_kbm_source_t source);

// True when this destination is a legal mapping target.
bool ns2_kbm_destination_valid(uint8_t destination);

// Single authority for what a NS2_DST_* means in the Pro Controller button
// layout (buttons[3] uses the SWITCH_MASK_* layout, extra the SWITCH_EXTRA_*
// one). The locked physical-controller map in ns2_seam.c calls this too, so
// there is exactly one table and the two mapping systems cannot disagree.
// Digital stick directions are not buttons and are deliberately ignored here;
// ns2_kbm_resolve() applies them to the axes.
void ns2_kbm_apply_destination(uint8_t destination, uint8_t buttons[3],
                               uint8_t *extra);

// ---------------------------------------------------------------------------
// Sparse override model
// ---------------------------------------------------------------------------
// Canonical defaults are immutable data (NS2_KBM_DEFAULT_*). User configuration
// is a bounded sparse list of overrides on top of them, so restoring defaults is
// "drop the overrides" rather than a procedural rebuild, and unmodified bindings
// cost no storage. An override whose destination is NS2_DST_NONE is an explicit
// unassign; removing the override restores that source's canonical default.
#define NS2_KBM_MAX_OVERRIDES 48u

typedef struct {
    ns2_kbm_source_t source;
    uint8_t destination;  // NS2_DST_*, NS2_DST_NONE = explicitly unassigned
    uint8_t reserved;     // keeps the persisted entry 4-byte aligned
} ns2_kbm_override_t;

typedef struct {
    uint8_t count;
    uint8_t reserved[3];
    ns2_kbm_override_t entries[NS2_KBM_MAX_OVERRIDES];
} ns2_kbm_profile_overrides_t;

// ---------------------------------------------------------------------------
// Mouse translation configuration
// ---------------------------------------------------------------------------
// These exist only for the mouse-to-stick translator used when the selected
// output personality has no native mouse. Native Joy-Con 2 mouse output is a
// hardware-validated wire path and is deliberately NOT made configurable here.
#define NS2_KBM_MOUSE_SENS_MIN 16u     // Q8.8: 0.0625 stick units per mouse count
#define NS2_KBM_MOUSE_SENS_MAX 8192u   // Q8.8: 32 stick units per mouse count
#define NS2_KBM_MOUSE_SENS_DEFAULT 512u
#define NS2_KBM_MOUSE_RECENTER_MIN_MS 10u
#define NS2_KBM_MOUSE_RECENTER_MAX_MS 2000u
#define NS2_KBM_MOUSE_RECENTER_DEFAULT_MS 120u

// Time constant of the mouse-velocity estimate the translated stick is derived
// from. Mouse reports are impulses on a Bluetooth cadence -- real BR/EDR sniff
// and BLE connection intervals put peripherals around 8..15 ms -- so the
// estimate low-passes over a few report intervals: long enough that one missing
// or empty report cannot collapse the deflection, short enough that starting,
// stopping and reversing stay tight. Not user configurable: it is a property of
// the transport, not a preference.
#define NS2_KBM_MOUSE_VELOCITY_MS 40u

// Largest anti-deadzone the translator will apply, as a percentage of full
// scale. This is COMPENSATION for a destination that discards the bottom of the
// stick range, not a general-purpose boost: a value above the game's real
// deadzone produces a minimum non-zero turn rate on the smallest movement, and
// holds it through the release tail. The cap keeps an obviously wrong value from
// being configurable at all.
#define NS2_KBM_MOUSE_ADZ_MAX 50u
// Off. Every existing installation and every default therefore keeps exactly the
// hardware-validated linear response.
#define NS2_KBM_MOUSE_ADZ_DEFAULT 0u

// How long the translator waits after the last report that carried real
// movement before forcing EXACT centre. Comfortably longer than any normal
// report interval so an ordinary gap inside one continuous gesture cannot
// produce a pulse, and short enough that releasing the mouse ends the camera
// command immediately. By this point the velocity estimate has already decayed
// well below any game deadzone; the deadline exists to guarantee exact centre,
// not to do the decelerating.
#define NS2_KBM_MOUSE_IDLE_MS 64u

typedef struct {
    uint16_t sensitivity_x;  // Q8.8 stick units per mouse count
    uint16_t sensitivity_y;
    // Velocity reference interval. Deflection is what `sensitivity` would make
    // of the mouse movement occurring over this interval AT THE CURRENT SPEED:
    //
    //   stick = mouse_counts_per_ms * (sensitivity / 256) * recenter_ms
    //
    // Historically this was the time a full deflection took to decay back to
    // neutral under constant-rate friction. That model made deflection depend on
    // whether input could outrun the decay, so the same number is now the
    // reference interval of the velocity model that replaced it -- see
    // src/ns2_kbm.c. The stored value, range, default and direction of effect
    // (larger = more deflection held for the same motion) are all unchanged, so
    // no persisted configuration is reinterpreted into a different feel.
    uint16_t recenter_ms;
    uint8_t invert_x;
    uint8_t invert_y;
    // Percentage of full scale that genuine motion starts at, 0..
    // NS2_KBM_MOUSE_ADZ_MAX. See ns2_kbm_mouse_anti_deadzone().
    uint8_t anti_deadzone;
    uint8_t reserved;  // keeps the persisted struct free of implicit padding
} ns2_kbm_mouse_config_t;

// ---------------------------------------------------------------------------
// Profile library and active realized mapping
// ---------------------------------------------------------------------------
// Three things are deliberately kept apart, because collapsing any two of them
// is what produced the hardware failure this feature exists to fix -- a binding
// that saved, read back correctly, reported success everywhere, and did nothing
// at the console.
//
//   TEMPLATE   immutable canonical content in ROM. One Default per layout.
//              Never edited, never stored, never consumes a slot.
//
//   PROFILE    a named user mapping in the library, belonging to exactly one
//              layout. Six custom profiles exist across both layouts.
//
//   ACTIVE     the REALIZED mapping the adapter is actually resolving against,
//              stored per layout as its own copy of the content.
//
// Active is a SNAPSHOT, not a pointer into a profile slot. That is the whole
// point: saving an edit to the profile that is currently applied must not change
// what the console does until the user deliberately applies it. A pointer model
// cannot express "saved but not applied" at all.
// ---------------------------------------------------------------------------
// Profile positions
// ---------------------------------------------------------------------------
// The user chooses a POSITION WITHIN A LAYOUT, never a storage slot number.
//
//     Keyboard bank:        Default, Profile 1, Profile 2, Profile 3
//     Keyboard+Mouse bank:  Default, Profile 1, Profile 2, Profile 3
//
// Physical storage remains six generic records; the position is what a user and
// a switch key address, and the mapping between the two lives in
// ns2_kbm_profile_at(). Exposing slot numbers would make "Profile 1" mean
// different things in different layouts and force the user to think about
// firmware storage.
//
// Default is built in and occupies no record, which is what keeps all three
// custom positions per layout available.
#define NS2_KBM_POSITIONS_PER_LAYOUT 3u
#define NS2_KBM_POSITION_DEFAULT 0u  // the built-in template of the layout

#define NS2_KBM_MAX_PROFILES 6u  // CUSTOM profiles; Defaults are built in
_Static_assert(NS2_KBM_MAX_PROFILES ==
                   NS2_KBM_POSITIONS_PER_LAYOUT * 2u,
               "six records is exactly three positions in each of two layouts");
// Bytes including the NUL, so 19 usable characters. Sized so the migration's
// own names fit whole -- "Current Keyboard" and "Current KB + Mouse" -- because
// a user upgrading should recognise their existing mapping, not find it under a
// truncated abbreviation.
#define NS2_KBM_PROFILE_NAME_MAX 20u

// Stable profile identity, independent of which storage slot holds it.
//
// A companion caches drafts against an id. If ids were slot indexes, deleting a
// profile and creating another would silently rebind a stale draft to an
// unrelated mapping.
#define NS2_KBM_PROFILE_ID_NONE 0u     // no profile / invalid
#define NS2_KBM_PROFILE_ID_DEFAULT 1u  // the built-in Default of some layout
#define NS2_KBM_PROFILE_ID_FIRST 2u    // first id a custom profile may take
#define NS2_KBM_PROFILE_ID_MAX 254u

// Everything a profile OWNS, and therefore everything Apply realizes and the
// fingerprint covers.
//
// The mouse block is profile-owned rather than global: sensitivity, inversion
// and anti-deadzone are exactly what a user expects to differ between a mapping
// built for one game and a mapping built for another. Velocity/idle constants
// stay compile-time properties of the transport and are not here.
typedef struct {
    ns2_kbm_profile_overrides_t overrides;
    ns2_kbm_mouse_config_t mouse;
} ns2_kbm_content_t;

typedef struct {
    uint8_t used;
    uint8_t layout;       // ns2_kbm_layout_t
    uint8_t profile_id;   // stable, NS2_KBM_PROFILE_ID_FIRST..MAX
    // WHICH POSITION OF ITS LAYOUT'S BANK THIS RECORD OCCUPIES: 1..3.
    //
    // The user-facing identity. `profile_id` remains the stable handle a client
    // caches; `position` is what a switch key and the UI address, and it is what
    // makes "Profile 1" mean the same thing to a person in both layouts.
    // Occupies v15's reserved byte, which was always written as zero.
    uint8_t position;
    // Bumped by every successful save. A client sends the revision its draft
    // was based on; a mismatch is a conflict, never a silent overwrite.
    uint16_t revision;
    char name[NS2_KBM_PROFILE_NAME_MAX];
    ns2_kbm_content_t content;
} ns2_kbm_profile_slot_t;

// What the adapter is really resolving against for one layout.
typedef struct {
    // Which profile produced this snapshot: a custom id, or
    // NS2_KBM_PROFILE_ID_DEFAULT for the built-in template.
    uint8_t source_id;
    uint8_t reserved;
    uint16_t source_revision;
    ns2_kbm_content_t content;
} ns2_kbm_active_t;

// ---------------------------------------------------------------------------
// Profile-switch bindings
// ---------------------------------------------------------------------------
// A physical key that selects a RESIDENT SLOT, so the six stored profiles are
// usable while the adapter runs standalone with no companion connected. That is
// the entire point of the adapter holding profiles at all.
//
// THESE ARE LAYOUT-LEVEL CONFIGURATION, NOT PART OF ANY PROFILE'S MAPPING.
//
// Putting a switch key inside a profile's own bindings would let switching to a
// profile that does not define it leave the user with no way to switch again --
// a one-way door out of every other profile. They live beside the library so
// they survive whichever profile is active.
//
// No usage is reserved or hardcoded. F1..F6 are a convenient default the UI may
// offer; the user may bind any source the model accepts.
// One binding per selectable position, plus Default.
#define NS2_KBM_SWITCH_BINDINGS_MAX (NS2_KBM_POSITIONS_PER_LAYOUT + 1u)

typedef struct {
    uint8_t used;    // 0 when this entry is unassigned
    uint8_t kind;    // ns2_kbm_source_kind_t
    uint8_t code;    // HID usage, or mouse button number
    // The SEMANTIC ACTION: NS2_KBM_POSITION_DEFAULT, or 1..3.
    //
    // Not a storage slot and not a profile id. One table serves both layouts,
    // and the key resolves through whichever layout is derived at the moment it
    // is pressed -- so F2 means "Profile 1" and selects the Keyboard bank's
    // Profile 1 or the Keyboard+Mouse bank's, according to what is connected.
    // Binding to a slot id instead would force the user to configure two
    // disjoint key ranges and to know which record lives where.
    uint8_t position;
} ns2_kbm_switch_binding_t;

typedef struct {
    uint8_t mode;  // ns2_kbm_mode_t
    // Next stable id to hand out. Monotonic, wrapping past
    // NS2_KBM_PROFILE_ID_MAX and skipping ids still in use.
    uint8_t next_profile_id;
    uint8_t reserved[2];
    ns2_kbm_profile_slot_t profiles[NS2_KBM_MAX_PROFILES];
    ns2_kbm_active_t active[NS2_KBM_LAYOUT_COUNT];

    // v15 -------------------------------------------------------------------

    // Which POSITION each layout realizes AT BOOT (0 = Default, 1..3).
    //
    // Separate from `active[]` on purpose. `active[]` is the RUNTIME realized
    // snapshot and a profile-switch key rewrites it in RAM with no flash write
    // at all -- switching profiles mid-game must not erase a config sector. This
    // is the persisted choice, written only by the explicit companion action,
    // and it is what init realizes. So a power cycle always returns to a
    // deterministic profile rather than to whatever key was pressed last.
    uint8_t boot_position[NS2_KBM_LAYOUT_COUNT];
    uint8_t reserved2[2];

    // ONE table, shared by both layouts. See ns2_kbm_switch_binding_t.
    ns2_kbm_switch_binding_t switches[NS2_KBM_SWITCH_BINDINGS_MAX];
    uint8_t reserved3[NS2_KBM_SWITCH_BINDINGS_MAX * 4u];
} ns2_kbm_config_t;

// The record occupying one position of a layout's bank, or NULL when that
// position is empty. Position 0 (Default) is built in and has no record, so it
// always returns NULL -- callers resolve it through ns2_kbm_template_default().
const ns2_kbm_profile_slot_t *ns2_kbm_profile_at(const ns2_kbm_config_t *config,
                                                 ns2_kbm_layout_t layout,
                                                 uint8_t position);

// The lowest unoccupied position in a layout's bank, or 0 when the bank is full.
uint8_t ns2_kbm_free_position(const ns2_kbm_config_t *config,
                              ns2_kbm_layout_t layout);

// The semantic action a source invokes, or NS2_KBM_SWITCH_NONE.
//
// NOT layout-scoped: one physical key means the same ACTION everywhere, and the
// layout is applied when the action is resolved, not when it is looked up.
#define NS2_KBM_SWITCH_NONE 0xFFu
uint8_t ns2_kbm_switch_action(const ns2_kbm_config_t *config,
                              ns2_kbm_source_t source);

// Assign or clear one switch binding. `position` of NS2_KBM_SWITCH_NONE clears.
// Returns false when the source is invalid, the position is out of range, or the
// table is full.
bool ns2_kbm_switch_bind(ns2_kbm_config_t *config, ns2_kbm_source_t source,
                         uint8_t position);

// Resolve a position to the profile id ns2_kbm_apply() takes, THROUGH a layout.
// This is where the semantic action becomes a concrete profile, and it is the
// only place the layout is applied. NS2_KBM_PROFILE_ID_NONE when that layout's
// position is empty.
uint8_t ns2_kbm_position_profile_id(const ns2_kbm_config_t *config,
                                    ns2_kbm_layout_t layout, uint8_t position);

// Detect a profile-switch KEY-DOWN EDGE between two keyboard bitmaps.
//
// Returns the semantic ACTION (NS2_KBM_POSITION_DEFAULT or 1..3), or
// NS2_KBM_SWITCH_NONE. Held keys do not repeat: only clear-to-set fires. The
// caller resolves the action through the CURRENT derived layout.
uint8_t ns2_kbm_switch_edge(const ns2_kbm_config_t *config,
                            const uint8_t *previous, const uint8_t *current);

// Clear every bound switch source from a keyboard bitmap, so the key is
// consumed by the switch and never also emits its gameplay binding.
void ns2_kbm_switch_mask(const ns2_kbm_config_t *config, uint8_t *bitmap);

// Choose the slot this layout realizes AT POWER-UP, and realize it now.
//
// The only operation that changes boot behaviour, and the only one worth a flash
// write. ns2_kbm_apply() on its own is a RUNTIME change: it is what the app's
// Activate and a profile-switch key both do, and it costs no persistence.
// `changed` reports whether anything actually moved, so a caller can skip the
// save entirely when it did not.
bool ns2_kbm_set_boot_position(ns2_kbm_config_t *config, ns2_kbm_layout_t layout,
                               uint8_t position, bool *changed);

// Realize each layout's PERSISTED boot-active slot into its runtime snapshot.
// Called once at load, so power-up is deterministic no matter which profile a
// switch key happened to select before the adapter was last written.
void ns2_kbm_realize_boot_profiles(ns2_kbm_config_t *config);

// ---------------------------------------------------------------------------
// Content fingerprint
// ---------------------------------------------------------------------------
// A deterministic 32-bit digest of canonicalized profile-owned content.
//
// Exists so a client can answer "is what I saved what is actually running?"
// without trusting a local flag, and so Apply can skip a flash write when the
// content is already realized. NOT a security primitive, and NOT the config
// record's integrity check -- the record has neither.
//
// The canonical form sorts overrides by (kind, code) and drops entries that
// merely restate the layout's default, so two profiles that behave identically
// fingerprint identically however they were built.
uint32_t ns2_kbm_content_fingerprint(const ns2_kbm_content_t *content,
                                     ns2_kbm_layout_t layout);

// Canonicalize in place: sort overrides, drop redundant ones, clamp settings.
// Applied before storing and before fingerprinting, so the two cannot disagree.
void ns2_kbm_content_canonicalize(ns2_kbm_content_t *content,
                                  ns2_kbm_layout_t layout);

// The built-in Default template for a layout: zero overrides plus default mouse
// settings. Canonical content lives in ROM, so this is what "apply Default"
// realizes and what "reset draft from Default" produces.
void ns2_kbm_template_default(ns2_kbm_layout_t layout,
                              ns2_kbm_content_t *out);

// ---------------------------------------------------------------------------
// Profile library operations
// ---------------------------------------------------------------------------
// All operate on the LIBRARY only. None of them changes what the console is
// doing; that is exclusively ns2_kbm_apply().

// Find a profile by stable id. NULL when no live profile carries it.
const ns2_kbm_profile_slot_t *ns2_kbm_profile_find(
    const ns2_kbm_config_t *config, uint8_t profile_id);

// Create an empty custom profile for `layout`, seeded from `content` (NULL for
// the layout's Default). Returns the new stable id, or
// NS2_KBM_PROFILE_ID_NONE when storage is full or the name is unusable.
uint8_t ns2_kbm_profile_create(ns2_kbm_config_t *config,
                               ns2_kbm_layout_t layout, const char *name,
                               const ns2_kbm_content_t *content);

// Store `content` into an existing profile, guarded by `expected_revision`.
// Returns the resulting revision, or 0 when rejected -- a stale revision, an
// unknown id, or content that fails validation. Never partially applied.
uint16_t ns2_kbm_profile_save(ns2_kbm_config_t *config, uint8_t profile_id,
                              uint16_t expected_revision, const char *name,
                              const ns2_kbm_content_t *content);

bool ns2_kbm_profile_rename(ns2_kbm_config_t *config, uint8_t profile_id,
                            const char *name);
bool ns2_kbm_profile_delete(ns2_kbm_config_t *config, uint8_t profile_id);

// Is `name` already taken by another profile of this layout? Names are unique
// within a layout so a user can identify a profile by what they called it; the
// same name in the other layout is fine and common ("Default" is both).
bool ns2_kbm_profile_name_taken(const ns2_kbm_config_t *config,
                                ns2_kbm_layout_t layout, const char *name,
                                uint8_t ignore_profile_id);

// ---------------------------------------------------------------------------
// Apply: the only operation that changes console behaviour
// ---------------------------------------------------------------------------
// Copies a profile's content (or the layout's Default, for
// NS2_KBM_PROFILE_ID_DEFAULT) into that layout's realized snapshot, in one step.
//
// `changed` reports whether anything actually differed, so a caller can skip a
// flash erase when the content is already realized. False return means the
// request was refused -- unknown id, or a profile belonging to another layout --
// and nothing was touched.
bool ns2_kbm_apply(ns2_kbm_config_t *config, ns2_kbm_layout_t layout,
                   uint8_t profile_id, bool *changed);

// The realized content a layout is resolving against right now.
const ns2_kbm_content_t *ns2_kbm_active_content(const ns2_kbm_config_t *config,
                                                ns2_kbm_layout_t layout);

// Does the realized snapshot still match the saved profile that produced it?
// False after the source profile has been edited and saved without applying,
// and after a legacy per-binding write has mutated the realized mapping.
bool ns2_kbm_active_matches_source(const ns2_kbm_config_t *config,
                                   ns2_kbm_layout_t layout);

// Reset to canonical defaults (Controller mode, no custom profiles, both
// layouts realizing their Default template).
void ns2_kbm_config_defaults(ns2_kbm_config_t *config);

// Clamp profile-owned mouse settings into range. True when nothing had to be
// repaired. Exposed because canonicalization, the library and the realized
// snapshots must all agree on what is in range.
bool ns2_kbm_mouse_sanitize(ns2_kbm_mouse_config_t *mouse);

// Fail-closed validation of persisted or management-supplied configuration.
// Returns true when `config` was already entirely usable. Returns false when
// anything was unusable, after repairing it in place: an unusable field falls
// back to its canonical default rather than being reinterpreted, and an
// unusable override entry is dropped rather than being pointed at an arbitrary
// destination. Unknown future identifiers therefore never become current ones.
bool ns2_kbm_config_sanitize(ns2_kbm_config_t *config);

// Effective binding for one source in some content: the override if present,
// otherwise the layout's canonical default.
//
// Takes CONTENT rather than a profile, because the same question is asked of a
// saved profile, of a staged draft, and of the realized active snapshot. Making
// those three share one function is what keeps them from drifting.
uint8_t ns2_kbm_binding(const ns2_kbm_content_t *content,
                        ns2_kbm_layout_t layout, ns2_kbm_source_t source);

// Canonical default binding, ignoring user overrides.
uint8_t ns2_kbm_default_binding(ns2_kbm_layout_t layout,
                                ns2_kbm_source_t source);

// Set / clear / revert one binding.
//   destination == NS2_DST_NONE  -> explicit unassign (stored as an override)
//   ns2_kbm_clear_binding()      -> drop the override, restoring the default
// Returns false when the identifiers are invalid or the override table is full.
bool ns2_kbm_set_binding(ns2_kbm_content_t *content, ns2_kbm_layout_t layout,
                         ns2_kbm_source_t source, uint8_t destination);
bool ns2_kbm_clear_binding(ns2_kbm_content_t *content, ns2_kbm_layout_t layout,
                           ns2_kbm_source_t source);

// Bounded enumeration of every source that currently has a non-NONE effective
// binding in `content`, so a UI can list a mapping without reconstructing
// defaults from firmware source. Returns the number written.
#define NS2_KBM_MAX_EFFECTIVE 96u
typedef struct {
    ns2_kbm_source_t source;
    uint8_t destination;
    uint8_t overridden;  // 1 when a user override supplied this destination
} ns2_kbm_effective_t;
uint16_t ns2_kbm_effective_bindings(const ns2_kbm_content_t *content,
                                    ns2_kbm_layout_t layout,
                                    ns2_kbm_effective_t *out,
                                    uint16_t capacity);

// ---------------------------------------------------------------------------
// Live source-owned state
// ---------------------------------------------------------------------------
// Keyboard-owned and mouse-owned state are stored separately and merged only at
// resolve time, so neither role's report can erase the other's contribution and
// either role can be cleared independently on disconnect.
typedef struct {
    // Keyboard-owned
    uint8_t keys[NS2_KBM_KEY_BITMAP_BYTES];  // held HID usages, incl. modifiers
    uint8_t keyboard_present;

    // Mouse-owned
    uint16_t mouse_buttons;  // bit (n-1) = button n held
    uint8_t mouse_present;
    int16_t mouse_delta_x;   // relative motion pending for a native mouse output
    int16_t mouse_delta_y;
    int8_t mouse_delta_wheel;
    // Mouse-to-stick translator. `motion_*` is the low-pass velocity estimate in
    // scaled counts (see src/ns2_kbm.c); `stick_*` is the deflection derived
    // from it, in stick units around 0, and is the only field the output path
    // reads.
    int32_t motion_x;
    int32_t motion_y;
    int32_t stick_x;
    int32_t stick_y;
    uint32_t motion_clock_ms;    // last time the estimate was advanced
    uint32_t last_motion_ms;     // last report that carried real movement
    uint8_t motion_clock_valid;
} ns2_kbm_state_t;

void ns2_kbm_state_init(ns2_kbm_state_t *state);
// Clear everything owned by one role. The other role's state is preserved.
void ns2_kbm_state_clear_keyboard(ns2_kbm_state_t *state);
void ns2_kbm_state_clear_mouse(ns2_kbm_state_t *state);

// Replace the held-key set from a parsed keyboard report. `bitmap` is
// NS2_KBM_KEY_BITMAP_BYTES long. Marks the keyboard role present.
void ns2_kbm_state_set_keys(ns2_kbm_state_t *state, const uint8_t *bitmap);
bool ns2_kbm_state_key_held(const ns2_kbm_state_t *state, uint8_t usage);

// Apply one mouse report. Deltas accumulate; buttons replace. Marks the mouse
// role present.
void ns2_kbm_state_mouse_report(ns2_kbm_state_t *state, uint16_t buttons,
                                int16_t delta_x, int16_t delta_y,
                                int8_t delta_wheel,
                                const ns2_kbm_mouse_config_t *mouse,
                                uint32_t now_ms);

// Advance the mouse-to-stick translator to `now_ms`: decay the velocity
// estimate over the interval that elapsed and, once the inactivity deadline has
// passed, force exact centre. Safe (and required) to call with no new motion --
// a mouse that stops moving stops reporting, so this is what guarantees the
// emulated stick returns to neutral and never latches off-centre.
void ns2_kbm_state_service(ns2_kbm_state_t *state,
                           const ns2_kbm_mouse_config_t *mouse,
                           uint32_t now_ms);

// Radial anti-deadzone: map a translated stick vector's MAGNITUDE from
// [0..full] into [percent..full], rescaling both axes by the same ratio.
//
// This is an output-response mapping applied after the velocity estimator, not
// part of it. It exists because a linear velocity->stick map hands the game a
// small deflection for slow mouse movement, and a game that discards its bottom
// N% turns that into no camera movement at all -- which measured as the slowest
// N% of the entire usable speed range being invisible, at EVERY sensitivity,
// because scaling a linear map cannot change that ratio.
//
// Radial, not per-axis. Independent per-axis floors rotate the vector: a slow
// nearly-horizontal sweep (x=20%, y=1%) becomes a 26-degree diagonal, because
// the floor lifts the tiny orthogonal component as hard as the real one.
// Scaling the magnitude preserves the angle to within integer rounding.
//
// Invariants:
//   * a zero vector is returned unchanged -- exact centre stays exact centre,
//     and no amount of anti-deadzone can invent movement
//   * percent == 0 is an exact no-op, so the default reproduces the linear
//     response byte for byte
//   * a magnitude already at or beyond full scale is left alone, so this can
//     only ever raise a deflection, never shrink one
void ns2_kbm_mouse_anti_deadzone(int32_t *x, int32_t *y, uint8_t percent);

// True while the translator still owns motion state a service tick has to
// advance: a live deflection to release, or a velocity estimate still decaying.
// False means the translated stick is exactly centred and idle, so the caller
// may stop ticking until the next mouse report.
bool ns2_kbm_state_mouse_motion_pending(const ns2_kbm_state_t *state);

// ---------------------------------------------------------------------------
// Resolved normalized controller output
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t buttons[3];  // SWITCH_MASK_* layout, matching switch_pro_input_t
    uint8_t extra;       // SWITCH_EXTRA_*
    uint16_t left_x, left_y;    // 12-bit, 0x800 centered, Switch orientation
    uint16_t right_x, right_y;
    uint8_t has_mouse;          // native relative mouse data is present
    int16_t mouse_delta_x;
    int16_t mouse_delta_y;
    int8_t mouse_delta_wheel;
} ns2_kbm_output_t;

// Recompute the complete normalized state from the held sources and the active
// profile. `native_mouse` selects the output path for mouse movement:
//   true  -> relative deltas are handed to the native mouse output unchanged and
//            the right stick stays centered (Joy-Con 2 mouse mode).
//   false -> movement is translated to the right stick and no relative deltas
//            are emitted.
// Passing the state non-const lets this consume the pending relative deltas
// exactly once, matching the one-shot nature of mouse movement.
void ns2_kbm_resolve(ns2_kbm_state_t *state, const ns2_kbm_config_t *config,
                     ns2_kbm_mode_t mode, bool native_mouse,
                     ns2_kbm_output_t *out);

// Profile a mode selects. Controller mode has no KB/M profile; it reports the
// keyboard profile so callers never index out of range, but no KB/M input is
// admitted in that mode at all.
ns2_kbm_layout_t ns2_kbm_mode_layout(ns2_kbm_mode_t mode);

const char *ns2_kbm_mode_name(ns2_kbm_mode_t mode);
bool ns2_kbm_mode_from_name(const char *name, ns2_kbm_mode_t *out);
const char *ns2_kbm_layout_name(ns2_kbm_layout_t profile);
bool ns2_kbm_layout_from_name(const char *name, ns2_kbm_layout_t *out);
// Stable textual identifiers for the management/UX surface.
const char *ns2_kbm_destination_name(uint8_t destination);
bool ns2_kbm_destination_from_name(const char *name, uint8_t *out);
// "key:<hex usage>" / "mouse:<button>"; buffer must be >= 12 bytes.
void ns2_kbm_source_format(ns2_kbm_source_t source, char *out, uint16_t len);
bool ns2_kbm_source_parse(const char *text, ns2_kbm_source_t *out);

// ---------------------------------------------------------------------------
// Peer roles and admission
// ---------------------------------------------------------------------------
// One logical input source at a time remains the rule. A Keyboard source is one
// keyboard peer; a Keyboard + Mouse source is one keyboard role plus one mouse
// role, which may be two peers or a single combo peer. Admission is decided
// here, above the Bluetooth stack, so the existing controller gate is untouched.
// What a peer can do, taken from its classification -- NOT from which report
// path a given report happened to arrive on. A great many real devices declare
// both: gaming mice carry keyboard usages for macro buttons, and keyboards
// carry pointer collections for trackpads and media sticks.
typedef struct {
    uint8_t keyboard;  // declares keyboard input
    uint8_t pointer;   // declares relative pointer input
} ns2_kbm_peer_caps_t;

// Which logical role this physical HID actually REPRESENTS.
//
// Capability and role are different questions, and conflating them is a real
// hardware failure, not a hypothetical one. An ASUS ROG KERIS II gaming mouse
// reports kbcap=true AND mousecap=true because its macro buttons put a keyboard
// collection in its descriptor. It is a mouse. If "has both capabilities" meant
// "is a combo device", that one peer would occupy both roles, the composite
// would look complete, and the user's actual keyboard could never join.
//
// Capability answers "what reports can this connection emit?".
// Primary answers "which logical role does this device stand for?".
// Only primary decides role ownership.
typedef enum {
    NS2_KBM_PRIMARY_NONE = 0,
    NS2_KBM_PRIMARY_KEYBOARD = 1,
    NS2_KBM_PRIMARY_MOUSE = 2,
    // Reserved for a peer POSITIVELY identified as supplying both roles -- a
    // Class-of-Device "combo keyboard/pointing" peripheral or unifying
    // receiver, which is an explicit declaration by the device itself. It is
    // never inferred from capabilities: see ns2_kbm_primary_from_caps().
    NS2_KBM_PRIMARY_COMBO = 3,
} ns2_kbm_primary_t;

// Default classification for an unresolved KB/M peer, from capabilities alone.
//
//   pointer present            -> MOUSE   (even with keyboard usages present)
//   else keyboard present      -> KEYBOARD
//   else                       -> NONE
//
// Pointer wins deliberately. Gaming mice commonly carry auxiliary keyboard
// usages; keyboards carrying a genuine relative pointer are rarer, and a
// keyboard misclassified as a mouse still leaves the keyboard role free for the
// next peer, whereas the reverse consumes the role the user needs.
//
// COMBO is never returned here. A caller with stronger, positive evidence --
// a Class-of-Device combo declaration -- may override the result.
ns2_kbm_primary_t ns2_kbm_primary_from_caps(ns2_kbm_peer_caps_t caps);

// Resolve a peer's PRIMARY role from its capabilities and whatever
// self-declaration the transport could supply.
//
// Three inputs, three different meanings, and conflating any two of them is how
// this went wrong twice:
//
//   `caps`             what the DESCRIPTOR declares the device can emit.
//   `declares_combo`   the device stating it IS an integrated keyboard-with-
//                      pointing-device: a Classic Class-of-Device combo
//                      peripheral. One physical unit that genuinely supplies
//                      both halves.
//   `strong_keyboard`  the descriptor is unmistakably a keyboard's -- it opens
//                      with a keyboard application collection, declares a
//                      standard modifier byte, and has real key capacity
//                      (bthid_keyboard_shape()).
//
// The rules, and what each protects:
//
//   COMBO requires `declares_combo`. It means ONE peer owns BOTH logical roles,
//   which is only correct for a device that really is both -- a keyboard with an
//   integrated trackpad. A keyboard that merely declares an auxiliary pointer
//   collection is NOT that, and making it COMBO makes it squat the mouse role so
//   a separately paired mouse is refused as a duplicate.
//
//   `strong_keyboard` makes a peer KEYBOARD-primary even when it also declares
//   a pointer. This is the BLE case: BLE has no Class of Device, so without it
//   capability precedence sends a real keyboard to MOUSE and its keys never
//   reach the keyboard role. It grants the keyboard role and NOTHING ELSE; the
//   pointer capability stays recorded as metadata and the mouse role stays free
//   for a real mouse.
//
//   Otherwise capability precedence decides, pointer first -- which is what
//   keeps a gaming mouse with macro keys out of the keyboard role.
//
// Separate keyboard and mouse peers are the NORMAL Keyboard + Mouse
// composition, not a fallback: ns2_kbm_roles_t holds one slot each and they are
// meant to be filled by two different peers.
ns2_kbm_primary_t ns2_kbm_primary_from_evidence(ns2_kbm_peer_caps_t caps,
                                                bool declares_combo,
                                                bool strong_keyboard);

// Which roles the current configuration permits a peer to take. Derived by the
// runtime from the explicit override plus the native-Joy-Con-mouse exception;
// kept out of this module so the portable model has no personality knowledge.
typedef struct {
    uint8_t allow_keyboard;
    uint8_t allow_mouse;
} ns2_kbm_role_policy_t;

typedef enum {
    NS2_KBM_ADMIT_REJECT_MODE = 0,        // this kind cannot own the console now
    NS2_KBM_ADMIT_REJECT_DUPLICATE = 1,   // the role is already held by another peer
    NS2_KBM_ADMIT_KEYBOARD = 2,
    NS2_KBM_ADMIT_MOUSE = 3,
    NS2_KBM_ADMIT_BOTH = 4,               // combo peer holding both roles
} ns2_kbm_admit_t;

typedef struct {
    uint8_t valid;
    uint8_t addr[6];
    uint8_t addr_valid;
    uint8_t conn_index;
    uint32_t connection_generation;
} ns2_kbm_peer_key_t;

typedef struct {
    ns2_kbm_peer_key_t keyboard;
    ns2_kbm_peer_key_t mouse;
    // Opaque, never-reused handle shared by every peer of the current composite
    // source. It is what makes two Bluetooth peers one arbiter-visible logical
    // owner without either of them pretending to be the other.
    uint32_t group_id;
    uint32_t next_group_id;

    uint32_t keyboard_reports;
    uint32_t mouse_reports;
    uint32_t rejected_mode;
    uint32_t rejected_duplicate;
    uint32_t role_losses;
} ns2_kbm_roles_t;

void ns2_kbm_roles_init(ns2_kbm_roles_t *roles);

// Decide whether a peer may join the current logical source, and bind whichever
// permitted role(s) are free. Re-submitting an already-bound peer is idempotent.
//
// Assignment is symmetric: a peer that can do both takes both when both are
// free, and takes whichever ONE is free otherwise. A peer is rejected only when
// no role it can fill is available. (The first implementation rejected a
// keyboard-capable peer outright when the keyboard role was taken, never
// offering it the free mouse role -- which is why a mouse whose descriptor also
// declares keyboard usages could never join a live keyboard.)
ns2_kbm_admit_t ns2_kbm_roles_admit(ns2_kbm_roles_t *roles,
                                    ns2_kbm_role_policy_t policy,
                                    ns2_kbm_primary_t primary,
                                    ns2_kbm_peer_caps_t caps,
                                    const ns2_kbm_peer_key_t *key);

// Release any role held by exactly this peer identity. A key carrying an older
// connection generation matches nothing, so a stale disconnect cannot release a
// replacement peer that reused the transport index. Returns which roles were
// released via the out flags; either may be NULL.
bool ns2_kbm_roles_release(ns2_kbm_roles_t *roles,
                           const ns2_kbm_peer_key_t *key,
                           bool *released_keyboard, bool *released_mouse);

// Release every role (mode change, wipe, full source loss).
void ns2_kbm_roles_release_all(ns2_kbm_roles_t *roles);

// True when this peer identity currently holds a role.
bool ns2_kbm_roles_contains(const ns2_kbm_roles_t *roles,
                            const ns2_kbm_peer_key_t *key);

#endif  // _NS2_KBM_H_
