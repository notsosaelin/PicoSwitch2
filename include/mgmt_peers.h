#ifndef MGMT_PEERS_H
#define MGMT_PEERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_wireless_bridge.h"

/*
 * Logical peer inventory for the management app.
 *
 * WHAT THIS IS NOT
 *
 * It is not the security database. BTstack owns that, in two of them: a Classic
 * link-key store and an LE device DB. This module answers a different question —
 * "which remote devices does this adapter know, and what are they to the user?" —
 * and it answers it WITHOUT ever seeing key material. The firmware glue reads
 * the databases; it copies out addresses and key TYPES and nothing else.
 *
 * WHY A SEPARATE MODEL IS NECESSARY
 *
 * A bond entry is one security record for one transport. A peer is one physical
 * device. Those differ, and the difference is not academic here: this project
 * builds with ENABLE_CROSS_TRANSPORT_KEY_DERIVATION, and the management phone is
 * simultaneously an LE management peer and (when Controller Link is running) a
 * Classic HID peer. One device, two records. Presenting the records would show
 * the user's phone twice and call it a controller.
 *
 * Entries sharing an identity address are therefore merged into one peer with a
 * transport bitmask. Known limitation: a dual-mode device whose LE identity
 * address differs from its Classic BD_ADDR appears as two peers, both
 * MGMT_PEER_ROLE_UNKNOWN. That is incomplete rather than wrong, and is preferable
 * to guessing an association the adapter cannot prove.
 *
 * Everything here is pure C with no BTstack dependency, so the merge, the role
 * ordering, the name sanitiser and the pagination are host-testable.
 */

#define MGMT_PEERS_PROTOCOL_VERSION 1u
#define MGMT_PEERS_RESPONSE_CAPACITY CONFIG_WIRELESS_RESPONSE_CAPACITY

// 16 Classic link keys + 16 LE device-DB entries is the configured capacity
// (NVM_NUM_LINK_KEYS / NVM_NUM_DEVICE_DB_ENTRIES), so no more entries than this
// can exist and the caller never needs a heap.
#define MGMT_PEERS_MAX_ENTRIES 32u
#define MGMT_PEERS_NAME_MAX 32u

// The bthid driver name that claimed a live connection ("Sony DualSense",
// "Nintendo Switch 2 Controller (BLE)"). Long enough for the longest registered
// driver name; see src/bt_hid/bt/bthid/devices/.
#define MGMT_PEERS_CLASS_MAX 40u

// "p_" plus eight hex digits plus NUL, with room to spare. Matches the buffer
// mgmt_peers_format_id() is called with.
#define MGMT_PEERS_ID_MAX 16u

// Peer roles.  UNKNOWN is a real answer and the correct one for a stored bond
// whose owner has not been seen since boot: this adapter has no persistent role
// metadata yet, and inventing a role from a bond entry alone would be exactly
// the kind of guess the project's evidence rules forbid.
//
// These values carry NO precedence meaning.  When two observations describe one
// peer, the winner is decided by role_precedence() in mgmt_peers.c, which is
// written down separately so that adding or reordering a value here cannot
// silently change which role a dual-relationship phone is given.
typedef enum {
    MGMT_PEER_ROLE_UNKNOWN = 0,
    // The phone/tablet currently holding the BLE management session.
    MGMT_PEER_ROLE_MANAGEMENT_COMPANION,
    // An Android device acting as a HID Device for Controller Link.
    MGMT_PEER_ROLE_CONTROLLER_LINK,
    // A real game controller.
    MGMT_PEER_ROLE_PHYSICAL_CONTROLLER,
} mgmt_peer_role_t;

#define MGMT_PEER_TRANSPORT_BREDR 0x01u
#define MGMT_PEER_TRANSPORT_LE 0x02u

// One stored security record, reduced to its non-secret identity.  Produced by
// the firmware glue from the Classic link-key iterator or the LE device DB.
typedef struct {
    uint8_t address[6];
    uint8_t transport;       // exactly one MGMT_PEER_TRANSPORT_* bit
    int16_t le_slot;         // LE device-DB slot, or -1 for a Classic entry
    uint8_t le_address_type; // BTstack bd_addr_type_t, meaningful for LE only
    uint8_t classic_key_type;// BTstack link_key_type_t, meaningful for BR/EDR only
} mgmt_peer_bond_t;

// What the adapter can currently SEE about a device, as opposed to what it has
// stored about one.  Live evidence is the only role evidence this phase has.
typedef struct {
    uint8_t address[6];
    uint8_t is_management_client; // drives our BLE peripheral role right now
    uint8_t is_bridge_source;     // identified as the Android Controller Bridge
    uint8_t is_direct_source;     // a controller connected to this adapter
    uint8_t is_last_connected;    // matches the JPLC reconnect record
    uint8_t connected;
    char name[MGMT_PEERS_NAME_MAX];
    // What the adapter's own driver stack decided this device IS, as opposed to
    // what the device called itself. Empty when no driver has claimed it.
    char classification[MGMT_PEERS_CLASS_MAX];
    uint16_t vendor_id;
    uint16_t product_id;
} mgmt_peer_observation_t;

typedef struct {
    uint8_t address[6];
    uint8_t transport;        // bitmask; both bits set means a multi-entry peer
    uint8_t role;             // mgmt_peer_role_t
    uint8_t connected;
    int16_t le_slot;          // -1 when the peer has no LE entry
    uint8_t le_address_type;
    uint8_t classic_key_type;
    char name[MGMT_PEERS_NAME_MAX];
    // Identity the adapter DERIVED rather than was told. See §20 of the
    // Bluetooth Management 2.0 design: a classifier answer outranks a
    // remote-supplied name, because the remote name is whatever the device says
    // it is and can be changed by its owner or spoofed outright.
    char classification[MGMT_PEERS_CLASS_MAX];
    uint16_t vendor_id;
    uint16_t product_id;
} mgmt_peer_t;

typedef struct {
    int total;
    int next;                 // -1 means complete; otherwise the next peer cursor
    bool complete;
} mgmt_peers_page_info_t;

typedef enum {
    MGMT_PEERS_INVALID = 0,
    MGMT_PEERS_LIST,
    MGMT_PEERS_FORGET,
} mgmt_peers_action_t;

/*
 * What a forget attempt did.  These are OUTCOMES, not error codes: three of the
 * four are successes, because "forget" is a request for an end state rather than
 * for an event.
 */
typedef enum {
    // A record existed and is gone.  Verified by re-enumeration, not assumed.
    MGMT_PEER_FORGET_REMOVED = 0,
    // Nothing to do.  Deliberately a SUCCESS: management replies can be lost
    // after the command was executed, so the app must be able to retry without
    // the retry reporting a failure for work that already happened.
    MGMT_PEER_FORGET_ALREADY_ABSENT,
    // The peer is this adapter's management companion.  Refused here on
    // purpose: clearing the companion's credential is a different, explicitly
    // named product action, and offering it beside the controllers is how a
    // user ends up cutting off the app that is talking to them.
    MGMT_PEER_FORGET_PROTECTED,
    // The delete ran but the peer still holds a record.  Reported rather than
    // smoothed over: a client that believes a stale "forgotten" will show a
    // pairing the adapter still has.
    MGMT_PEER_FORGET_INCOMPLETE,
} mgmt_peer_forget_outcome_t;

/*
 * Parse the argument after the "peers " command prefix.
 *
 * Grammar:
 *   list            -> MGMT_PEERS_LIST, cursor 0
 *   list <cursor>   -> MGMT_PEERS_LIST, cursor is a peer index, never a slot
 *   forget <peerId> -> MGMT_PEERS_FORGET, id copied to `id`
 *
 * `id` may be NULL when the caller only wants the action.  A syntactically
 * invalid id is rejected here rather than being looked up and reported absent,
 * so a typo cannot masquerade as "already forgotten".
 */
bool mgmt_peers_parse_command(const char *arg,
                              mgmt_peers_action_t *action,
                              int *cursor,
                              char *id, size_t id_capacity);

// Is this the exact shape mgmt_peers_format_id() produces?  "p_" + 8 uppercase
// hex digits and nothing else.
bool mgmt_peers_id_valid(const char *id);

/*
 * Resolve an opaque peer id to its index in `peers`, or -1.
 *
 * The id is a one-way hash of the identity address, so resolution is by
 * recomputing every peer's id and comparing.  That is deliberate: it keeps the
 * id opaque to clients, and it means a client can never address a peer the
 * adapter did not just report.
 */
int mgmt_peers_find_by_id(const mgmt_peer_t *peers, size_t count, const char *id);

/*
 * Format the reply for a forget attempt.
 *
 * Always reports the VERIFIED post-state, so a client refreshes into agreement
 * rather than trusting an optimistic acknowledgement.  Returns the length
 * written, or 0 if the buffer was too small.
 */
size_t mgmt_peers_format_forget_result(const char *id,
                                       mgmt_peer_forget_outcome_t outcome,
                                       unsigned remaining_transports,
                                       char *output, size_t capacity);

const char *mgmt_peer_forget_outcome_name(mgmt_peer_forget_outcome_t outcome);

/*
 * Decide a peer's role from live evidence, most confident first.
 *
 * 1. It is the connected management client.  Certain: nothing else can occupy
 *    that role.
 * 2. It was identified as the Android Controller Bridge from its HID
 *    descriptor.  Certain for the current connection.
 * 3. It is a connected input source that is not the bridge.
 * 4. It matches the stored last-connected reconnect record, which only ever
 *    holds a controller.
 * 5. Otherwise unknown.
 *
 * Rule 1 outranks rules 2 and 3 deliberately: one phone can be BOTH the
 * management companion and the Controller Link peer at the same time, and in
 * that case calling it a controller is the specific mistake this ordering
 * exists to prevent.
 */
mgmt_peer_role_t mgmt_peers_classify(const mgmt_peer_observation_t *observation);

const char *mgmt_peer_role_name(mgmt_peer_role_t role);

/*
 * Merge bond entries and live observations into sorted logical peers.
 *
 * Entries are merged by identity address.  Output is sorted by address so the
 * page cursor means the same thing between calls: the underlying databases are
 * sparse and their enumeration order is not a contract.
 *
 * Returns the number of peers written, at most `capacity`.
 */
size_t mgmt_peers_merge(const mgmt_peer_bond_t *bonds, size_t bond_count,
                        const mgmt_peer_observation_t *observations,
                        size_t observation_count,
                        mgmt_peer_t *out, size_t capacity);

/*
 * Copy a remote-supplied name into a peer, made safe for JSON and for logs.
 *
 * Remote Bluetooth names are untrusted input.  Bytes outside printable ASCII are
 * replaced rather than passed through: it costs the accents in a non-English
 * controller name, and it buys the guarantee that no name can terminate a JSON
 * string, inject a log line, or emit invalid UTF-8 from a truncated multi-byte
 * sequence.  A full UTF-8 validator would preserve those names and is the right
 * follow-up if a real device is found that needs one.
 */
void mgmt_peers_sanitize_name(const char *raw, char *out, size_t capacity);

/*
 * Format one page of peers beginning at peer index `start`.
 *
 * Never emits key bytes: `mgmt_peer_t` has nowhere to put them.  The reply is
 * always bounded by `capacity`; zero indicates an invalid range or a page that
 * could not fit even one peer, which the caller must turn into an explicit
 * error rather than an empty success.
 */
size_t mgmt_peers_format_page(const mgmt_peer_t *peers, size_t peer_count,
                              int start,
                              char *output, size_t capacity,
                              mgmt_peers_page_info_t *info);

// Opaque, stable, non-secret peer handle derived from the identity address.
// Deterministic across reboots so the app can key on it; not a slot index,
// which the LE database is free to reuse.
void mgmt_peers_format_id(const uint8_t address[6], char *out, size_t capacity);

#endif
