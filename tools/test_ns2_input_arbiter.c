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
                                    NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(decision.accepted && !decision.transition_applied);
    uint32_t xbox_id = source_id(&arbiter, &xbox);
    assert(xbox_id != NS2_INPUT_SOURCE_ID_NONE);

    // A second source is visible but cannot merge into the console stream.
    assert(!ns2_input_arbiter_submit(&arbiter, &android, "Android", 0, 0,
                                      NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
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
    assert(ns2_input_arbiter_submit(&arbiter, &first, "first", 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(!ns2_input_arbiter_submit(&arbiter, &second, "second", 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    uint32_t second_id = source_id(&arbiter, &second);
    assert(ns2_input_arbiter_request_active(&arbiter, second_id));

    // The first old-source report applies the pending transaction but is not
    // allowed through.  The caller emits its neutral boundary at this edge.
    assert(!ns2_input_arbiter_submit(&arbiter, &first, NULL, 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
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
    assert(ns2_input_arbiter_submit(&arbiter, &second, NULL, 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(decision.accepted && decision.fresh_report);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(!status.awaiting_fresh);

    // Repeating the same selection is idempotent and does not create another
    // neutral boundary.
    uint32_t transitions = status.transition_count;
    assert(ns2_input_arbiter_request_active(&arbiter, second_id));
    assert(ns2_input_arbiter_submit(&arbiter, &second, NULL, 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
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
                                    NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(ns2_input_arbiter_submit(&arbiter, &other, "other", 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_DIRECT, &decision) == false);
    uint32_t other_id = source_id(&arbiter, &other);
    assert(ns2_input_arbiter_request_active(&arbiter, other_id));
    assert(ns2_input_arbiter_submit(&arbiter, &other, NULL, 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(decision.accepted);

    assert(ns2_input_arbiter_disconnect(&arbiter, &other, &was_active));
    assert(was_active);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == NS2_INPUT_SOURCE_ID_NONE);
    assert(status.explicit_active);

    // The old source is inactive and cannot silently become active after the
    // selected source disconnects.
    assert(!ns2_input_arbiter_submit(&arbiter, &old_source, NULL, 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(ns2_input_arbiter_disconnect(&arbiter, &old_source, &was_active));
    assert(!was_active);

    // A recycled connection index is a new source, and an old disconnect key
    // cannot remove it because stable address + lifecycle differ.
    assert(!ns2_input_arbiter_submit(&arbiter, &reused, "reused", 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    uint32_t reused_id = source_id(&arbiter, &reused);
    assert(reused_id != NS2_INPUT_SOURCE_ID_NONE && reused_id != source_id(&arbiter, &old_source));
    assert(!ns2_input_arbiter_disconnect(&arbiter, &old_source, &was_active));
    assert(ns2_input_arbiter_request_active(&arbiter, reused_id));
    assert(ns2_input_arbiter_submit(&arbiter, &reused, NULL, 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(decision.accepted && decision.fresh_report);
}

static void test_none_selection_and_source_metadata(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t source = key(3, 4, 0x51, 8);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;
    assert(ns2_input_arbiter_submit(&arbiter, &source, "one", 1, 2, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    uint32_t id = source_id(&arbiter, &source);
    assert(ns2_input_arbiter_request_active(&arbiter, 0));
    assert(!ns2_input_arbiter_submit(&arbiter, &source, NULL, 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
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
    assert(ns2_input_arbiter_submit(&arbiter, &first, "first", 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(!ns2_input_arbiter_submit(&arbiter, &second, "second", 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    uint32_t second_id = source_id(&arbiter, &second);
    assert(ns2_input_arbiter_request_active(&arbiter, second_id));
    assert(ns2_input_arbiter_disconnect(&arbiter, &second, &was_active));
    assert(was_active);
    assert(!ns2_input_arbiter_submit(&arbiter, &first, NULL, 0, 0, NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    ns2_input_arbiter_status_t status;
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == NS2_INPUT_SOURCE_ID_NONE);
    assert(status.explicit_active);
}

// Default ownership policy: with no explicit user choice, a controller paired
// directly to the adapter outranks the companion app's bridge, and the bridge
// takes the console whenever nothing else is connected.
static void test_bridge_defaults_and_yields_to_direct(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t bridge = key(2, 0, 0x61, 9);
    ns2_input_source_key_t pad = key(2, 1, 0x62, 10);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;
    bool was_active = false;

    // Nothing else is connected, so the companion bridge simply plays.
    assert(ns2_input_arbiter_submit(&arbiter, &bridge, "Handheld", 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_BRIDGE, &decision));
    assert(decision.accepted && !decision.auto_switched);
    uint32_t bridge_id = source_id(&arbiter, &bridge);

    // A directly paired controller arrives and takes the console automatically.
    // The user chose nothing, so this must not count as an explicit selection.
    // The report that registers it is by definition its first, so it is both the
    // takeover and the fresh post-neutral state the switch requires.
    assert(ns2_input_arbiter_submit(&arbiter, &pad, "Pad", 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    uint32_t pad_id = source_id(&arbiter, &pad);
    assert(decision.auto_switched && decision.accepted && decision.fresh_report);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == pad_id);
    assert(!status.explicit_active);
    assert(!status.awaiting_fresh);

    // The displaced bridge is now inactive and may not publish.
    assert(!ns2_input_arbiter_submit(&arbiter, &bridge, NULL, 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_BRIDGE, &decision));
    assert(ns2_input_arbiter_submit(&arbiter, &pad, NULL, 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(decision.accepted && !decision.auto_switched);

    // The controller disconnects: the console falls back to the bridge rather
    // than going dead, because the user never made a choice to honour.
    assert(ns2_input_arbiter_disconnect(&arbiter, &pad, &was_active));
    assert(was_active);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == bridge_id);
    assert(!status.explicit_active);
    // Nothing else owns the console at this point, so the bridge resumes without
    // having to wait for a fresh report.
    assert(!status.awaiting_fresh);
    assert(ns2_input_arbiter_submit(&arbiter, &bridge, NULL, 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_BRIDGE, &decision));
    assert(decision.accepted);
}

// An explicit choice outranks the policy in both directions.
static void test_explicit_choice_is_never_overridden(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t bridge = key(2, 0, 0x71, 11);
    ns2_input_source_key_t pad = key(2, 1, 0x72, 12);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;

    assert(ns2_input_arbiter_submit(&arbiter, &bridge, "Handheld", 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_BRIDGE, &decision));
    uint32_t bridge_id = source_id(&arbiter, &bridge);

    // The user deliberately keeps the handheld.
    assert(ns2_input_arbiter_request_active(&arbiter, bridge_id));
    assert(ns2_input_arbiter_submit(&arbiter, &bridge, NULL, 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_BRIDGE, &decision));
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.explicit_active && status.active_id == bridge_id);

    // A directly paired controller now arrives. Class preference must NOT steal
    // the console, because the user already said what they wanted.
    assert(!ns2_input_arbiter_submit(&arbiter, &pad, "Pad", 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(!decision.auto_switched);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == bridge_id);
}

// A connection hook registers a source before any report, so it cannot know
// whether the source is the companion bridge. The provisional class must not let
// it outrank an identified source, and identifying it later must re-evaluate
// ownership rather than leaving the console with whoever got there first.
static void test_unknown_source_is_reclassified_by_its_first_report(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t bridge = key(2, 0, 0x81, 13);
    ns2_input_source_key_t pad = key(2, 1, 0x82, 14);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;

    // The bridge identifies itself and owns an otherwise idle console.
    assert(ns2_input_arbiter_submit(&arbiter, &bridge, "Handheld", 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_BRIDGE, &decision));
    uint32_t bridge_id = source_id(&arbiter, &bridge);

    // A controller connects but has not reported yet. It must NOT take the
    // console while its class is still unknown.
    assert(!ns2_input_arbiter_submit(&arbiter, &pad, "Pad", 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_UNKNOWN, &decision));
    assert(!decision.auto_switched);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == bridge_id);

    // Its first report identifies it as directly paired, and only now does it
    // take ownership.
    assert(ns2_input_arbiter_submit(&arbiter, &pad, "Pad", 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(decision.auto_switched);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == source_id(&arbiter, &pad));
    assert(!status.explicit_active);
}

int main(void)
{
    test_bridge_defaults_and_yields_to_direct();
    test_unknown_source_is_reclassified_by_its_first_report();
    test_explicit_choice_is_never_overridden();
    test_interleaving_and_legacy_default();
    test_atomic_selection_and_fresh_gate();
    test_disconnect_index_reuse_and_no_fallback();
    test_none_selection_and_source_metadata();
    test_pending_target_disconnect_stays_neutral();
    puts("ns2 input arbiter tests passed");
    return 0;
}
