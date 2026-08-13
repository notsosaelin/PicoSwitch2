/*
 * Host coverage for the production `bonds` management grammar.
 *
 * The legacy command remains `bonds list`; the bounded v2 page command is
 * `bonds list v2 [cursor]`; removal remains `bonds remove <index>`.  Parsing
 * is deliberately strict because this command is reachable over wireless.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude \
 *   tools/test_bonds_command.c src/mgmt_bonds.c \
 *   -o build/host-tests/test_bonds_command.exe
 */
#include <assert.h>
#include <stdio.h>

#include "mgmt_bonds.h"

static void expect(const char *text, mgmt_bonds_action_t action, int value)
{
    mgmt_bonds_action_t actual = MGMT_BONDS_INVALID;
    int parsed = -1;
    assert(mgmt_bonds_parse_command(text, &actual, &parsed));
    assert(actual == action);
    assert(parsed == value);
}

static void reject(const char *text)
{
    mgmt_bonds_action_t action = MGMT_BONDS_INVALID;
    int value = -1;
    assert(!mgmt_bonds_parse_command(text, &action, &value));
    assert(action == MGMT_BONDS_INVALID);
}

int main(void)
{
    expect("list", MGMT_BONDS_LIST_LEGACY, 0);
    expect("list v2", MGMT_BONDS_LIST_PAGE, 0);
    expect("list v2 0", MGMT_BONDS_LIST_PAGE, 0);
    expect("list v2 15", MGMT_BONDS_LIST_PAGE, 15);
    expect("remove 0", MGMT_BONDS_REMOVE, 0);
    expect("remove 15", MGMT_BONDS_REMOVE, 15);
    expect("remove 100000", MGMT_BONDS_REMOVE, 100000);

    reject(NULL);
    reject("");
    reject("list ");
    reject("listx");
    reject("LIST");
    reject("list v1");
    reject("list v2 ");
    reject("list v2 -1");
    reject("list v2 +1");
    reject("list v2 1x");
    reject("list v2 1 2");
    reject("list v2 100001");
    reject("remove");
    reject("remove ");
    reject("remove abc");
    reject("remove -1");
    reject("remove +1");
    reject("remove 1x");
    reject("remove 1 2");
    reject("remove  1");
    reject("remove 0x1");
    reject("remove 100001");
    reject("removeall");

    puts("bonds command grammar tests passed");
    return 0;
}
