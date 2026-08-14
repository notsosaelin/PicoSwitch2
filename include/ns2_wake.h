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

// App/management-initiated wake: latch a one-shot request from ANY core (e.g. a
// config `wake` command on core0). core1's ns2_wake_service consumes it and, if
// the console is asleep and a wake identity exists, issues ns2_wake_request().
// A no-op if the console is already awake. Safe cross-core: a single volatile
// boolean, same publication model as the USB-state flags.
void ns2_wake_manual_request(void);

// Outcome of the most recent app/management-initiated wake.
//
// The `wake` command can only ever confirm that the COMMAND was delivered: the
// request is latched on core0 and performed later on core1. Reporting that as
// success is misleading, so core1 records what actually happened and the app
// polls for it rather than assuming.
typedef enum {
    NS2_WAKE_RESULT_NONE = 0,      // no app wake requested since boot
    NS2_WAKE_RESULT_PENDING,       // latched, core1 has not serviced it yet
    NS2_WAKE_RESULT_ADVERTISED,    // wake advertisement actually started
    NS2_WAKE_RESULT_CONSOLE_AWAKE, // console was not asleep; nothing to do
    NS2_WAKE_RESULT_NO_IDENTITY,   // never completed a USB pairing while awake
    NS2_WAKE_RESULT_RADIO_BUSY,    // radio/advertiser refused the request
} ns2_wake_result_t;

typedef struct {
    uint8_t result;           // ns2_wake_result_t
    uint8_t console_asleep;   // console state observed at the attempt
    uint8_t identity_valid;   // a usable wake identity exists
    uint32_t attempts;        // app wake requests serviced since boot
    uint32_t last_attempt_ms; // when the last one was serviced
} ns2_wake_status_t;

void ns2_wake_get_status(ns2_wake_status_t *out);
const char *ns2_wake_result_name(uint8_t result);

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
