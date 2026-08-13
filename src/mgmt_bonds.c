#include "mgmt_bonds.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum {
    MGMT_BONDS_MAX_DECIMAL = 100000,
    // Leave enough room for the comma, closing array, and page cursor even
    // when all six address bytes and a five-digit slot index are present.
    MGMT_BONDS_PAGE_SUFFIX_RESERVE = 32,
    // The legacy command wraps the array in the v2 envelope.  Keep a margin
    // here so a complete legacy response remains safe if the caller adds a
    // small compatibility field in a future release.
    MGMT_BONDS_LEGACY_SUFFIX_RESERVE = 40,
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
        if (parsed > ((unsigned long)MGMT_BONDS_MAX_DECIMAL - digit) / 10u)
            return false;
        parsed = parsed * 10u + digit;
        if (parsed > (unsigned long)INT_MAX)
            return false;
    }
    if (value)
        *value = (int)parsed;
    return true;
}

bool mgmt_bonds_parse_command(const char *arg,
                              mgmt_bonds_action_t *action,
                              int *value)
{
    if (!arg || !action || !value)
        return false;

    *action = MGMT_BONDS_INVALID;
    *value = 0;

    if (strcmp(arg, "list") == 0) {
        *action = MGMT_BONDS_LIST_LEGACY;
        return true;
    }
    if (strcmp(arg, "list v2") == 0) {
        *action = MGMT_BONDS_LIST_PAGE;
        return true;
    }
    if (strncmp(arg, "list v2 ", 8) == 0 &&
        parse_decimal(arg + 8, value)) {
        *action = MGMT_BONDS_LIST_PAGE;
        return true;
    }
    if (strncmp(arg, "remove ", 7) == 0 &&
        parse_decimal(arg + 7, value)) {
        *action = MGMT_BONDS_REMOVE;
        return true;
    }
    return false;
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

static bool append_entry(char *output, size_t capacity, size_t *length,
                         const mgmt_bond_entry_t *entry, bool comma)
{
    char encoded[80];
    int written = snprintf(encoded, sizeof(encoded),
                           "%s{\"i\":%d,\"type\":%d,\"addr\":\"%02X%02X%02X%02X%02X%02X\"}",
                           comma ? "," : "", entry->index, entry->type,
                           entry->address[0], entry->address[1], entry->address[2],
                           entry->address[3], entry->address[4], entry->address[5]);
    if (written < 0 || (size_t)written >= sizeof(encoded))
        return false;
    return append_text(output, capacity, length, encoded);
}

static int count_entries(mgmt_bonds_entry_at_fn entry_at, void *context,
                         int slot_count)
{
    int total = 0;
    if (!entry_at || slot_count < 0)
        return 0;
    for (int slot = 0; slot < slot_count; ++slot) {
        mgmt_bond_entry_t entry;
        if (entry_at(context, slot, &entry))
            ++total;
    }
    return total;
}

size_t mgmt_bonds_format_legacy(mgmt_bonds_entry_at_fn entry_at,
                                void *context,
                                int slot_count,
                                char *output,
                                size_t capacity,
                                bool *complete)
{
    int total = count_entries(entry_at, context, slot_count);
    size_t length = 0;
    int shown = 0;
    bool fit = true;

    if (complete)
        *complete = false;
    if (!output || capacity == 0 || !entry_at || slot_count < 0)
        return 0;
    output[0] = '\0';

    if (!append_text(output, capacity, &length,
                     "{\"v\":2,\"total\":")) {
        output[0] = '\0';
        return 0;
    }
    {
        char number[16];
        snprintf(number, sizeof(number), "%d,\"bonds\":[", total);
        if (!append_text(output, capacity, &length, number)) {
            output[0] = '\0';
            return 0;
        }
    }

    for (int slot = 0; slot < slot_count; ++slot) {
        mgmt_bond_entry_t entry;
        if (!entry_at(context, slot, &entry))
            continue;
        // Do not use the last bytes of the array for an entry: the suffix
        // and terminating NUL must fit too.  A failed append is never sent.
        if (length + MGMT_BONDS_LEGACY_SUFFIX_RESERVE >= capacity ||
            !append_entry(output, capacity, &length, &entry, shown != 0)) {
            fit = false;
            break;
        }
        ++shown;
    }
    if (!fit || !append_text(output, capacity, &length, "],\"next\":null}")) {
        output[0] = '\0';
        return 0;
    }
    if (complete)
        *complete = (shown == total);
    if (shown != total) {
        output[0] = '\0';
        return 0;
    }
    return length;
}

size_t mgmt_bonds_format_page(mgmt_bonds_entry_at_fn entry_at,
                              void *context,
                              int slot_count,
                              int start_slot,
                              char *output,
                              size_t capacity,
                              mgmt_bonds_page_info_t *info)
{
    int total = count_entries(entry_at, context, slot_count);
    size_t length = 0;
    int next = -1;
    int shown = 0;

    if (info) {
        info->total = total;
        info->next = -1;
        info->complete = false;
    }
    if (!output || capacity == 0 || !entry_at || slot_count < 0 ||
        start_slot < 0 || start_slot > slot_count)
        return 0;
    output[0] = '\0';
    if (!append_text(output, capacity, &length,
                     "{\"v\":2,\"total\":")) {
        output[0] = '\0';
        return 0;
    }
    {
        char number[16];
        snprintf(number, sizeof(number), "%d,\"bonds\":[", total);
        if (!append_text(output, capacity, &length, number)) {
            output[0] = '\0';
            return 0;
        }
    }

    for (int slot = start_slot; slot < slot_count; ++slot) {
        mgmt_bond_entry_t entry;
        if (!entry_at(context, slot, &entry))
            continue;
        if (length + MGMT_BONDS_PAGE_SUFFIX_RESERVE >= capacity ||
            !append_entry(output, capacity, &length, &entry, shown != 0)) {
            next = slot;
            break;
        }
        ++shown;
    }
    if (shown == 0 && next >= 0) {
        // A page must make progress.  If even one entry cannot fit, fail
        // closed so a client cannot loop forever on the same cursor.
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
