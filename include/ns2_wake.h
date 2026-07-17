#ifndef NS2_WAKE_H
#define NS2_WAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pairing capture is deliberately two-phase. 0x15/01 only stages the console
// address; it becomes trusted/persistent after the console sends 0x15/03.
void ns2_wake_pairing_reset(void);
bool ns2_wake_pairing_stage(const uint8_t *pairing_data, size_t len,
                            uint16_t product_id,
                            const uint8_t controller_addr_wire[6]);
void ns2_wake_pairing_commit(void);

// Request one non-blocking wake-advertising sequence using the last completed
// USB pairing identity. Must be called from BTstack/core1 context.
bool ns2_wake_request(void);

// Automatic wake coordination. Core0 publishes TinyUSB host state; core1
// latches real controller input and services the wake decision. Each distinct
// neutral-to-pressed edge can make one attempt; a held button cannot repeat it.
// A new controller session must establish a neutral baseline before a pressed
// report can become wake intent. A bare reconnect/restored startup state never
// wakes the console.
void ns2_wake_publish_usb_state(bool mounted, bool suspended, uint32_t now_ms);
void ns2_wake_controller_session_started(uint8_t source);
void ns2_wake_controller_session_ended(uint8_t source);
// Forget any startup/protocol-transition input observed in the current
// connection and require a fresh neutral baseline. Drivers use this while a
// controller is negotiating an input mode whose reports are not user intent.
void ns2_wake_controller_rebaseline(uint8_t source);
void ns2_wake_note_controller_input(uint8_t source, bool non_neutral, uint32_t now_ms);
void ns2_wake_set_input_suppressed(bool suppressed);
void ns2_wake_service(uint32_t now_ms);

#endif
