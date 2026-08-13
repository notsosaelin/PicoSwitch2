#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config_wireless_bridge.h"

static uint32_t take(const char *expected)
{
    char command[CONFIG_WIRELESS_COMMAND_CAPACITY];
    uint32_t session = 0;
    assert(config_wireless_bridge_take_command(
        command, sizeof(command), &session));
    assert(strcmp(command, expected) == 0);
    return session;
}

static void test_fragmented_command_and_response(void)
{
    config_wireless_bridge_init();
    assert(config_wireless_bridge_receive(
        (const uint8_t *)"amiibo chu", 10) == CONFIG_WIRELESS_RX_OK);
    assert(config_wireless_bridge_receive(
        (const uint8_t *)"nk 0 AABB\r\n", 11) ==
        CONFIG_WIRELESS_RX_COMMAND_READY);

    uint32_t session = take("amiibo chunk 0 AABB");
    assert(config_wireless_bridge_session_active(session));
    assert(config_wireless_bridge_receive(
        (const uint8_t *)"get\n", 4) == CONFIG_WIRELESS_RX_BUSY);
    assert(config_wireless_bridge_publish_response(
        session, "{\"ok\":true}"));

    uint8_t bytes[32];
    size_t first = config_wireless_bridge_peek_response(bytes, 5);
    assert(first == 5);
    assert(memcmp(bytes, "{\"ok\"", 5) == 0);
    config_wireless_bridge_consume_response(first);

    size_t second = config_wireless_bridge_peek_response(
        bytes, sizeof(bytes));
    assert(second == strlen(":true}\n"));
    assert(memcmp(bytes, ":true}\n", second) == 0);
    config_wireless_bridge_consume_response(second);
    assert(!config_wireless_bridge_response_pending());
}

static void test_busy_and_session_isolation(void)
{
    config_wireless_bridge_init();
    assert(config_wireless_bridge_receive(
        (const uint8_t *)"ping\n", 5) ==
        CONFIG_WIRELESS_RX_COMMAND_READY);
    assert(config_wireless_bridge_receive(
        (const uint8_t *)"get\n", 4) == CONFIG_WIRELESS_RX_BUSY);

    uint32_t old_session = take("ping");
    config_wireless_bridge_reset_session();
    assert(!config_wireless_bridge_session_active(old_session));
    assert(!config_wireless_bridge_publish_response(
        old_session, "{\"ok\":true}"));

    assert(config_wireless_bridge_receive(
        (const uint8_t *)"info\n", 5) ==
        CONFIG_WIRELESS_RX_COMMAND_READY);
    uint32_t new_session = take("info");
    assert(new_session != old_session);
    assert(config_wireless_bridge_publish_response(
        new_session, "{\"id\":\"picoswitch\"}"));
}

static void test_oversized_line_recovers(void)
{
    config_wireless_bridge_init();
    uint8_t oversized[CONFIG_WIRELESS_COMMAND_CAPACITY + 8u];
    memset(oversized, 'x', sizeof(oversized));
    assert(config_wireless_bridge_receive(
        oversized, sizeof(oversized)) == CONFIG_WIRELESS_RX_TOO_LONG);
    assert(config_wireless_bridge_receive(
        (const uint8_t *)"\nping\n", 6) ==
        CONFIG_WIRELESS_RX_COMMAND_READY);
    (void)take("ping");
}

static void test_wireless_command_policy(void)
{
    assert(config_wireless_command_allowed("info"));
    assert(config_wireless_command_allowed("device"));
    assert(config_wireless_command_allowed("input sources"));
    assert(config_wireless_command_allowed("input active 1"));
    assert(config_wireless_command_allowed("input active none"));
    assert(config_wireless_command_allowed("personality"));
    assert(config_wireless_command_allowed("personality gc"));
    assert(config_wireless_command_allowed("wake"));
    assert(config_wireless_command_allowed("mgmt"));
    assert(config_wireless_command_allowed("mgmt status"));
    assert(config_wireless_command_allowed("mgmt off"));
    assert(config_wireless_command_allowed("bonds list"));
    assert(config_wireless_command_allowed("bonds remove 0"));
    assert(config_wireless_command_allowed("save"));
    assert(config_wireless_command_allowed("amiibo status"));
    assert(config_wireless_command_allowed("amiibo chunk 0 0011"));
    assert(config_wireless_command_allowed("amiibo eject"));
    assert(config_wireless_command_allowed("amiibo present"));
    assert(config_wireless_command_allowed("body 1 2 3"));

    assert(!config_wireless_command_allowed(NULL));
    assert(!config_wireless_command_allowed("audiostat"));
    assert(!config_wireless_command_allowed("imu"));
    assert(!config_wireless_command_allowed("fwreads"));
    assert(!config_wireless_command_allowed("sw2cap start"));
    assert(!config_wireless_command_allowed("btid desc 0"));
    assert(!config_wireless_command_allowed("state"));
    assert(!config_wireless_command_allowed("input active"));
    assert(!config_wireless_command_allowed("input activex 1"));
    assert(!config_wireless_command_allowed("raw"));
    assert(!config_wireless_command_allowed("getns2map 0"));
    assert(!config_wireless_command_allowed("setns2map 0 1 2"));
    assert(!config_wireless_command_allowed("amiibo"));
    assert(!config_wireless_command_allowed("mgmtx"));  // prefix must not leak
}

int main(void)
{
    test_fragmented_command_and_response();
    test_busy_and_session_isolation();
    test_oversized_line_recovers();
    test_wireless_command_policy();
    puts("config wireless bridge tests passed");
    return 0;
}
