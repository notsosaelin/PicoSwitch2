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
}

int main(void)
{
    test_priority_and_connection_truth();
    test_wall_clock_patterns();
    puts("Owner LED policy tests passed");
    return 0;
}
