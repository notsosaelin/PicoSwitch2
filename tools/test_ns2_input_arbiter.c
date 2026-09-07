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

// A composite logical source: two Bluetooth peers, one console owner. This is
// the Keyboard + Mouse shape; the group handle comes from ns2_kbm's role
// registry, which is the only component that knows two peers are halves of one
// controller.
static void test_composite_group_source(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;

    ns2_input_source_key_t keyboard = key(2, 0, 0xA1, 1);
    ns2_input_source_key_t mouse = key(2, 1, 0xB2, 2);
    const uint32_t group = 7u;

    // The first member takes the idle console.
    assert(ns2_input_arbiter_submit_group(&arbiter, &keyboard, "KB", 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    uint32_t keyboard_id = source_id(&arbiter, &keyboard);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == keyboard_id);

    // The second member publishes too, without a handover and without taking
    // ownership away from the first.
    assert(ns2_input_arbiter_submit_group(&arbiter, &mouse, "Mouse", 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    assert(!decision.auto_switched);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == keyboard_id);
    assert(status.source_count == 2);

    // A peer outside the group is registered but must not publish.
    ns2_input_source_key_t pad = key(2, 2, 0xC3, 3);
    assert(!ns2_input_arbiter_submit(&arbiter, &pad, "Pad", 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));

    // Losing the owning member does NOT surrender the console: the surviving
    // member of the composite inherits the token.
    bool was_active = false;
    assert(ns2_input_arbiter_disconnect(&arbiter, &keyboard, &was_active));
    assert(!was_active);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == source_id(&arbiter, &mouse));
    assert(ns2_input_arbiter_submit_group(&arbiter, &mouse, "Mouse", 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));

    // A stale disconnect for the departed peer cannot clear the replacement
    // that reused its transport index.
    ns2_input_source_key_t replacement = key(2, 0, 0xD4, 4);
    assert(ns2_input_arbiter_submit_group(&arbiter, &replacement, "KB2", 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    assert(!ns2_input_arbiter_disconnect(&arbiter, &keyboard, &was_active));
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == source_id(&arbiter, &mouse));
    assert(source_id(&arbiter, &replacement) != NS2_INPUT_SOURCE_ID_NONE);

    // Losing the last member of the composite IS whole-source loss, and the
    // caller owes the console a neutral boundary.
    assert(ns2_input_arbiter_disconnect(&arbiter, &replacement, &was_active));
    assert(!was_active);  // the mouse still owns it
    assert(ns2_input_arbiter_disconnect(&arbiter, &mouse, &was_active));
    assert(was_active);
    ns2_input_arbiter_get_status(&arbiter, &status);
    // With no explicit choice the automatic policy runs, and the only remaining
    // source is the unrelated gamepad -- which is exactly the pre-existing
    // fallback behavior, unchanged by grouping.
    assert(status.active_id == source_id(&arbiter, &pad));

    // Group 0 keeps the historical standalone behavior: two ungrouped sources
    // never share the console.
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t a = key(2, 0, 0x11, 1);
    ns2_input_source_key_t b = key(2, 1, 0x22, 2);
    assert(ns2_input_arbiter_submit_group(&arbiter, &a, "A", 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, 0u,
                                          &decision));
    assert(!ns2_input_arbiter_submit_group(&arbiter, &b, "B", 0, 0,
                                           NS2_INPUT_SOURCE_CLASS_DIRECT, 0u,
                                           &decision));
}

// An explicit user selection of one composite member still owns the console for
// the whole group, and the explicit-choice rules are unchanged.
static void test_composite_explicit_selection(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;

    ns2_input_source_key_t keyboard = key(2, 0, 0xA1, 1);
    ns2_input_source_key_t mouse = key(2, 1, 0xB2, 2);
    const uint32_t group = 3u;
    (void)ns2_input_arbiter_submit_group(&arbiter, &keyboard, "KB", 0, 0,
                                         NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                         &decision);
    (void)ns2_input_arbiter_submit_group(&arbiter, &mouse, "Mouse", 0, 0,
                                         NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                         &decision);

    // Select the mouse half explicitly; both halves keep publishing.
    uint32_t mouse_id = source_id(&arbiter, &mouse);
    assert(ns2_input_arbiter_request_active(&arbiter, mouse_id));
    assert(ns2_input_arbiter_submit_group(&arbiter, &mouse, "Mouse", 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    assert(decision.fresh_report);
    assert(ns2_input_arbiter_submit_group(&arbiter, &keyboard, "KB", 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.explicit_active);

    // Losing the explicitly selected half hands the token to the surviving
    // member rather than dropping the user's choice of logical source.
    bool was_active = false;
    assert(ns2_input_arbiter_disconnect(&arbiter, &mouse, &was_active));
    assert(!was_active);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == source_id(&arbiter, &keyboard));
    assert(status.explicit_active);
}

// The Android Controller Link arrives as an INCOMING Classic HID Device
// connection, so no inquiry record ever supplies a Bluetooth name and bthid
// leaves it empty (hardware-confirmed 2026-08-21). An empty name must not
// become "no controller" on the console slot or in the source list.
static void test_nameless_bridge_source_is_still_identified(void)
{
    const char *bridge = ns2_input_source_display_name(
        "", NS2_INPUT_SOURCE_CLASS_BRIDGE);
    assert(bridge && strcmp(bridge, "Controller Link") == 0);
    assert(strcmp(ns2_input_source_display_name(NULL,
               NS2_INPUT_SOURCE_CLASS_BRIDGE), "Controller Link") == 0);
    // Whitespace-only is as unusable as empty for a UI row.
    assert(strcmp(ns2_input_source_display_name("   ",
               NS2_INPUT_SOURCE_CLASS_BRIDGE), "Controller Link") == 0);

    // A real Bluetooth name always wins; the substitution never renames a peer.
    assert(strcmp(ns2_input_source_display_name("DualSense Edge",
               NS2_INPUT_SOURCE_CLASS_DIRECT), "DualSense Edge") == 0);
    assert(strcmp(ns2_input_source_display_name("PicoSwitch Bridge",
               NS2_INPUT_SOURCE_CLASS_BRIDGE), "PicoSwitch Bridge") == 0);

    // Only the bridge has a truthful stand-in. A nameless direct controller
    // stays nameless rather than being labelled as something it is not.
    assert(ns2_input_source_display_name("", NS2_INPUT_SOURCE_CLASS_DIRECT) == NULL);
    assert(ns2_input_source_display_name("", NS2_INPUT_SOURCE_CLASS_UNKNOWN) == NULL);
}

// Two controllers paired to the adapter at once, driven through the SAME call
// sequence the firmware actually produces per report: bthid delivers every
// input report to the raw hook (ns2_active_input_note_connection -> class
// UNKNOWN, because a connection hook cannot see what a peer is), and only then
// does the bound driver parse it into a normalized event (class DIRECT).
//
// Reported from hardware: a DualSense and an Xbox Elite paired together BOTH
// moved the console, while the companion still showed one source driving it.
// The information-free UNKNOWN registration must never overwrite a class the
// source has already established, or the owner is demoted below its rival on
// every report and ownership follows whichever controller reported last.
static void test_connection_hook_cannot_demote_a_classified_source(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t ds5 = key(2, 0, 0xC1, 21);
    ns2_input_source_key_t elite = key(2, 1, 0xC2, 22);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;

    // Both connect: the lifecycle hook registers each one before it can be
    // identified, and each one's first report classifies it.
    (void)ns2_input_arbiter_submit(&arbiter, &ds5, "DualSense", 0x054C, 0x0CE6,
                                   NS2_INPUT_SOURCE_CLASS_UNKNOWN, &decision);
    assert(ns2_input_arbiter_submit(&arbiter, &ds5, "DualSense", 0x054C, 0x0CE6,
                                    NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    uint32_t ds5_id = source_id(&arbiter, &ds5);
    (void)ns2_input_arbiter_submit(&arbiter, &elite, "Xbox Elite", 0x045E, 0x0B22,
                                   NS2_INPUT_SOURCE_CLASS_UNKNOWN, &decision);
    assert(!ns2_input_arbiter_submit(&arbiter, &elite, "Xbox Elite", 0x045E,
                                     0x0B22, NS2_INPUT_SOURCE_CLASS_DIRECT,
                                     &decision));
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == ds5_id);

    // Ordinary play: every report of either controller hits the raw hook first.
    // The owner must keep the console, and the other controller must stay shut
    // out through the whole exchange.
    for (unsigned round = 0; round < 4u; ++round) {
        (void)ns2_input_arbiter_submit(&arbiter, &ds5, NULL, 0, 0,
                                       NS2_INPUT_SOURCE_CLASS_UNKNOWN, &decision);
        assert(!decision.auto_switched);
        assert(ns2_input_arbiter_submit(&arbiter, &ds5, NULL, 0, 0,
                                        NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
        assert(decision.accepted && !decision.auto_switched);

        (void)ns2_input_arbiter_submit(&arbiter, &elite, NULL, 0, 0,
                                       NS2_INPUT_SOURCE_CLASS_UNKNOWN, &decision);
        assert(!decision.auto_switched);
        assert(!ns2_input_arbiter_submit(&arbiter, &elite, NULL, 0, 0,
                                         NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
        assert(!decision.accepted);
    }

    // The decisive case: a report the bound driver does NOT turn into an event
    // (a mode/battery/paddle report, a report id the driver skips) reaches the
    // raw hook and nothing else. The owner is then sitting on an unfollowed
    // UNKNOWN registration, and this is where the rival used to take over.
    (void)ns2_input_arbiter_submit(&arbiter, &ds5, NULL, 0, 0,
                                   NS2_INPUT_SOURCE_CLASS_UNKNOWN, &decision);
    (void)ns2_input_arbiter_submit(&arbiter, &elite, NULL, 0, 0,
                                   NS2_INPUT_SOURCE_CLASS_UNKNOWN, &decision);
    assert(!ns2_input_arbiter_submit(&arbiter, &elite, NULL, 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(!decision.accepted && !decision.auto_switched);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == ds5_id);

    // And the owner is still the owner, without a handover of its own.
    assert(ns2_input_arbiter_submit(&arbiter, &ds5, NULL, 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(decision.accepted && !decision.auto_switched && !decision.fresh_report);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.transition_count <= 2u);  // the two initial takeovers only

    // A source that genuinely changes standing is still reclassified: UNKNOWN
    // is refused because it carries no information, not because the class is
    // frozen.
    assert(!ns2_input_arbiter_submit(&arbiter, &elite, NULL, 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_BRIDGE, &decision));
    ns2_input_arbiter_get_status(&arbiter, &status);
    for (unsigned i = 0; i < status.source_count; ++i) {
        if (status.sources[i].id != ds5_id)
            assert(status.sources[i].source_class == NS2_INPUT_SOURCE_CLASS_BRIDGE);
    }
}

// Ties go to the source that registered first, and "first" has to survive a
// freed registry slot being reused. Otherwise a controller reconnecting into
// the slot its predecessor vacated silently takes the console away from the
// controller currently being played on.
static void test_equal_class_arrival_cannot_take_a_live_console(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_source_key_t first = key(2, 0, 0xD1, 31);
    ns2_input_source_key_t second = key(2, 1, 0xD2, 32);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;
    bool was_active = false;

    assert(ns2_input_arbiter_submit(&arbiter, &first, "first", 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(!ns2_input_arbiter_submit(&arbiter, &second, "second", 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    uint32_t second_id = source_id(&arbiter, &second);

    // The incumbent leaves; the survivor inherits the console by policy.
    assert(ns2_input_arbiter_disconnect(&arbiter, &first, &was_active));
    assert(was_active);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == second_id);

    // It comes back and lands in the registry slot the first one vacated. The
    // player is mid-game on the survivor: an equal-class arrival does not get
    // the console.
    ns2_input_source_key_t returning = key(2, 0, 0xD1, 33);
    assert(!ns2_input_arbiter_submit(&arbiter, &returning, "first", 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(!decision.accepted && !decision.auto_switched);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == second_id);
    assert(ns2_input_arbiter_submit(&arbiter, &second, NULL, 0, 0,
                                    NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(decision.accepted);
}

// Keyboard + Mouse is two Bluetooth peers that are ONE logical owner, so both
// of the rules the two-controller fix changed have to be checked against it.
//
// In production a composite peer only ever reaches the arbiter through
// ns2_kbm_runtime's submit, always class DIRECT and always carrying the group
// handle -- ns2_kbm_runtime_gates_connection() keeps the UNKNOWN-registering
// connection hook off any peer holding a KB/M role. This test does NOT rely on
// that gate: it drives the composite through an UNKNOWN re-registration anyway,
// so the arbiter alone still holds the composite together if the gate above it
// ever changes.
static void test_composite_survives_the_two_controller_fix(void)
{
    ns2_input_arbiter_t arbiter;
    ns2_input_arbiter_init(&arbiter);
    ns2_input_route_decision_t decision;
    ns2_input_arbiter_status_t status;
    const uint32_t group = 5u;
    ns2_input_source_key_t keyboard = key(2, 0, 0xE1, 41);
    ns2_input_source_key_t mouse = key(2, 1, 0xE2, 42);

    assert(ns2_input_arbiter_submit_group(&arbiter, &keyboard, "KB", 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    uint32_t keyboard_id = source_id(&arbiter, &keyboard);
    assert(ns2_input_arbiter_submit_group(&arbiter, &mouse, "Mouse", 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    assert(decision.accepted && !decision.auto_switched);

    // Rule 1, applied to a composite: an information-free registration of the
    // member holding the token must not demote it, must not move the token to
    // its own other half, and must not stop either half publishing. Both peers
    // keep typing and pointing across it.
    (void)ns2_input_arbiter_submit_group(&arbiter, &keyboard, NULL, 0, 0,
                                         NS2_INPUT_SOURCE_CLASS_UNKNOWN, group,
                                         &decision);
    assert(!decision.auto_switched);
    (void)ns2_input_arbiter_submit_group(&arbiter, &mouse, NULL, 0, 0,
                                         NS2_INPUT_SOURCE_CLASS_UNKNOWN, group,
                                         &decision);
    assert(!decision.auto_switched);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == keyboard_id);
    assert(ns2_input_arbiter_submit_group(&arbiter, &keyboard, NULL, 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    assert(decision.accepted && !decision.auto_switched);
    assert(ns2_input_arbiter_submit_group(&arbiter, &mouse, NULL, 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    assert(decision.accepted && !decision.auto_switched);

    // Rule 2, applied to a composite: the keyboard's battery dies, the mouse
    // inherits the token, and the keyboard comes back into the registry slot the
    // keyboard just vacated. Ties are decided by source id, not by that slot, so
    // the returning half rejoins its own composite instead of taking the token
    // mid-sentence -- and the group is unbroken, so it publishes immediately
    // with no neutral boundary and no fresh-report wait.
    bool was_active = false;
    assert(ns2_input_arbiter_disconnect(&arbiter, &keyboard, &was_active));
    assert(!was_active);  // one peer lost is not whole-source loss
    uint32_t mouse_id = source_id(&arbiter, &mouse);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == mouse_id);

    ns2_input_source_key_t returning = key(2, 0, 0xE1, 43);
    assert(ns2_input_arbiter_submit_group(&arbiter, &returning, "KB", 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    assert(decision.accepted && !decision.auto_switched && !decision.fresh_report);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == mouse_id);
    assert(status.source_count == 2);

    // An unrelated controller still cannot join or displace the composite, and
    // the composite still cannot be split by it.
    ns2_input_source_key_t pad = key(2, 2, 0xE3, 44);
    assert(!ns2_input_arbiter_submit(&arbiter, &pad, "Pad", 0, 0,
                                     NS2_INPUT_SOURCE_CLASS_DIRECT, &decision));
    assert(!decision.accepted && !decision.auto_switched);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == mouse_id);
    assert(ns2_input_arbiter_submit_group(&arbiter, &returning, NULL, 0, 0,
                                          NS2_INPUT_SOURCE_CLASS_DIRECT, group,
                                          &decision));
    assert(decision.accepted);

    // Whole-source loss is still whole-source loss: both halves gone means the
    // caller owes the console a neutral boundary and the policy falls back.
    assert(ns2_input_arbiter_disconnect(&arbiter, &returning, &was_active));
    assert(!was_active);
    assert(ns2_input_arbiter_disconnect(&arbiter, &mouse, &was_active));
    assert(was_active);
    ns2_input_arbiter_get_status(&arbiter, &status);
    assert(status.active_id == source_id(&arbiter, &pad));
}

int main(void)
{
    test_nameless_bridge_source_is_still_identified();
    test_connection_hook_cannot_demote_a_classified_source();
    test_equal_class_arrival_cannot_take_a_live_console();
    test_composite_survives_the_two_controller_fix();
    test_composite_group_source();
    test_composite_explicit_selection();
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
