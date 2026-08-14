// Deterministic source-ownership tests.  This is a host-only test: it does
// not require Pico SDK, BTstack, a console, or a connected controller.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ns2_input_arbiter.h"

static ns2_input_source_key_t key(uint8_t transport, uint8_t conn,
                                  uint8_t address, uint32_t lifecycle)
{
    ns2_input_source_key_t result;
    memset(&result, 0, sizeof(result));
    result.transport = transport;
    result.dev_addr = conn;
    result.instance = 0;
    result.stable_addr_valid = 1;
    result.stable_addr[5] = address;
    result.connection_generation = lifecycle;
    return result;
}

static uint32_t source_id(const ns2_input_arbiter_t *arbiter,
                          const ns2_input_source_key_t *source_key)
{
    return ns2_input_arbiter_source_id(arbiter, source_key);
}

static void test_interleaving_and_legacy_default(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t xbox = key(2, 0, 0x10, 1);
    ns2_input_source_key_t android = key(2, 1, 0x20, 2);
    ns2_input_route_decision_t decision;

    assert(ns2_input_arbiter_submit(&arbiter, &xbox, "Xbox", 0x045E, 0x0B13,
                                    &decision));
    assert(decision.accepted && !decision.transition_applied);
    uint32_t xbox_id = source_id(&arbiter, &xbox);
    assert(xbox_id != NS2_INPUT_SOURCE_ID_NONE);

    // A second source is visible but cannot merge into the console stream.
    assert(!ns2_input_arbiter_submit(&arbiter, &android, "Android", 0, 0,
                                      &decision));
    assert(!decision.accepted);
    ns2_input_arbiter_status_t status;
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == xbox_id);
    assert(status.source_count == 2);
    assert(strcmp(status.sources[0].name, "Xbox") == 0);
}

static void test_atomic_selection_and_fresh_gate(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t first = key(2, 0, 0x31, 3);
    ns2_input_source_key_t second = key(2, 1, 0x32, 4);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;
    assert(ns2_input_arbiter_submit(&arbiter, &first, "first", 0, 0, &decision));
    assert(!ns2_input_arbiter_submit(&arbiter, &second, "second", 0, 0, &decision));
    uint32_t second_id = source_id(&arbiter, &second);
    assert(ns2_input_arbiter_request_active(&arbiter, second_id));

    // The first old-source report applies the pending transaction but is not
    // allowed through.  The caller emits its neutral boundary at this edge.
    assert(!ns2_input_arbiter_submit(&arbiter, &first, NULL, 0, 0, &decision));
    assert(decision.transition_applied && !decision.accepted);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == second_id && status.awaiting_fresh);
    assert(status.explicit_active);
    // The request has been applied, so no switch is outstanding any more. A stale
    // pending target here is what left a streaming controller permanently labelled
    // "switching" in the companion, and it also defeats the caller-side idempotence
    // fast path, which re-neutralizes the console slot on every repeat selection.
    assert(status.pending_id == NS2_INPUT_SOURCE_ID_NONE);

    // Exactly the first complete report from the selected source is accepted
    // as the fresh post-neutral state.
    assert(ns2_input_arbiter_submit(&arbiter, &second, NULL, 0, 0, &decision));
    assert(decision.accepted && decision.fresh_report);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(!status.awaiting_fresh);

    // Repeating the same selection is idempotent and does not create another
    // neutral boundary.
    uint32_t transitions = status.transition_count;
    assert(ns2_input_arbiter_request_active(&arbiter, second_id));
    assert(ns2_input_arbiter_submit(&arbiter, &second, NULL, 0, 0, &decision));
    assert(decision.accepted && !decision.transition_applied);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.transition_count == transitions);
    assert(status.pending_id == NS2_INPUT_SOURCE_ID_NONE);

    // While a switch to a different source really is outstanding, the pending
    // target must still be reported: that is what the UI renders as "switching",
    // and what makes a pending source's disconnect count as an ownership loss.
    uint32_t first_id = source_id(&arbiter, &first);
    assert(ns2_input_arbiter_request_active(&arbiter, first_id));
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.pending_id == first_id);
    assert(status.active_id == second_id);
}

static void test_disconnect_index_reuse_and_no_fallback(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t old_source = key(2, 0, 0x41, 5);
    ns2_input_source_key_t other = key(2, 1, 0x42, 6);
    ns2_input_source_key_t reused = key(2, 0, 0x43, 7);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;
    bool was_active;

    assert(ns2_input_arbiter_submit(&arbiter, &old_source, "old", 0, 0,
                                    &decision));
    assert(ns2_input_arbiter_submit(&arbiter, &other, "other", 0, 0,
                                    &decision) == false);
    uint32_t other_id = source_id(&arbiter, &other);
    assert(ns2_input_arbiter_request_active(&arbiter, other_id));
    assert(ns2_input_arbiter_submit(&arbiter, &other, NULL, 0, 0, &decision));
    assert(decision.accepted);

    assert(ns2_input_arbiter_disconnect(&arbiter, &other, &was_active));
    assert(was_active);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == NS2_INPUT_SOURCE_ID_NONE);
    assert(status.explicit_active);

    // The old source is inactive and cannot silently become active after the
    // selected source disconnects.
    assert(!ns2_input_arbiter_submit(&arbiter, &old_source, NULL, 0, 0,
                                     &decision));
    assert(ns2_input_arbiter_disconnect(&arbiter, &old_source, &was_active));
    assert(!was_active);

    // A recycled connection index is a new source, and an old disconnect key
    // cannot remove it because stable address + lifecycle differ.
    assert(!ns2_input_arbiter_submit(&arbiter, &reused, "reused", 0, 0,
                                     &decision));
    uint32_t reused_id = source_id(&arbiter, &reused);
    assert(reused_id != NS2_INPUT_SOURCE_ID_NONE && reused_id != source_id(&arbiter, &old_source));
    assert(!ns2_input_arbiter_disconnect(&arbiter, &old_source, &was_active));
    assert(ns2_input_arbiter_request_active(&arbiter, reused_id));
    assert(ns2_input_arbiter_submit(&arbiter, &reused, NULL, 0, 0, &decision));
    assert(decision.accepted && decision.fresh_report);
}

static void test_none_selection_and_source_metadata(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t source = key(3, 4, 0x51, 8);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;
    assert(ns2_input_arbiter_submit(&arbiter, &source, "one", 1, 2, &decision));
    uint32_t id = source_id(&arbiter, &source);
    assert(ns2_input_arbiter_request_active(&arbiter, 0));
    assert(!ns2_input_arbiter_submit(&arbiter, &source, NULL, 0, 0, &decision));
    assert(decision.transition_applied);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == 0 && status.explicit_active);
    assert(status.source_count == 1 && status.sources[0].id == id);
}

static void test_pending_target_disconnect_stays_neutral(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t first = key(2, 0, 0x61, 9);
    ns2_input_source_key_t second = key(2, 1, 0x62, 10);
    ns2_input_route_decision_t decision;
    bool was_active = false;
    assert(ns2_input_arbiter_submit(&arbiter, &first, "first", 0, 0, &decision));
    assert(!ns2_input_arbiter_submit(&arbiter, &second, "second", 0, 0, &decision));
    uint32_t second_id = source_id(&arbiter, &second);
    assert(ns2_input_arbiter_request_active(&arbiter, second_id));
    assert(ns2_input_arbiter_disconnect(&arbiter, &second, &was_active));
    assert(was_active);
    assert(!ns2_input_arbiter_submit(&arbiter, &first, NULL, 0, 0, &decision));
    ns2_input_arbiter_status_t status;
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == NS2_INPUT_SOURCE_ID_NONE);
    assert(status.explicit_active);
}

int main(void)
{
    test_interleaving_and_legacy_default();
    test_atomic_selection_and_fresh_gate();
    test_disconnect_index_reuse_and_no_fallback();
    test_none_selection_and_source_metadata();
    test_pending_target_disconnect_stays_neutral();
    puts("ns2 input arbiter tests passed");
    return 0;
}
