/*
 * Host coverage for remote controller pairing: command grammar, admission
 * policy, wrap-safe countdown, reason codes and the status envelope.
 *
 * Independent of BTstack on purpose. The firmware glue drives ONE pairing state
 * machine -- the same one the BOOTSEL gesture drives -- and everything that can
 * be got wrong in deciding what a management client may ask of it, and how the
 * answer is worded, is decided here.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude \
 *   tools/test_mgmt_pairing.c src/mgmt_pairing.c \
 *   -o build/host-tests/test_mgmt_pairing.exe
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mgmt_pairing.h"

static void test_command_grammar(void)
{
    mgmt_pairing_action_t action;
    assert(mgmt_pairing_parse_command("start", &action) && action == MGMT_PAIRING_START);
    assert(mgmt_pairing_parse_command("status", &action) && action == MGMT_PAIRING_STATUS);
    assert(mgmt_pairing_parse_command("cancel", &action) && action == MGMT_PAIRING_CANCEL);

    // No arguments, and nothing that merely starts with a verb. The window
    // duration belongs to the firmware: letting a client set it would make the
    // physical gesture's behaviour depend on what an app asked for earlier.
    assert(!mgmt_pairing_parse_command("start 60000", &action));
    assert(!mgmt_pairing_parse_command("starts", &action));
    assert(!mgmt_pairing_parse_command("", &action));
    assert(!mgmt_pairing_parse_command("Start", &action));
    assert(!mgmt_pairing_parse_command(NULL, &action));
    assert(!mgmt_pairing_parse_command("cancel ", &action));
}

static void test_admission_policy_and_precedence(void)
{
    mgmt_pairing_reason_t reason;

    assert(mgmt_pairing_start_allowed(true, false, false, &reason));
    assert(reason == MGMT_PAIRING_REASON_NONE);

    // Management disabled outranks everything: a disabled management plane
    // could not have started the operation that would make it busy.
    assert(!mgmt_pairing_start_allowed(false, true, true, &reason));
    assert(reason == MGMT_PAIRING_REASON_MANAGEMENT_DISABLED);

    // A wipe in progress outranks busy for the same shape of reason.
    assert(!mgmt_pairing_start_allowed(true, true, true, &reason));
    assert(reason == MGMT_PAIRING_REASON_LOCKED_OUT);

    // Already pairing -- including a window the USER opened with the gesture,
    // because it is the same window. Refused rather than silently re-armed:
    // re-arming would extend a window the user physically opened.
    assert(!mgmt_pairing_start_allowed(true, true, false, &reason));
    assert(reason == MGMT_PAIRING_REASON_BUSY);

    // The reason pointer is optional.
    assert(mgmt_pairing_start_allowed(true, false, false, NULL));
}

static void test_remaining_time_is_wrap_safe(void)
{
    // Ordinary case.
    assert(mgmt_pairing_remaining_ms(30000, 5000, true) == 25000);
    // Exactly due, and past due, both saturate at zero rather than going
    // negative or wrapping to ~49 days.
    assert(mgmt_pairing_remaining_ms(5000, 5000, true) == 0);
    assert(mgmt_pairing_remaining_ms(5000, 6000, true) == 0);
    // Inactive always reports zero whatever the stale deadline says.
    assert(mgmt_pairing_remaining_ms(30000, 5000, false) == 0);

    // Across the 32-bit millisecond wrap: deadline just after the wrap, now
    // just before it. A plain `deadline > now` comparison gets this backwards.
    assert(mgmt_pairing_remaining_ms(1000u, 0xFFFFF000u, true) == 5096u);
    // And the genuinely-expired case on the other side of the wrap.
    assert(mgmt_pairing_remaining_ms(0xFFFFF000u, 1000u, true) == 0);
}

static void test_status_envelope(void)
{
    char out[MGMT_PAIRING_RESPONSE_CAPACITY];
    mgmt_pairing_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.operation = 7;
    s.state = MGMT_PAIRING_DISCOVERING;
    s.reason = MGMT_PAIRING_REASON_NONE;
    s.remaining_ms = 24000;
    s.candidates = 2;
    assert(mgmt_pairing_format_status(&s, out, sizeof(out)) > 0);
    assert(strstr(out, "\"ok\":true") != NULL);
    assert(strstr(out, "\"op\":7") != NULL);
    assert(strstr(out, "\"state\":\"discovering\"") != NULL);
    assert(strstr(out, "\"remaining_ms\":24000") != NULL);
    assert(strstr(out, "\"candidates\":2") != NULL);

    // A read that reports failure is still a successful read: `ok` describes
    // the command, so a client can tell "I could not ask" from "the answer is
    // that it did not work".
    s.state = MGMT_PAIRING_TIMED_OUT;
    s.reason = MGMT_PAIRING_REASON_NO_CONTROLLER;
    s.remaining_ms = 0;
    assert(mgmt_pairing_format_status(&s, out, sizeof(out)) > 0);
    assert(strstr(out, "\"ok\":true") != NULL);
    assert(strstr(out, "\"state\":\"timed_out\"") != NULL);
    assert(strstr(out, "\"reason\":\"no_controller\"") != NULL);

    // Only an explicit refusal is not ok.
    s.state = MGMT_PAIRING_BLOCKED;
    s.reason = MGMT_PAIRING_REASON_BUSY;
    assert(mgmt_pairing_format_status(&s, out, sizeof(out)) > 0);
    assert(strstr(out, "\"ok\":false") != NULL);
    assert(strstr(out, "\"reason\":\"busy\"") != NULL);
}

static void test_status_never_leaks_identity_or_overruns(void)
{
    // Pairing status is progress, not an inventory. No address, no key, no
    // name -- nothing that identifies a device.
    char out[MGMT_PAIRING_RESPONSE_CAPACITY];
    mgmt_pairing_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.operation = 0xFFFFFFFFu;
    s.state = MGMT_PAIRING_CONNECTING;
    s.remaining_ms = 0xFFFFFFFFu;
    s.candidates = 255;
    size_t len = mgmt_pairing_format_status(&s, out, sizeof(out));
    assert(len > 0 && len < sizeof(out));
    assert(strstr(out, "addr") == NULL);
    assert(strstr(out, "key") == NULL);
    assert(strstr(out, "name") == NULL);

    // Refuses rather than truncating into invalid JSON.
    char tiny[16];
    assert(mgmt_pairing_format_status(&s, tiny, sizeof(tiny)) == 0);
    assert(tiny[0] == '\0');
    assert(mgmt_pairing_format_status(NULL, out, sizeof(out)) == 0);
    assert(mgmt_pairing_format_status(&s, NULL, 0) == 0);
}

static void test_every_state_and_reason_has_a_stable_name(void)
{
    // The app owns human wording; these are the machine-readable values it
    // switches on, so an unnamed one would silently become "unknown" in the UI.
    const mgmt_pairing_state_t states[] = {
        MGMT_PAIRING_IDLE, MGMT_PAIRING_DISCOVERING, MGMT_PAIRING_CONNECTING,
        MGMT_PAIRING_PAIRED, MGMT_PAIRING_TIMED_OUT, MGMT_PAIRING_CANCELLED,
        MGMT_PAIRING_BLOCKED,
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        const char *name = mgmt_pairing_state_name(states[i]);
        assert(name && name[0] != '\0');
        // Distinct: two states sharing a name would be indistinguishable.
        for (size_t j = 0; j < i; ++j)
            assert(strcmp(name, mgmt_pairing_state_name(states[j])) != 0);
    }
    const mgmt_pairing_reason_t reasons[] = {
        MGMT_PAIRING_REASON_NONE, MGMT_PAIRING_REASON_NO_CONTROLLER,
        MGMT_PAIRING_REASON_MANAGEMENT_DISABLED, MGMT_PAIRING_REASON_BUSY,
        MGMT_PAIRING_REASON_LOCKED_OUT,
    };
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        const char *name = mgmt_pairing_reason_name(reasons[i]);
        assert(name && name[0] != '\0');
        for (size_t j = 0; j < i; ++j)
            assert(strcmp(name, mgmt_pairing_reason_name(reasons[j])) != 0);
    }
}

int main(void)
{
    test_command_grammar();
    test_admission_policy_and_precedence();
    test_remaining_time_is_wrap_safe();
    test_status_envelope();
    test_status_never_leaks_identity_or_overruns();
    test_every_state_and_reason_has_a_stable_name();
    printf("mgmt_pairing: all tests passed\n");
    return 0;
}
