#ifndef NS2_ACTIVE_INPUT_H
#define NS2_ACTIVE_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/input_event.h"
#include "ns2_input_arbiter.h"

// Firmware seam wrapper around the pure arbiter.  Source drivers continue to
// call the existing joypad-os entry points; ns2_seam invokes these hooks before
// writing report.c state.
void ns2_active_input_init(void);

bool ns2_active_input_submit(const input_event_t *event,
                             ns2_input_route_decision_t *decision);

// Composite form for a feature whose one logical source is backed by more than
// one Bluetooth peer (Keyboard + Mouse).  `group_id` is the composite handle
// owned by that feature; 0 behaves exactly like ns2_active_input_submit().
bool ns2_active_input_submit_group(const input_event_t *event,
                                   uint32_t group_id,
                                   ns2_input_route_decision_t *decision);

// Opaque source id currently registered for this connection, or 0.
uint32_t ns2_active_input_source_id_for(uint8_t conn_index,
                                        uint32_t connection_generation);

// Drop every registered source and leave the console neutral and unowned.
// Used when an admission-policy change (an input-mode switch) invalidates the
// entire registry; the caller re-registers whichever peers remain eligible.
void ns2_active_input_reset(void);
// Returns true only when the disconnected source owned the active output.
bool ns2_active_input_disconnected(uint8_t dev_addr, int8_t instance);
// Token-aware disconnect used by HID driver lifecycle callbacks.  A stale
// callback carrying an older generation cannot remove a replacement source
// that reused the same transport index.
bool ns2_active_input_disconnected_generation(uint8_t dev_addr,
                                              int8_t instance,
                                              uint32_t connection_generation);

// Raw HID and native Pro2 motion arrive on paths that do not carry an
// input_event_t.  They use the same source registry/active token.
void ns2_active_input_note_connection(uint8_t conn_index);
bool ns2_active_input_connection_is_active(uint8_t conn_index);
bool ns2_active_input_connection_is_active_generation(
    uint8_t conn_index, uint32_t connection_generation);
uint32_t ns2_active_input_connection_generation(uint8_t conn_index);

// Refresh source metadata after a driver rebind without creating a new source
// or revoking the existing logical owner.
void ns2_active_input_rebind(uint8_t conn_index);

// Queue a source id from the management/UART surface.  The caller receives a
// compact status snapshot separately; the neutral boundary is published now,
// while the active token commits at the next core-1 input boundary.
bool ns2_active_input_request(uint32_t source_id);
void ns2_active_input_status(ns2_input_arbiter_status_t *status);

#endif  // NS2_ACTIVE_INPUT_H
