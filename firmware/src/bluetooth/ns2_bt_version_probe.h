/*
 * One-shot, read-only firmware-version probe for a genuine Switch 2
 * controller connected to PicoSwitch2's BLE host.
 *
 * The UART/core0 side requests and snapshots. btstack_host.c/core1 performs
 * native command 0x10/0x01 and publishes the raw 12-byte reply.
 */
#ifndef NS2_BT_VERSION_PROBE_H
#define NS2_BT_VERSION_PROBE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NS2_BT_VERSION_IDLE = 0,
    NS2_BT_VERSION_REQUESTED,
    NS2_BT_VERSION_SENT,
    NS2_BT_VERSION_READY,
    NS2_BT_VERSION_NO_CONTROLLER,
    NS2_BT_VERSION_PROTOCOL_ERROR,
} ns2_bt_version_state_t;

typedef struct {
    ns2_bt_version_state_t state;
    uint8_t length;
    uint8_t raw[12];
} ns2_bt_version_result_t;

/* Safe from core0; the actual GATT write is marshalled to BTstack/core1. */
void ns2_bt_version_probe_request(void);
void ns2_bt_version_probe_snapshot(ns2_bt_version_result_t *out);
const char *ns2_bt_version_state_name(ns2_bt_version_state_t state);

#endif
