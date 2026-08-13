/*
 * Integration/scenario coverage for an in-band management SESSION: composes the
 * REAL wireless bridge (src/config_wireless_bridge.c) and the REAL wireless
 * allowlist with the access gates, walking through realistic (and hostile)
 * command sequences a phone/portal would drive. Proves the pieces work together
 * in order -- not just as isolated units.
 *
 * The dispatch gate below mirrors the mgmt_allow_write contract from
 * tools/test_mgmt_access.c (kept inline so this test stays self-contained):
 * a command is dispatched only when the feature is enabled, a client is
 * connected AND bonded, and the command is allowlisted.
 *
 * gcc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude -Itools \
 *   tools/test_mgmt_session.c src/config_wireless_bridge.c \
 *   -o build/host-tests/test_mgmt_session.exe
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config_wireless_bridge.h"

typedef struct { bool enabled, connected, bonded; } session_t;

// Would the firmware dispatch this command to handle_line, or reject it before?
static bool dispatch_allowed(const session_t *s, const char *cmd) {
    return s->enabled && s->connected && s->bonded &&
           config_wireless_command_allowed(cmd);
}

// Simulate one full request/response turn over the bridge and return the reply
// the client would observe (reassembled from MTU chunks). A rejected command
// still gets the firmware's JSON error line, exactly like handle_line's
// "command unavailable over Bluetooth".
static bool run_turn(const session_t *s, const char *line, char *reply, size_t cap) {
    char framed[160];
    int n = snprintf(framed, sizeof(framed), "%s\n", line);
    if (config_wireless_bridge_receive((const uint8_t *)framed, (size_t)n) !=
        CONFIG_WIRELESS_RX_COMMAND_READY) {
        return false;
    }
    char cmd[CONFIG_WIRELESS_COMMAND_CAPACITY]; uint32_t sess;
    if (!config_wireless_bridge_take_command(cmd, sizeof(cmd), &sess)) return false;

    const char *response = dispatch_allowed(s, cmd)
        ? "{\"ok\":true}"
        : "{\"error\":\"command unavailable over Bluetooth\"}";
    assert(config_wireless_bridge_publish_response(sess, response));

    // Drain in 20-byte MTU-safe chunks, as the real notify path does.
    size_t len = 0; uint8_t chunk[20]; size_t got;
    while ((got = config_wireless_bridge_peek_response(chunk, sizeof(chunk))) > 0) {
        assert(len + got < cap);
        memcpy(reply + len, chunk, got);
        len += got;
        config_wireless_bridge_consume_response(got);
    }
    reply[len] = '\0';
    assert(!config_wireless_bridge_response_pending());
    return true;
}

static void test_bonded_user_command_succeeds(void) {
    config_wireless_bridge_init();
    session_t s = { .enabled = true, .connected = true, .bonded = true };
    char reply[128];
    assert(run_turn(&s, "amiibo select save1", reply, sizeof(reply)));
    assert(strstr(reply, "\"ok\":true"));
    assert(run_turn(&s, "body 10 20 30", reply, sizeof(reply)));
    assert(strstr(reply, "\"ok\":true"));
    assert(run_turn(&s, "reenumerate", reply, sizeof(reply)));
    assert(strstr(reply, "\"ok\":true"));
}

static void test_diagnostic_command_rejected_over_ble(void) {
    config_wireless_bridge_init();
    session_t s = { .enabled = true, .connected = true, .bonded = true };
    char reply[128];
    assert(run_turn(&s, "imu", reply, sizeof(reply)));
    assert(strstr(reply, "unavailable"));            // allowlist blocks diagnostics
    assert(run_turn(&s, "sw2cap start", reply, sizeof(reply)));
    assert(strstr(reply, "unavailable"));
}

static void test_unbonded_client_cannot_command(void) {
    config_wireless_bridge_init();
    session_t s = { .enabled = true, .connected = true, .bonded = false };
    char reply[128];
    // Even an allowlisted, otherwise-valid command is refused without a bond.
    assert(run_turn(&s, "save", reply, sizeof(reply)));
    assert(strstr(reply, "unavailable"));
    assert(run_turn(&s, "amiibo present", reply, sizeof(reply)));
    assert(strstr(reply, "unavailable"));
}

static void test_backpressure_between_turns(void) {
    config_wireless_bridge_init();
    // A client that fires a second command before reading the first reply is
    // throttled: the bridge holds one command + one reply at a time.
    assert(config_wireless_bridge_receive((const uint8_t *)"ping\n", 5) ==
           CONFIG_WIRELESS_RX_COMMAND_READY);
    assert(config_wireless_bridge_receive((const uint8_t *)"get\n", 4) ==
           CONFIG_WIRELESS_RX_BUSY);
}

static void test_disconnect_drops_inflight_reply(void) {
    config_wireless_bridge_init();
    assert(config_wireless_bridge_receive((const uint8_t *)"get\n", 4) ==
           CONFIG_WIRELESS_RX_COMMAND_READY);
    char cmd[CONFIG_WIRELESS_COMMAND_CAPACITY]; uint32_t sess;
    assert(config_wireless_bridge_take_command(cmd, sizeof(cmd), &sess));
    // Client disconnects (or the console sleeps -> mgmt_should_drop_client) mid
    // command: the session generation advances and the stale reply is refused,
    // so a reconnecting client never receives a previous session's answer.
    config_wireless_bridge_reset_session();
    assert(!config_wireless_bridge_publish_response(sess, "{\"ok\":true}"));
    assert(!config_wireless_bridge_response_pending());
}

static void test_feature_disabled_rejects_everything(void) {
    config_wireless_bridge_init();
    session_t s = { .enabled = false, .connected = true, .bonded = true };
    char reply[128];
    assert(run_turn(&s, "get", reply, sizeof(reply)));
    assert(strstr(reply, "unavailable"));            // disabled overrides bond
}

int main(void) {
    test_bonded_user_command_succeeds();
    test_diagnostic_command_rejected_over_ble();
    test_unbonded_client_cannot_command();
    test_backpressure_between_turns();
    test_disconnect_drops_inflight_reply();
    test_feature_disabled_rejects_everything();
    puts("mgmt session integration tests passed");
    return 0;
}
