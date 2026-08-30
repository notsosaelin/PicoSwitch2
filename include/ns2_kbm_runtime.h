#ifndef _NS2_KBM_RUNTIME_H_
#define _NS2_KBM_RUNTIME_H_

// Firmware adapter for the Bluetooth Keyboard / Keyboard + Mouse model.
//
// ns2_kbm.c owns the portable contract (mappings, merge, translation, roles).
// This module owns everything that needs the firmware around it: which peers
// hold which role, the source-arbiter integration, the cross-core configuration
// copy, publishing to report.c, and diagnostics.
//
// Ownership of the two cores:
//   core 0  management/UART command handling, configuration mutation, persistence
//   core 1  Bluetooth reports, role admission, resolve, publish
// Configuration crosses that boundary through a generation counter plus a
// seqlock snapshot, so core 1 never reads a half-written override table and a
// mapping change always lands on an explicit neutral boundary.

#include <stdbool.h>
#include <stdint.h>

#include "core/input_event.h"
#include "ns2_kbm.h"

void ns2_kbm_runtime_init(void);

// --- selected input mode -----------------------------------------------------
// The EFFECTIVE mode: derived from which roles are actually held, so pairing an
// ordinary HID device works without the user selecting a mode first.
ns2_kbm_mode_t ns2_kbm_runtime_mode(void);
// Sets the persisted OVERRIDE (NS2_KBM_MODE_AUTO restores inference). Returns
// false for an out-of-range value. Changing it releases every KB/M role,
// neutralizes the console slot, and re-evaluates every connected peer, so the
// previous composition can never keep publishing.
bool ns2_kbm_runtime_set_mode(ns2_kbm_mode_t mode);

// --- configuration -----------------------------------------------------------
// Adopt a persisted (or default) blob. Returns false when the blob had to be
// repaired, which the caller may surface as "persisted mapping data was
// unusable". Never fails closed into an unusable state.
bool ns2_kbm_runtime_config_load(const ns2_kbm_config_t *config);
void ns2_kbm_runtime_config_get(ns2_kbm_config_t *out);
uint32_t ns2_kbm_runtime_config_generation(void);

// --- legacy per-binding surface ----------------------------------------------
// These mutate the layout's REALIZED mapping directly and immediately, which is
// what their existing clients have always relied on. The realized mapping then
// stops matching whatever saved profile produced it, and the status reply says
// so rather than letting a client keep claiming the profile is applied.
//
// The profile editors deliberately do NOT use these: an editor that wrote one
// command per changed key would erase flash once per keystroke and could not
// offer Save or Discard at all.
bool ns2_kbm_runtime_set_binding(ns2_kbm_layout_t layout,
                                 ns2_kbm_source_t source, uint8_t destination);
bool ns2_kbm_runtime_clear_binding(ns2_kbm_layout_t layout,
                                   ns2_kbm_source_t source);
void ns2_kbm_runtime_reset_layout(ns2_kbm_layout_t layout);

// --- profile library ---------------------------------------------------------
// A snapshot of the whole KB/M configuration, for formatting replies.
void ns2_kbm_runtime_config_snapshot(ns2_kbm_config_t *out);

uint8_t ns2_kbm_runtime_profile_create(ns2_kbm_layout_t layout,
                                       const char *name,
                                       const ns2_kbm_content_t *content);
uint16_t ns2_kbm_runtime_profile_save(uint8_t profile_id,
                                      uint16_t expected_revision,
                                      const char *name,
                                      const ns2_kbm_content_t *content);
bool ns2_kbm_runtime_profile_rename(uint8_t profile_id, const char *name);
bool ns2_kbm_runtime_profile_delete(uint8_t profile_id);

// Realize a profile (or NS2_KBM_PROFILE_ID_DEFAULT) into a layout's active
// mapping. `changed` is false when the content was already realized, in which
// case no configuration write happened at all.
// Persist which resident slot a layout realizes at power-up, and realize it.
// The ONLY profile selection worth a flash write: Apply and a profile-switch key
// are both runtime-only.
bool ns2_kbm_runtime_set_boot_profile(ns2_kbm_layout_t layout,
                                      uint8_t profile_id, bool *changed);

// Assign or clear one profile-switch key. NS2_KBM_PROFILE_ID_NONE clears.
bool ns2_kbm_runtime_switch_bind(ns2_kbm_layout_t layout,
                                 ns2_kbm_source_t source, uint8_t profile_id);

// The slot this layout is realizing RIGHT NOW, which a switch key changes
// without touching stored configuration.
uint8_t ns2_kbm_runtime_active_profile(ns2_kbm_layout_t layout);

// How many times a switch key has selected a profile this session.
uint32_t ns2_kbm_runtime_switch_count(void);

bool ns2_kbm_runtime_apply(ns2_kbm_layout_t layout, uint8_t profile_id,
                           bool *changed);
void ns2_kbm_runtime_reset_all(void);
bool ns2_kbm_runtime_set_mouse(const ns2_kbm_mouse_config_t *mouse);
void ns2_kbm_runtime_get_mouse(ns2_kbm_mouse_config_t *out);

// --- source admission gate ---------------------------------------------------
// True when the KB/M feature, not the ordinary controller path, decides whether
// this connection may own the console. Callers that speculatively register a
// connected peer with the arbiter must honour it so a KB/M mode cannot be
// silently taken over by a gamepad, and so a keyboard never becomes an
// ordinary Controller-mode source.
bool ns2_kbm_runtime_gates_connection(uint8_t conn_index);

// Bind a live peer to its KB/M role from the driver bthid selected for it,
// without waiting for its first input report. Called from the same lifecycle
// hooks that register ordinary sources. Idempotent; a no-op in Controller mode
// and for peers that are neither keyboard nor pointer.
void ns2_kbm_runtime_note_ready(uint8_t conn_index);

// True when the selected mode is still looking for a role a Bluetooth Classic
// peripheral of this Class-of-Device could fill. Classic discovery admits
// gamepad-class devices only; this is what lets a keyboard or a mouse be found
// in a KB/M mode WITHOUT changing Controller mode's admission policy, and it
// stops answering as soon as the role is filled. Always false in Controller
// mode.
bool ns2_kbm_runtime_wants_peripheral(bool cod_keyboard, bool cod_pointing);

// --- report ingress (core 1) -------------------------------------------------
// Record what a peer's descriptor declares.
//
// `declares_combo` must be set ONLY from a positive statement by the device that
// it supplies both roles (a Class-of-Device "combo keyboard/pointing"
// peripheral) -- never from the mere presence of both capabilities, because
// gaming mice routinely declare keyboard usages for their macro buttons and must
// leave the keyboard role free.
//
// `strong_keyboard` is the same KIND of statement, made through the report
// descriptor instead of the Class of Device: the peer opens with a keyboard
// application collection, declares a standard modifier byte, and has real key
// capacity (see bthid_keyboard_shape()). It exists because BLE has no Class of
// Device at all, so without it a genuine BLE keyboard that also declares a
// pointer collection loses the keyboard role to capability precedence. It is
// deliberately NOT "both capabilities are present": that is exactly the gaming
// mouse, which opens with Usage(Mouse) and therefore never sets this.
void ns2_kbm_runtime_note_classification(uint8_t conn_index,
                                         uint32_t connection_generation,
                                         bool has_keyboard, bool has_pointer,
                                         bool declares_combo, bool strong_keyboard);

// Keyboard report. `usage_bitmap` is NS2_KBM_KEY_BITMAP_BYTES of held HID usage
// page 0x07 ids and is borrowed for the duration of the call.
bool ns2_kbm_runtime_submit_keyboard(const input_event_t *event,
                                     const uint8_t *usage_bitmap);

// Keyboard ingress used by the driver. Implemented in ns2_seam.c alongside
// router_submit_input() so keyboard input follows the same wake-intent rules as
// every other source; it forwards to ns2_kbm_runtime_submit_keyboard().
void router_submit_keyboard_input(const input_event_t *event,
                                  const uint8_t *usage_bitmap);
// Mouse report. Returns false when this mode/peer has no mouse role, in which
// case the caller keeps the legacy Controller-mode mouse behavior.
bool ns2_kbm_runtime_submit_mouse(const input_event_t *event);

// Record that a keyboard reported ErrorRollOver: the report carried no usable
// key set and the previous held state was retained.
void ns2_kbm_runtime_note_rollover(void);

// A report reached the keyboard driver and decoded as neither a keyboard nor a
// pointer report -- what a mis-parsed report descriptor looks like downstream.
void ns2_kbm_runtime_note_undecoded(void);

// Peer teardown. Returns true when a KB/M role was released.
bool ns2_kbm_runtime_disconnect(uint8_t conn_index, int8_t instance,
                                uint32_t connection_generation);

// Periodic maintenance on core 1: advances the mouse-to-stick recenter and
// republishes while the translated stick is off centre. Cheap no-op in
// Controller mode.
void ns2_kbm_runtime_service(void);

// --- diagnostics -------------------------------------------------------------
typedef struct {
    uint8_t mode;           // EFFECTIVE mode, inferred from the role composition
    uint8_t mode_override;  // persisted override; NS2_KBM_MODE_AUTO = infer
    uint8_t profile;
    uint8_t keyboard_connected;
    uint8_t mouse_connected;
    uint8_t native_mouse_output;
    uint8_t keyboard_conn;
    uint8_t mouse_conn;
    uint32_t group_id;
    uint32_t source_id;
    uint32_t keyboard_reports;
    uint32_t mouse_reports;
    uint32_t rejected_mode;
    uint32_t rejected_duplicate;
    // What the live layout is REALLY running.
    //
    // `active_profile` is the stable id of the profile that produced the
    // realized mapping (NS2_KBM_PROFILE_ID_DEFAULT for the built-in template).
    // `active_matches_source` is the one a UI must believe: it is false after
    // the source profile was edited and saved without applying, and after a
    // legacy per-binding write mutated the realized mapping. An id alone cannot
    // express either, which is why both are reported.
    uint8_t active_profile;
    uint8_t active_matches_source;
    uint16_t active_revision;
    uint32_t active_fingerprint;
    char active_profile_name[NS2_KBM_PROFILE_NAME_MAX];
    uint32_t rejected_not_owner;
    // Outcomes that used to be silent; see ns2_kbm_runtime.c.
    uint32_t rejected_no_peer_key;
    uint32_t rejected_unclassified;
    uint32_t rejected_no_role;
    uint32_t undecoded_reports;
    uint32_t rollover_reports;
    uint32_t role_losses;
    uint32_t config_generation;
    uint32_t remap_neutralizations;
    uint32_t publishes;
    uint32_t stick_recenters;
} ns2_kbm_runtime_status_t;

void ns2_kbm_runtime_status(ns2_kbm_runtime_status_t *out);

#endif  // _NS2_KBM_RUNTIME_H_
