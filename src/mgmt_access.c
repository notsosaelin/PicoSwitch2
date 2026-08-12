/*
 * In-band BLE management access-control decisions (production).
 *
 * Lifted verbatim from the test-first spec in tools/test_mgmt_access.c. See
 * mgmt_access.h and docs/bluetooth/in-band-management-plan.md for the rationale
 * behind each rule. The only external dependency is the real wireless allowlist.
 */
#include "mgmt_access.h"

#include "config_wireless_bridge.h"  // config_wireless_command_allowed (real)

bool mgmt_should_advertise(const mgmt_state_t *s)
{
    return s->enabled && s->console_awake && !s->wake_active &&
           !s->scanning && !s->client_connected;
}

bool mgmt_accept_connection(const mgmt_state_t *s)
{
    return s->enabled && s->console_awake && !s->client_connected;
}

bool mgmt_accept_bonding(const mgmt_state_t *s)
{
    return s->enabled && s->pairing_window_open;
}

bool mgmt_allow_write(const mgmt_state_t *s, const char *command)
{
    return s->enabled && s->client_connected && s->client_bonded &&
           config_wireless_command_allowed(command);
}

bool mgmt_should_drop_client(const mgmt_state_t *s)
{
    return s->client_connected &&
           (!s->enabled || !s->console_awake || s->wake_active);
}
