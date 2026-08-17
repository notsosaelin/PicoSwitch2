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
// Mapping profiles
// ---------------------------------------------------------------------------
typedef enum {
    NS2_KBM_PROFILE_KEYBOARD = 0,
    NS2_KBM_PROFILE_KEYBOARD_MOUSE = 1,
    NS2_KBM_PROFILE_COUNT
} ns2_kbm_profile_t;

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

typedef struct {
    uint16_t sensitivity_x;  // Q8.8 stick units per mouse count
    uint16_t sensitivity_y;
    // Time a FULL deflection takes to return to neutral with no further motion,
    // at a constant rate. Smaller deflections return proportionally sooner.
    uint16_t recenter_ms;
    uint8_t invert_x;
    uint8_t invert_y;
} ns2_kbm_mouse_config_t;

// ---------------------------------------------------------------------------
// Complete persistable configuration
// ---------------------------------------------------------------------------
// One POD blob. config.c stores it verbatim and never interprets its contents;
// ns2_kbm owns validation.
typedef struct {
    uint8_t mode;  // ns2_kbm_mode_t
    uint8_t reserved[3];
    ns2_kbm_profile_overrides_t profiles[NS2_KBM_PROFILE_COUNT];
    ns2_kbm_mouse_config_t mouse;
} ns2_kbm_config_t;

// Reset to canonical defaults (Controller mode, no overrides, default mouse).
void ns2_kbm_config_defaults(ns2_kbm_config_t *config);

// Reset exactly one profile's overrides. Every other setting is untouched.
void ns2_kbm_config_reset_profile(ns2_kbm_config_t *config,
                                  ns2_kbm_profile_t profile);

// Fail-closed validation of persisted or management-supplied configuration.
// Returns true when `config` was already entirely usable. Returns false when
// anything was unusable, after repairing it in place: an unusable field falls
// back to its canonical default rather than being reinterpreted, and an
// unusable override entry is dropped rather than being pointed at an arbitrary
// destination. Unknown future identifiers therefore never become current ones.
bool ns2_kbm_config_sanitize(ns2_kbm_config_t *config);

// Effective binding for one source in one profile: the override if present,
// otherwise the canonical default.
uint8_t ns2_kbm_binding(const ns2_kbm_config_t *config,
                        ns2_kbm_profile_t profile,
                        ns2_kbm_source_t source);

// Canonical default binding, ignoring user overrides.
uint8_t ns2_kbm_default_binding(ns2_kbm_profile_t profile,
                                ns2_kbm_source_t source);

// Set / clear / revert one binding.
//   destination == NS2_DST_NONE  -> explicit unassign (stored as an override)
//   ns2_kbm_clear_binding()      -> drop the override, restoring the default
// Returns false when the identifiers are invalid or the override table is full.
bool ns2_kbm_set_binding(ns2_kbm_config_t *config, ns2_kbm_profile_t profile,
                         ns2_kbm_source_t source, uint8_t destination);
bool ns2_kbm_clear_binding(ns2_kbm_config_t *config, ns2_kbm_profile_t profile,
                           ns2_kbm_source_t source);

// Bounded enumeration of every source that currently has a non-NONE effective
// binding in `profile`, so a UI can list the profile without reconstructing
// defaults from firmware source. Returns the number written.
#define NS2_KBM_MAX_EFFECTIVE 96u
typedef struct {
    ns2_kbm_source_t source;
    uint8_t destination;
    uint8_t overridden;  // 1 when a user override supplied this destination
} ns2_kbm_effective_t;
uint16_t ns2_kbm_effective_bindings(const ns2_kbm_config_t *config,
                                    ns2_kbm_profile_t profile,
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
    int32_t stick_x;         // mouse-to-stick deflection, stick units around 0
    int32_t stick_y;
    uint32_t last_decay_ms;
    uint8_t decay_clock_valid;
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

// Advance the mouse-to-stick recenter. Safe (and required) to call with no new
// motion; this is what guarantees the emulated stick returns to neutral and
// never latches off-center.
void ns2_kbm_state_service(ns2_kbm_state_t *state,
                           const ns2_kbm_mouse_config_t *mouse,
                           uint32_t now_ms);

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
ns2_kbm_profile_t ns2_kbm_mode_profile(ns2_kbm_mode_t mode);

const char *ns2_kbm_mode_name(ns2_kbm_mode_t mode);
bool ns2_kbm_mode_from_name(const char *name, ns2_kbm_mode_t *out);
const char *ns2_kbm_profile_name(ns2_kbm_profile_t profile);
bool ns2_kbm_profile_from_name(const char *name, ns2_kbm_profile_t *out);
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
