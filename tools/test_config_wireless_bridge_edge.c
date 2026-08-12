/*
 * Adversarial edge-case coverage for the PRODUCTION wireless command bridge
 * (src/config_wireless_bridge.c). Complements tools/test_config_wireless_bridge.c
 * (the happy path). These exercise the weird/hostile inputs a buggy or malicious
 * BLE/Web-Bluetooth client could send, and pin the bridge's actual behavior as a
 * regression guard. Where the behavior is surprising-but-safe it is noted.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude -Itools \
 *   tools/test_config_wireless_bridge_edge.c src/config_wireless_bridge.c \
 *   -o build/host-tests/test_config_wireless_bridge_edge.exe
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config_wireless_bridge.h"

#define RXOK   CONFIG_WIRELESS_RX_OK
#define RXCMD  CONFIG_WIRELESS_RX_COMMAND_READY
#define RXBUSY CONFIG_WIRELESS_RX_BUSY
#define RXLONG CONFIG_WIRELESS_RX_TOO_LONG

static config_wireless_rx_result_t rx(const void *p, size_t n) {
    return config_wireless_bridge_receive((const uint8_t *)p, n);
}
static bool take(char *buf, size_t cap, uint32_t *sess) {
    return config_wireless_bridge_take_command(buf, cap, sess);
}

// A NUL embedded in the stream is stored verbatim, but every downstream consumer
// (handle_line's strcmp/strncmp, the allowlist) treats the command as a C string
// and therefore sees it truncated at the NUL. Safe: it cannot smuggle a second
// allowlisted command past the check -- "imu\0amiibo status" reads as "imu".
static void test_embedded_nul_truncates_safely(void) {
    config_wireless_bridge_init();
    assert(rx("imu\0amiibo status\n", 18) == RXCMD);
    char cmd[CONFIG_WIRELESS_COMMAND_CAPACITY]; uint32_t s;
    assert(take(cmd, sizeof(cmd), &s));
    assert(strcmp(cmd, "imu") == 0);                 // consumer view truncates at NUL
    // The allowlist (the real security gate) sees the truncated string:
    assert(!config_wireless_command_allowed(cmd));   // "imu" is denied -> no bypass
}

static void test_carriage_returns_and_blank_lines(void) {
    config_wireless_bridge_init();
    assert(rx("\r\r\r", 3) == RXOK);                 // bare CRs: nothing happens
    assert(rx("\n\n\n", 3) == RXOK);                 // blank lines: nothing happens
    assert(rx("ping\r\n", 6) == RXCMD);              // CRLF terminates one command
    char cmd[CONFIG_WIRELESS_COMMAND_CAPACITY]; uint32_t s;
    assert(take(cmd, sizeof(cmd), &s) && strcmp(cmd, "ping") == 0);
}

static void test_capacity_boundary_exact(void) {
    // 127 payload chars (CAPACITY-1) + '\n' is the largest command that fits.
    char big[CONFIG_WIRELESS_COMMAND_CAPACITY + 4];
    memset(big, 'a', CONFIG_WIRELESS_COMMAND_CAPACITY - 1);
    big[CONFIG_WIRELESS_COMMAND_CAPACITY - 1] = '\n';
    config_wireless_bridge_init();
    assert(rx(big, CONFIG_WIRELESS_COMMAND_CAPACITY) == RXCMD);
    char cmd[CONFIG_WIRELESS_COMMAND_CAPACITY]; uint32_t s;
    assert(take(cmd, sizeof(cmd), &s));
    assert(strlen(cmd) == CONFIG_WIRELESS_COMMAND_CAPACITY - 1u);

    // One more payload char (128) overflows and is rejected; the bridge then
    // recovers on the next line.
    memset(big, 'b', CONFIG_WIRELESS_COMMAND_CAPACITY);
    big[CONFIG_WIRELESS_COMMAND_CAPACITY] = '\n';
    config_wireless_bridge_init();
    assert(rx(big, CONFIG_WIRELESS_COMMAND_CAPACITY + 1) == RXLONG);
    assert(rx("ping\n", 5) == RXCMD);                // recovered
}

// Two commands in a single receive: only the first is captured (single slot);
// the second arrives while the slot is full and is dropped with RX_BUSY. The
// production caller drains one command per core0 pass and the browser waits for
// each reply, so this back-pressure is by design -- but pin it so nobody assumes
// batching works.
static void test_second_command_in_one_frame_is_dropped(void) {
    config_wireless_bridge_init();
    assert(rx("aa\nbb\n", 6) == RXBUSY);
    char cmd[CONFIG_WIRELESS_COMMAND_CAPACITY]; uint32_t s;
    assert(take(cmd, sizeof(cmd), &s) && strcmp(cmd, "aa") == 0);
    assert(!take(cmd, sizeof(cmd), &s));             // "bb" was dropped
}

// If the consumer offers a buffer smaller than the pending command, the command
// is DROPPED (slot cleared) rather than truncated. Production always passes a
// full-capacity buffer so it never hits this, but the contract is pinned: a
// too-small buffer loses the command, it does not overflow.
static void test_take_into_too_small_buffer_drops(void) {
    config_wireless_bridge_init();
    assert(rx("ping\n", 5) == RXCMD);
    char small[3]; uint32_t s;
    assert(!take(small, sizeof(small), &s));         // rejected, no overflow
    char full[CONFIG_WIRELESS_COMMAND_CAPACITY];
    assert(!take(full, sizeof(full), &s));           // and the command is gone
}

static void test_response_edges(void) {
    config_wireless_bridge_init();
    assert(rx("ping\n", 5) == RXCMD);
    char cmd[CONFIG_WIRELESS_COMMAND_CAPACITY]; uint32_t s;
    assert(take(cmd, sizeof(cmd), &s));

    // Empty response still yields a single '\n' terminator on the wire.
    assert(config_wireless_bridge_publish_response(s, ""));
    uint8_t b[8];
    assert(config_wireless_bridge_peek_response(b, sizeof(b)) == 1 && b[0] == '\n');
    // A second publish while one is pending is refused.
    assert(!config_wireless_bridge_publish_response(s, "x"));
    // peek with zero capacity returns nothing; consume overrun clamps to remaining.
    assert(config_wireless_bridge_peek_response(b, 0) == 0);
    config_wireless_bridge_consume_response(999);
    assert(!config_wireless_bridge_response_pending());
}

static void test_response_capacity_boundary(void) {
    config_wireless_bridge_init();
    assert(rx("get\n", 4) == RXCMD);
    char cmd[CONFIG_WIRELESS_COMMAND_CAPACITY]; uint32_t s;
    assert(take(cmd, sizeof(cmd), &s));

    // 511 chars + '\n' == 512 == capacity: the largest response that fits.
    char resp[CONFIG_WIRELESS_RESPONSE_CAPACITY];
    memset(resp, 'r', CONFIG_WIRELESS_RESPONSE_CAPACITY - 1);
    resp[CONFIG_WIRELESS_RESPONSE_CAPACITY - 1] = '\0';
    assert(config_wireless_bridge_publish_response(s, resp));
    // Drain it fully in MTU-sized chunks.
    uint8_t chunk[20]; size_t total = 0, got;
    while ((got = config_wireless_bridge_peek_response(chunk, sizeof(chunk))) > 0) {
        total += got;
        config_wireless_bridge_consume_response(got);
    }
    assert(total == CONFIG_WIRELESS_RESPONSE_CAPACITY);   // 511 + '\n'
    assert(!config_wireless_bridge_response_pending());

    // 512 chars would need 513 with the terminator: rejected.
    config_wireless_bridge_init();
    assert(rx("get\n", 4) == RXCMD);
    assert(take(cmd, sizeof(cmd), &s));
    char toobig[CONFIG_WIRELESS_RESPONSE_CAPACITY + 1];
    memset(toobig, 'r', CONFIG_WIRELESS_RESPONSE_CAPACITY);
    toobig[CONFIG_WIRELESS_RESPONSE_CAPACITY] = '\0';
    assert(!config_wireless_bridge_publish_response(s, toobig));
}

// A pending (un-drained) response blocks the next command: the client must read
// its reply before the bridge accepts another line. Prevents a fast client from
// overwriting a reply the slow BLE notify path has not sent yet.
static void test_pending_response_blocks_new_command(void) {
    config_wireless_bridge_init();
    assert(rx("ping\n", 5) == RXCMD);
    char cmd[CONFIG_WIRELESS_COMMAND_CAPACITY]; uint32_t s;
    assert(take(cmd, sizeof(cmd), &s));
    assert(config_wireless_bridge_publish_response(s, "{\"ok\":true}"));
    assert(rx("get\n", 4) == RXBUSY);                // blocked until reply drained
    uint8_t b[64];
    size_t got = config_wireless_bridge_peek_response(b, sizeof(b));
    config_wireless_bridge_consume_response(got);
    assert(rx("get\n", 4) == RXCMD);                 // now accepted
}

static void test_null_data_guard(void) {
    config_wireless_bridge_init();
    assert(rx(NULL, 0) == RXOK);                     // benign
    assert(rx(NULL, 5) == RXLONG);                   // defended, no deref
}

int main(void) {
    test_embedded_nul_truncates_safely();
    test_carriage_returns_and_blank_lines();
    test_capacity_boundary_exact();
    test_second_command_in_one_frame_is_dropped();
    test_take_into_too_small_buffer_drops();
    test_response_edges();
    test_response_capacity_boundary();
    test_pending_response_blocks_new_command();
    test_null_data_guard();
    puts("config wireless bridge edge tests passed");
    return 0;
}
