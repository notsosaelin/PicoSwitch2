#ifndef MGMT_BONDS_H
#define MGMT_BONDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_wireless_bridge.h"

// Bond-list replies are newline-framed through a 512-byte bridge slot.  The
// JSON payload therefore must be at most 511 bytes (the final byte is the
// bridge-owned newline).
#define MGMT_BONDS_PROTOCOL_VERSION 2u
#define MGMT_BONDS_RESPONSE_CAPACITY CONFIG_WIRELESS_RESPONSE_CAPACITY

typedef enum {
    MGMT_BONDS_INVALID = 0,
    MGMT_BONDS_LIST_LEGACY,
    MGMT_BONDS_LIST_PAGE,
    MGMT_BONDS_REMOVE,
} mgmt_bonds_action_t;

// Parse the argument after the "bonds " command prefix.  LIST_LEGACY keeps
// the original "bonds list" spelling; LIST_PAGE is the versioned
// "bonds list v2 [cursor]" form.  The output value is zero for list actions
// and is the requested cursor/index for the other actions.
bool mgmt_bonds_parse_command(const char *arg,
                              mgmt_bonds_action_t *action,
                              int *value);

typedef struct {
    int index;                 // le_device_db slot index
    int type;                  // BTstack address type
    uint8_t address[6];
} mgmt_bond_entry_t;

// Return one active entry for a BTstack device-db slot.  A false result means
// that the slot is empty.  The callback is invoked only on the BTstack core.
typedef bool (*mgmt_bonds_entry_at_fn)(void *context, int slot,
                                       mgmt_bond_entry_t *entry);

typedef struct {
    int total;
    int next;                  // -1 means complete; otherwise next slot cursor
    bool complete;
} mgmt_bonds_page_info_t;

// Format the backward-compatible "bonds list" response.  It includes the
// v2 envelope and a null next cursor, while retaining the existing "bonds"
// array field used by older clients.  Returns zero and sets complete=false if
// the complete list cannot fit; callers must then send a compact error rather
// than expose a partial array.
size_t mgmt_bonds_format_legacy(mgmt_bonds_entry_at_fn entry_at,
                                void *context,
                                int slot_count,
                                char *output,
                                size_t capacity,
                                bool *complete);

// Format one versioned page beginning at a BTstack device-db slot cursor.
// `next` is null when all active slots are present; otherwise it is the slot
// cursor for the next request.  The returned JSON is always bounded by
// `capacity`; zero indicates an invalid range or an unexpectedly tiny buffer.
size_t mgmt_bonds_format_page(mgmt_bonds_entry_at_fn entry_at,
                              void *context,
                              int slot_count,
                              int start_slot,
                              char *output,
                              size_t capacity,
                              mgmt_bonds_page_info_t *info);

#endif
