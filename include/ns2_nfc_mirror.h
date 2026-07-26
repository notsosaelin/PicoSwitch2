/*
 * Diagnostic bridge for observing genuine Switch 2 NFC transactions.
 *
 * The runtime bridge is disabled by default and controlled only over UART.
 * It mirrors console command-0x01 packets to a connected genuine Pro
 * Controller 2 and returns matching genuine replies through a bounded
 * cross-core slot. Outside the explicit UART gate it is completely inert.
 */
#ifndef NS2_NFC_MIRROR_H
#define NS2_NFC_MIRROR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS2_NFC_MIRROR_COMMAND_MAX 128u
#define NS2_NFC_MIRROR_EXTENDED_PREFIX 33u
#define NS2_NFC_MIRROR_FRAME_MAX \
    (NS2_NFC_MIRROR_EXTENDED_PREFIX + NS2_NFC_MIRROR_COMMAND_MAX)
#define NS2_NFC_MIRROR_RESPONSE_MAX 128u

typedef enum {
    NS2_NFC_MIRROR_OFF = 0,
    NS2_NFC_MIRROR_SUBSCRIBE_PENDING,
    NS2_NFC_MIRROR_ACTIVE,
    NS2_NFC_MIRROR_UNSUBSCRIBE_PENDING,
    NS2_NFC_MIRROR_ERROR,
} ns2_nfc_mirror_state_t;

typedef struct {
    bool requested;
    bool active;
    bool command_pending;
    bool awaiting_response;
    uint8_t state;
    uint8_t last_att_status;
    uint8_t last_send_status;
    uint8_t last_command;
    uint8_t last_subcommand;
    uint8_t report_state;
    uint16_t source_pid;
    uint16_t connection_handle;
    uint16_t last_response_length;
    uint32_t commands_submitted;
    uint32_t commands_sent;
    uint32_t notifications;
    uint32_t report_state_transitions;
    uint32_t response_timeouts;
    uint32_t rejected;
} ns2_nfc_mirror_diag_t;

// Copy an ordinary USB NFC command and change only its transport byte to BLE.
// This helper is pure and host-testable; it never infers or rewrites protocol
// fields beyond byte 2.
bool ns2_nfc_mirror_prepare_ble_command(
    uint8_t *destination, size_t destination_capacity,
    const uint8_t *source, size_t source_length);

// Build the 0x0016 characteristic value used by genuine controllers:
// 33 leading zero bytes followed by the BLE-transport command.
// Returns the complete framed length, or zero on invalid input/capacity.
size_t ns2_nfc_mirror_prepare_extended_ble_command(
    uint8_t *destination, size_t destination_capacity,
    const uint8_t *source, size_t source_length);

// Convert a genuine BLE reply header to the equivalent console USB header.
// Payload bytes beginning at offset 8 are preserved exactly.
size_t ns2_nfc_mirror_translate_ble_response(
    uint8_t *destination, size_t destination_capacity,
    const uint8_t *source, size_t source_length);

// Runtime control/snapshot. The request is serviced by the BTstack thread.
void ns2_nfc_mirror_request(bool enabled);
void ns2_nfc_mirror_snapshot(ns2_nfc_mirror_diag_t *out);

// Core0 console command submission. Returns false when the diagnostic is off,
// not yet subscribed, has no genuine Pro2 source, or its one command slot is
// occupied. It never blocks.
bool ns2_nfc_mirror_submit(const uint8_t *command, size_t length);

// BTstack-thread response publication and USB/core0 consumption.
bool ns2_nfc_mirror_accept_ble_response(
    const uint8_t *response, size_t length);
bool ns2_nfc_mirror_take_usb_response(
    uint8_t *response, size_t capacity, size_t *length);

// Genuine report-0x000E NFC state, published on the BTstack thread and read
// by the console-facing report builder only while the UART mirror is active.
uint8_t ns2_nfc_mirror_report_state(void);

#endif  // NS2_NFC_MIRROR_H
