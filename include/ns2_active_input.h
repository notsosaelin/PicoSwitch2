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
// Returns true only when the disconnected source owned the active output.
bool ns2_active_input_disconnected(uint8_t dev_addr, int8_t instance);

// Raw HID and native Pro2 motion arrive on paths that do not carry an
// input_event_t.  They use the same source registry/active token.
void ns2_active_input_note_connection(uint8_t conn_index);
bool ns2_active_input_connection_is_active(uint8_t conn_index);

// Queue a source id from the management/UART surface.  The caller receives a
// compact status snapshot separately; the neutral boundary is published now,
// while the active token commits at the next core-1 input boundary.
bool ns2_active_input_request(uint32_t source_id);
void ns2_active_input_status(ns2_input_arbiter_status_t *status);

#endif  // NS2_ACTIVE_INPUT_H
