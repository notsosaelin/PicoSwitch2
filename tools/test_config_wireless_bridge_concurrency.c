/*
 * Concurrency stress test for the cross-core wireless bridge. In firmware core1
 * (BLE) is the sole producer (config_wireless_bridge_receive) and core0 is the
 * sole consumer (take_command / publish_response / drain). This drives that same
 * SPSC handshake from two host threads to shake out logic races: lost,
 * duplicated, re-ordered, or torn commands.
 *
 * NOTE: the host x86 memory model is TSO, so this proves the handshake LOGIC,
 * not ARM memory ordering -- that rests on the bridge's acquire/release atomics
 * (correct by construction) plus on-hardware validation. Logic-race coverage is
 * the point here.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -pthread -Isrc -Iinclude -Itools \
 *   tools/test_config_wireless_bridge_concurrency.c src/config_wireless_bridge.c \
 *   -o build/host-tests/test_config_wireless_bridge_concurrency.exe
 */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_wireless_bridge.h"

#define N 20000

static void *producer(void *arg) {
    (void)arg;
    for (int i = 0; i < N; ++i) {
        char framed[32];
        int n = snprintf(framed, sizeof(framed), "c%d\n", i);
        // Resend the SAME command until it is accepted (a BUSY means the consumer
        // has not drained the previous turn yet) -- exactly what core1 must do.
        for (;;) {
            config_wireless_rx_result_t r =
                config_wireless_bridge_receive((const uint8_t *)framed, (size_t)n);
            if (r == CONFIG_WIRELESS_RX_COMMAND_READY) break;
            sched_yield();
        }
    }
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    for (int expected = 0; expected < N; ) {
        char cmd[CONFIG_WIRELESS_COMMAND_CAPACITY];
        uint32_t sess;
        if (!config_wireless_bridge_take_command(cmd, sizeof(cmd), &sess)) {
            sched_yield();
            continue;
        }
        // In-order, no loss, no duplication, not torn.
        char want[32];
        snprintf(want, sizeof(want), "c%d", expected);
        assert(strcmp(cmd, want) == 0);

        assert(config_wireless_bridge_publish_response(sess, "{\"ok\":true}"));
        uint8_t chunk[20]; size_t got;
        while ((got = config_wireless_bridge_peek_response(chunk, sizeof(chunk))) > 0)
            config_wireless_bridge_consume_response(got);
        assert(!config_wireless_bridge_response_pending());
        expected++;
    }
    return NULL;
}

int main(void) {
    config_wireless_bridge_init();
    pthread_t p, c;
    // Start the consumer first so the producer never spins unboundedly.
    assert(pthread_create(&c, NULL, consumer, NULL) == 0);
    assert(pthread_create(&p, NULL, producer, NULL) == 0);
    assert(pthread_join(p, NULL) == 0);
    assert(pthread_join(c, NULL) == 0);
    printf("concurrency: %d commands delivered in order, no loss/dup/tear\n", N);
    puts("config wireless bridge concurrency test passed");
    return 0;
}
