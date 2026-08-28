#include "mgmt_peers.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum {
    MGMT_PEERS_MAX_DECIMAL = 100000,
    // Room for the closing array, the page cursor and the terminating NUL even
    // when the cursor is the widest value the parser accepts.
    MGMT_PEERS_SUFFIX_RESERVE = 32,
};

static bool parse_decimal(const char *text, int *value)
{
    if (!text || !*text)
        return false;

    unsigned long parsed = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        unsigned digit = (unsigned)(*p - '0');
        if (parsed > ((unsigned long)MGMT_PEERS_MAX_DECIMAL - digit) / 10u)
            return false;
        parsed = parsed * 10u + digit;
        if (parsed > (unsigned long)INT_MAX)
            return false;
    }
    if (value)
        *value = (int)parsed;
    return true;
}

bool mgmt_peers_id_valid(const char *id)
{
    if (!id || id[0] != 'p' || id[1] != '_')
        return false;
    for (int i = 0; i < 8; ++i) {
        char c = id[2 + i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        if (!hex)
            return false;
    }
    return id[10] == '\0';
}

bool mgmt_peers_parse_command(const char *arg,
                              mgmt_peers_action_t *action,
                              int *cursor,
                              char *id, size_t id_capacity)
{
    if (!arg || !action || !cursor)
        return false;

    *action = MGMT_PEERS_INVALID;
    *cursor = 0;
    if (id && id_capacity > 0)
        id[0] = '\0';

    if (strcmp(arg, "list") == 0) {
        *action = MGMT_PEERS_LIST;
        return true;
    }
    if (strncmp(arg, "list ", 5) == 0 && parse_decimal(arg + 5, cursor)) {
        *action = MGMT_PEERS_LIST;
        return true;
    }
    if (strncmp(arg, "forget ", 7) == 0) {
        const char *wanted = arg + 7;
        // Validated here rather than at lookup time. A malformed id must be a
        // usage error: reported as "already forgotten" it would tell the user a
        // controller was removed when nothing was even addressed.
        if (!mgmt_peers_id_valid(wanted))
            return false;
        if (id) {
            if (id_capacity < MGMT_PEERS_ID_MAX)
                return false;
            snprintf(id, id_capacity, "%s", wanted);
        }
        *action = MGMT_PEERS_FORGET;
        return true;
    }
    return false;
}

int mgmt_peers_find_by_id(const mgmt_peer_t *peers, size_t count, const char *id)
{
    if (!peers || !mgmt_peers_id_valid(id))
        return -1;
    for (size_t i = 0; i < count; ++i) {
        char candidate[MGMT_PEERS_ID_MAX];
        mgmt_peers_format_id(peers[i].address, candidate, sizeof(candidate));
        if (strcmp(candidate, id) == 0)
            return (int)i;
    }
    return -1;
}

const char *mgmt_peer_forget_outcome_name(mgmt_peer_forget_outcome_t outcome)
{
    switch (outcome) {
    case MGMT_PEER_FORGET_REMOVED:
        return "removed";
    case MGMT_PEER_FORGET_ALREADY_ABSENT:
        return "already_absent";
    case MGMT_PEER_FORGET_PROTECTED:
        return "management_peer";
    case MGMT_PEER_FORGET_INCOMPLETE:
    default:
        return "incomplete";
    }
}

size_t mgmt_peers_format_forget_result(const char *id,
                                       mgmt_peer_forget_outcome_t outcome,
                                       unsigned remaining_transports,
                                       char *output, size_t capacity)
{
    if (!output || capacity == 0)
        return 0;
    output[0] = '\0';
    if (!mgmt_peers_id_valid(id))
        return 0;

    // `ok` is false only for the refusal and the unverified delete. "Already
    // absent" is a success: the caller asked for an end state and the adapter
    // is in it.
    bool ok = outcome == MGMT_PEER_FORGET_REMOVED ||
              outcome == MGMT_PEER_FORGET_ALREADY_ABSENT;
    // The verified post-state travels with every outcome, so a client never has
    // to infer what happened from the outcome name alone.
    int written = snprintf(
        output, capacity,
        "{\"ok\":%s,\"id\":\"%s\",\"result\":\"%s\",\"bonded\":%s,\"tr\":%u}",
        ok ? "true" : "false", id,
        mgmt_peer_forget_outcome_name(outcome),
        remaining_transports ? "true" : "false",
        remaining_transports);
    if (written < 0 || (size_t)written >= capacity) {
        output[0] = '\0';
        return 0;
    }
    return (size_t)written;
}

mgmt_peer_role_t mgmt_peers_classify(const mgmt_peer_observation_t *observation)
{
    if (!observation)
        return MGMT_PEER_ROLE_UNKNOWN;
    // Order is the contract; see the header. The management client is checked
    // first because one phone can hold BOTH relationships at once.
    if (observation->is_management_client)
        return MGMT_PEER_ROLE_MANAGEMENT_COMPANION;
    if (observation->is_bridge_source)
        return MGMT_PEER_ROLE_CONTROLLER_LINK;
    if (observation->is_direct_source)
        return MGMT_PEER_ROLE_PHYSICAL_CONTROLLER;
    if (observation->is_last_connected)
        return MGMT_PEER_ROLE_PHYSICAL_CONTROLLER;
    return MGMT_PEER_ROLE_UNKNOWN;
}

/*
 * How much a role claim is worth when two observations describe one peer.
 *
 * Deliberately NOT the enum's numeric order. The enum is a wire-adjacent
 * identifier set and someone will eventually add a value to it or reorder it;
 * making precedence depend on that ordering would silently change which role
 * wins for a phone that is both the management companion and a Controller Link
 * peer. That is the exact misclassification this module exists to prevent, so
 * the ranking is written down separately.
 */
static int role_precedence(mgmt_peer_role_t role)
{
    switch (role) {
    case MGMT_PEER_ROLE_MANAGEMENT_COMPANION:
        return 3;
    case MGMT_PEER_ROLE_CONTROLLER_LINK:
        return 2;
    case MGMT_PEER_ROLE_PHYSICAL_CONTROLLER:
        return 1;
    case MGMT_PEER_ROLE_UNKNOWN:
    default:
        return 0;
    }
}

const char *mgmt_peer_role_name(mgmt_peer_role_t role)
{
    switch (role) {
    case MGMT_PEER_ROLE_MANAGEMENT_COMPANION:
        return "management";
    case MGMT_PEER_ROLE_CONTROLLER_LINK:
        return "controller_link";
    case MGMT_PEER_ROLE_PHYSICAL_CONTROLLER:
        return "controller";
    case MGMT_PEER_ROLE_UNKNOWN:
    default:
        return "unknown";
    }
}

void mgmt_peers_sanitize_name(const char *raw, char *out, size_t capacity)
{
    size_t written = 0;
    if (!out || capacity == 0)
        return;
    out[0] = '\0';
    if (!raw)
        return;

    for (const unsigned char *p = (const unsigned char *)raw;
         *p && written + 1u < capacity; ++p) {
        unsigned char c = *p;
        // Printable ASCII only, and never the two characters that would end or
        // escape the surrounding JSON string.
        if (c < 0x20u || c > 0x7Eu || c == '"' || c == '\\')
            c = ' ';
        out[written++] = (char)c;
    }
    // A name that is only replacement spaces carries nothing; trim it away so
    // the app falls back to its own label rather than rendering blanks.
    while (written > 0 && out[written - 1] == ' ')
        --written;
    out[written] = '\0';
}

void mgmt_peers_format_id(const uint8_t address[6], char *out, size_t capacity)
{
    if (!out || capacity == 0)
        return;
    out[0] = '\0';
    if (!address || capacity < 12u)
        return;
    // FNV-1a over the identity address. Chosen because it is deterministic
    // across reboots and firmware builds, needs no state, and reveals nothing
    // the address itself does not. It is a handle, not a secret.
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 6; ++i) {
        hash ^= address[i];
        hash *= 16777619u;
    }
    snprintf(out, capacity, "p_%08lX", (unsigned long)hash);
}

static int compare_address(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6);
}

static mgmt_peer_t *find_peer(mgmt_peer_t *peers, size_t count,
                              const uint8_t address[6])
{
    for (size_t i = 0; i < count; ++i) {
        if (compare_address(peers[i].address, address) == 0)
            return &peers[i];
    }
    return NULL;
}

size_t mgmt_peers_merge(const mgmt_peer_bond_t *bonds, size_t bond_count,
                        const mgmt_peer_observation_t *observations,
                        size_t observation_count,
                        mgmt_peer_t *out, size_t capacity)
{
    size_t count = 0;
    if (!out || capacity == 0)
        return 0;

    for (size_t i = 0; bonds && i < bond_count; ++i) {
        const mgmt_peer_bond_t *bond = &bonds[i];
        mgmt_peer_t *peer = find_peer(out, count, bond->address);
        if (!peer) {
            if (count >= capacity)
                break;
            peer = &out[count++];
            memset(peer, 0, sizeof(*peer));
            memcpy(peer->address, bond->address, sizeof(peer->address));
            peer->le_slot = -1;
            peer->classic_key_type = 0xFFu;
            peer->le_address_type = 0xFFu;
            peer->role = (uint8_t)MGMT_PEER_ROLE_UNKNOWN;
        }
        // A peer accumulates transports. This is the whole reason the model
        // exists: one physical device, one row, however many key records.
        peer->transport |= bond->transport;
        if (bond->transport & MGMT_PEER_TRANSPORT_LE) {
            peer->le_slot = bond->le_slot;
            peer->le_address_type = bond->le_address_type;
        }
        if (bond->transport & MGMT_PEER_TRANSPORT_BREDR)
            peer->classic_key_type = bond->classic_key_type;
    }

    for (size_t i = 0; observations && i < observation_count; ++i) {
        const mgmt_peer_observation_t *observation = &observations[i];
        mgmt_peer_t *peer = find_peer(out, count, observation->address);
        if (!peer) {
            // A connected device with no stored key is real and worth showing:
            // it is what a controller looks like mid-pairing, and hiding it
            // would make the inventory disagree with the adapter's own state.
            if (count >= capacity)
                continue;
            peer = &out[count++];
            memset(peer, 0, sizeof(*peer));
            memcpy(peer->address, observation->address, sizeof(peer->address));
            peer->le_slot = -1;
            peer->classic_key_type = 0xFFu;
            peer->le_address_type = 0xFFu;
        }
        mgmt_peer_role_t role = mgmt_peers_classify(observation);
        // Never downgrade a role already established by a stronger observation
        // of the same peer in this pass.
        if (role_precedence(role) > role_precedence((mgmt_peer_role_t)peer->role))
            peer->role = (uint8_t)role;
        if (observation->connected)
            peer->connected = 1u;
        if (peer->name[0] == '\0' && observation->name[0] != '\0')
            mgmt_peers_sanitize_name(observation->name, peer->name, sizeof(peer->name));
        // Classification and VID/PID are additive facts about one device, so the
        // first observation carrying them wins and a later one that does not know
        // them cannot erase them. A peer observed twice in one pass is the case
        // this protects: only one of those records may have had a driver bound,
        // and the answer must not depend on which arrived last.
        if (peer->classification[0] == '\0' && observation->classification[0] != '\0') {
            mgmt_peers_sanitize_name(observation->classification, peer->classification,
                                     sizeof(peer->classification));
        }
        if (peer->vendor_id == 0 && peer->product_id == 0) {
            peer->vendor_id = observation->vendor_id;
            peer->product_id = observation->product_id;
        }
    }

    // Sort by address so the page cursor addresses the same peer between calls.
    // Insertion sort: the list is at most MGMT_PEERS_MAX_ENTRIES long.
    for (size_t i = 1; i < count; ++i) {
        mgmt_peer_t key = out[i];
        size_t j = i;
        while (j > 0 && compare_address(out[j - 1].address, key.address) > 0) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = key;
    }
    return count;
}

static bool append_text(char *output, size_t capacity, size_t *length,
                        const char *text)
{
    size_t text_length;
    if (!output || !length || !text || *length >= capacity)
        return false;
    text_length = strlen(text);
    if (text_length >= capacity - *length)
        return false;
    memcpy(output + *length, text, text_length);
    *length += text_length;
    output[*length] = '\0';
    return true;
}

static bool append_peer(char *output, size_t capacity, size_t *length,
                        const mgmt_peer_t *peer, bool comma)
{
    char id[16];
    // Widest possible row: the fixed keys, a 32-char name, a 40-char
    // classification and both identifiers. Sized with headroom so a row is never
    // rejected by this buffer before the page budget has had its say.
    char encoded[256];
    mgmt_peers_format_id(peer->address, id, sizeof(id));

    int written = snprintf(
        encoded, sizeof(encoded),
        "%s{\"id\":\"%s\",\"addr\":\"%02X%02X%02X%02X%02X%02X\",\"tr\":%u,"
        "\"role\":\"%s\",\"bonded\":%s,\"conn\":%s",
        comma ? "," : "", id,
        peer->address[0], peer->address[1], peer->address[2],
        peer->address[3], peer->address[4], peer->address[5],
        (unsigned)peer->transport,
        mgmt_peer_role_name((mgmt_peer_role_t)peer->role),
        peer->transport ? "true" : "false",
        peer->connected ? "true" : "false");
    if (written < 0 || (size_t)written >= sizeof(encoded))
        return false;

    if (peer->name[0] != '\0') {
        // The name is already sanitised, so it cannot close this string.
        int extra = snprintf(encoded + written, sizeof(encoded) - (size_t)written,
                             ",\"name\":\"%s\"", peer->name);
        if (extra < 0 || (size_t)(written + extra) >= sizeof(encoded))
            return false;
        written += extra;
    }
    // Optional and omitted when unknown rather than sent empty: an absent
    // field says "this adapter cannot tell you", which is what the client must
    // render, and every byte here competes with another peer for the same page.
    if (peer->classification[0] != '\0') {
        int extra = snprintf(encoded + written, sizeof(encoded) - (size_t)written,
                             ",\"class\":\"%s\"", peer->classification);
        if (extra < 0 || (size_t)(written + extra) >= sizeof(encoded))
            return false;
        written += extra;
    }
    if (peer->vendor_id != 0 || peer->product_id != 0) {
        int extra = snprintf(encoded + written, sizeof(encoded) - (size_t)written,
                             ",\"vid\":%u,\"pid\":%u",
                             (unsigned)peer->vendor_id, (unsigned)peer->product_id);
        if (extra < 0 || (size_t)(written + extra) >= sizeof(encoded))
            return false;
        written += extra;
    }
    if ((size_t)written + 2u >= sizeof(encoded))
        return false;
    encoded[written++] = '}';
    encoded[written] = '\0';
    return append_text(output, capacity, length, encoded);
}

size_t mgmt_peers_format_page(const mgmt_peer_t *peers, size_t peer_count,
                              int start,
                              char *output, size_t capacity,
                              mgmt_peers_page_info_t *info)
{
    size_t length = 0;
    int next = -1;
    int shown = 0;

    if (info) {
        info->total = (int)peer_count;
        info->next = -1;
        info->complete = false;
    }
    if (!output || capacity == 0 || start < 0 || (size_t)start > peer_count)
        return 0;
    output[0] = '\0';

    {
        char header[40];
        snprintf(header, sizeof(header), "{\"v\":%u,\"total\":%d,\"peers\":[",
                 (unsigned)MGMT_PEERS_PROTOCOL_VERSION, (int)peer_count);
        if (!append_text(output, capacity, &length, header)) {
            output[0] = '\0';
            return 0;
        }
    }

    for (size_t i = (size_t)start; i < peer_count; ++i) {
        if (length + MGMT_PEERS_SUFFIX_RESERVE >= capacity ||
            !append_peer(output, capacity, &length, &peers[i], shown != 0)) {
            next = (int)i;
            break;
        }
        ++shown;
    }
    if (shown == 0 && next >= 0) {
        // A page must make progress, or a client following the cursor loops on
        // it forever. Fail closed and let the caller send an explicit error.
        output[0] = '\0';
        return 0;
    }
    if (next >= 0) {
        char suffix[32];
        snprintf(suffix, sizeof(suffix), "],\"next\":%d}", next);
        if (!append_text(output, capacity, &length, suffix)) {
            output[0] = '\0';
            return 0;
        }
    } else if (!append_text(output, capacity, &length, "],\"next\":null}")) {
        output[0] = '\0';
        return 0;
    }
    if (info) {
        info->next = next;
        info->complete = (next < 0);
    }
    return length;
}
