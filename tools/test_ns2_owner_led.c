#include <assert.h>
#include <stdio.h>

#include "ns2_owner_led.h"

static void test_priority_and_connection_truth(void)
{
    ns2_owner_led_inputs_t in = {0};
    assert(ns2_owner_led_decide(in) == NS2_OWNER_LED_IDLE);
    in.controller_ready = true;
    assert(ns2_owner_led_decide(in) == NS2_OWNER_LED_CONNECTED);
    in.pairing_active = true;
    assert(ns2_owner_led_decide(in) == NS2_OWNER_LED_PAIRING);
    in.wipe_active = true;
    assert(ns2_owner_led_decide(in) == NS2_OWNER_LED_WIPE);
    in.config_mode = true;
    assert(ns2_owner_led_decide(in) == NS2_OWNER_LED_CONFIG);
    in.gc_diag = true;
    assert(ns2_owner_led_decide(in) == NS2_OWNER_LED_GC_DIAG);
    in.mode_ack = true;
    assert(ns2_owner_led_decide(in) == NS2_OWNER_LED_MODE_ACK);

    // A raw ACL/ATT slot is intentionally absent from the inputs: only an
    // authoritative HID-ready controller can select solid connected.
    in = (ns2_owner_led_inputs_t){0};
    assert(ns2_owner_led_decide(in) != NS2_OWNER_LED_CONNECTED);
}

static void test_wall_clock_patterns(void)
{
    assert(ns2_owner_led_render(NS2_OWNER_LED_CONNECTED, 999999u, 0, 0, 0));
    assert(ns2_owner_led_render(NS2_OWNER_LED_IDLE, 0u, 0, 0, 0));
    assert(!ns2_owner_led_render(NS2_OWNER_LED_IDLE, 90u, 0, 0, 0));
    assert(!ns2_owner_led_render(NS2_OWNER_LED_IDLE, 9999u, 0, 0, 0));
    assert(ns2_owner_led_render(NS2_OWNER_LED_IDLE, 10000u, 0, 0, 0));

    assert(ns2_owner_led_render(NS2_OWNER_LED_PAIRING, 0u, 0, 0, 0));
    assert(!ns2_owner_led_render(NS2_OWNER_LED_PAIRING, 120u, 0, 0, 0));
    assert(ns2_owner_led_render(NS2_OWNER_LED_PAIRING, 240u, 0, 0, 0));

    assert(ns2_owner_led_render(NS2_OWNER_LED_MODE_ACK, 0u, 2, 0, 0));
    assert(!ns2_owner_led_render(NS2_OWNER_LED_MODE_ACK, 150u, 2, 0, 0));
    assert(ns2_owner_led_render(NS2_OWNER_LED_MODE_ACK, 300u, 2, 0, 0));
    assert(!ns2_owner_led_render(NS2_OWNER_LED_MODE_ACK, 600u, 2, 0, 0));

    assert(ns2_owner_led_render(NS2_OWNER_LED_CONFIG, 0u, 0, 0, 0));
    assert(!ns2_owner_led_render(NS2_OWNER_LED_CONFIG, 500u, 0, 0, 0));
    assert(ns2_owner_led_render(NS2_OWNER_LED_WIPE, 0u, 0, 0, 0));
    assert(!ns2_owner_led_render(NS2_OWNER_LED_WIPE, 60u, 0, 0, 0));
    assert(ns2_owner_led_render(NS2_OWNER_LED_GC_DIAG, 0u, 0, 255u, 0));
    assert(ns2_owner_led_render(NS2_OWNER_LED_GC_DIAG, 0u, 0, 0u, 0));
    assert(!ns2_owner_led_render(NS2_OWNER_LED_GC_DIAG, 750u, 0, 0u, 0));
    assert(ns2_owner_led_render(NS2_OWNER_LED_GC_DIAG, 0u, 0, 2u, 0));
    assert(!ns2_owner_led_render(NS2_OWNER_LED_GC_DIAG, 300u, 0, 2u, 0));
    assert(ns2_owner_led_render(NS2_OWNER_LED_GC_DIAG, 0u, 0, 21u, 42u));
}

static void test_output_transition_timestamp(void)
{
    ns2_owner_led_output_state_t state = {0};

    ns2_owner_led_track_output(&state, false, 10u);
    assert(!state.output_on);
    assert(state.last_transition_ms == 0u);

    ns2_owner_led_track_output(&state, true, 20u);
    assert(state.output_on);
    assert(state.last_transition_ms == 20u);

    ns2_owner_led_track_output(&state, true, 30u);
    assert(state.last_transition_ms == 20u);

    ns2_owner_led_track_output(&state, false, 40u);
    assert(!state.output_on);
    assert(state.last_transition_ms == 40u);
}

int main(void)
{
    test_priority_and_connection_truth();
    test_wall_clock_patterns();
    test_output_transition_timestamp();
    puts("Owner LED policy tests passed");
    return 0;
}
