/*
 * Spec + host test for the NEW `bonds` management command grammar
 * (saved-pairing management from the app/portal). Test-first: defines the exact
 * argument parsing the future cmd_bonds() handler and the app must agree on,
 * before any firmware exists. The runtime bond enumeration/removal (le_device_db
 * / gap_delete_bonding) is firmware-integration validated on hardware; only the
 * pure grammar is pinned here.
 *
 * Command forms (handle_line dispatches "bonds " -> cmd_bonds(arg)):
 *   "list"        -> enumerate saved bonds
 *   "remove <n>"  -> delete the bond at index n (n >= 0; range checked at runtime)
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Itools \
 *   tools/test_bonds_command.c -o build/host-tests/test_bonds_command.exe
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum { BONDS_INVALID = 0, BONDS_LIST, BONDS_REMOVE } bonds_action_t;

// Parse the argument that follows "bonds ". Returns the action; for BONDS_REMOVE
// also writes a non-negative index. Strict: exactly "list", or "remove " + one
// non-negative decimal integer with no trailing characters. Everything else is
// BONDS_INVALID (the handler replies with a usage error).
static bonds_action_t bonds_parse(const char *arg, long *index) {
    if (!arg) return BONDS_INVALID;

    // "list" exactly (no trailing space/args).
    if (arg[0] == 'l') {
        const char *p = "list";
        size_t i = 0;
        for (; p[i] && arg[i] == p[i]; ++i) { }
        if (p[i] == '\0' && arg[i] == '\0') return BONDS_LIST;
        return BONDS_INVALID;
    }

    // "remove " + digits, nothing else.
    const char *r = "remove ";
    size_t i = 0;
    for (; r[i] && arg[i] == r[i]; ++i) { }
    if (r[i] != '\0') return BONDS_INVALID;      // prefix didn't fully match
    const char *num = arg + i;
    if (*num == '\0') return BONDS_INVALID;      // "remove" / "remove " with no number
    long value = 0;
    for (const char *c = num; *c; ++c) {
        if (*c < '0' || *c > '9') return BONDS_INVALID;   // non-digit / trailing junk / negative
        value = value * 10 + (*c - '0');
        if (value > 100000) return BONDS_INVALID;         // absurd -> reject (overflow guard)
    }
    if (index) *index = value;
    return BONDS_REMOVE;
}

#define OKV(arg, act, idx)                                                     \
    do {                                                                       \
        long _i = -777;                                                        \
        assert(bonds_parse((arg), &_i) == (act));                              \
        if ((act) == BONDS_REMOVE) assert(_i == (idx));                        \
    } while (0)
#define BAD(arg) assert(bonds_parse((arg), NULL) == BONDS_INVALID)

int main(void) {
    // Valid.
    OKV("list", BONDS_LIST, 0);
    OKV("remove 0", BONDS_REMOVE, 0);
    OKV("remove 1", BONDS_REMOVE, 1);
    OKV("remove 15", BONDS_REMOVE, 15);
    OKV("remove 100", BONDS_REMOVE, 100);   // range checked at runtime, not here

    // Weird/hostile: strictly rejected.
    BAD(NULL);
    BAD("");
    BAD("list ");            // trailing space
    BAD("listx");            // superstring
    BAD("LIST");             // case-sensitive
    BAD("remove");           // no index
    BAD("remove ");          // no index
    BAD("remove abc");       // non-numeric
    BAD("remove -1");        // negative (the '-' is a non-digit)
    BAD("remove 1x");        // trailing junk
    BAD("remove 1 2");       // second token
    BAD("remove  1");        // double space (second space is non-digit)
    BAD("remove 0x1");       // hex not accepted
    BAD("clear");            // unrelated word
    BAD("removeall");        // no space
    BAD("remove 999999999"); // absurd index rejected by the overflow guard

    puts("bonds command grammar tests passed");
    return 0;
}
