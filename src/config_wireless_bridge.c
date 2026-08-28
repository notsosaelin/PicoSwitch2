#include "config_wireless_bridge.h"

#include <string.h>

enum {
    SLOT_EMPTY = 0,
    SLOT_READY = 1,
    SLOT_IN_PROGRESS = 2,
};

static char rx_line[CONFIG_WIRELESS_COMMAND_CAPACITY];
static size_t rx_length;
static bool rx_overflow;

static char command_slot[CONFIG_WIRELESS_COMMAND_CAPACITY];
static uint16_t command_length;
static uint32_t command_session;
static volatile uint32_t command_state;

static uint8_t response_slot[CONFIG_WIRELESS_RESPONSE_CAPACITY];
static uint16_t response_length;
static uint16_t response_offset;
static uint32_t response_session;
static volatile uint32_t response_state;

static volatile uint32_t active_session;

static uint32_t load_acquire(const volatile uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void store_release(volatile uint32_t *value, uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

void config_wireless_bridge_init(void)
{
    rx_length = 0;
    rx_overflow = false;
    command_length = 0;
    response_length = 0;
    response_offset = 0;
    store_release(&command_state, SLOT_EMPTY);
    store_release(&response_state, SLOT_EMPTY);
    __atomic_store_n(&active_session, 1u, __ATOMIC_RELEASE);
}

void config_wireless_bridge_reset_session(void)
{
    rx_length = 0;
    rx_overflow = false;
    command_length = 0;
    response_length = 0;
    response_offset = 0;
    store_release(&command_state, SLOT_EMPTY);
    store_release(&response_state, SLOT_EMPTY);
    (void)__atomic_add_fetch(&active_session, 1u, __ATOMIC_ACQ_REL);
}

config_wireless_rx_result_t config_wireless_bridge_receive(
    const uint8_t *data, size_t length)
{
    if (!data && length != 0) {
        return CONFIG_WIRELESS_RX_TOO_LONG;
    }

    config_wireless_rx_result_t result = CONFIG_WIRELESS_RX_OK;
    for (size_t i = 0; i < length; ++i) {
        uint8_t c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            if (rx_overflow) {
                continue;
            }
            if (rx_length >= CONFIG_WIRELESS_COMMAND_CAPACITY - 1u) {
                rx_overflow = true;
                result = CONFIG_WIRELESS_RX_TOO_LONG;
                continue;
            }
            rx_line[rx_length++] = (char)c;
            continue;
        }

        if (rx_overflow) {
            rx_length = 0;
            rx_overflow = false;
            result = CONFIG_WIRELESS_RX_TOO_LONG;
            continue;
        }
        if (rx_length == 0) {
            continue;
        }
        if (load_acquire(&command_state) != SLOT_EMPTY ||
            load_acquire(&response_state) != SLOT_EMPTY) {
            rx_length = 0;
            result = CONFIG_WIRELESS_RX_BUSY;
            continue;
        }

        memcpy(command_slot, rx_line, rx_length);
        command_slot[rx_length] = '\0';
        command_length = (uint16_t)rx_length;
        command_session = load_acquire(&active_session);
        rx_length = 0;
        store_release(&command_state, SLOT_READY);
        result = CONFIG_WIRELESS_RX_COMMAND_READY;
    }
    return result;
}

bool config_wireless_bridge_take_command(
    char *command, size_t capacity, uint32_t *session)
{
    if (!command || capacity == 0 || !session ||
        load_acquire(&command_state) != SLOT_READY) {
        return false;
    }

    size_t length = command_length;
    if (length + 1u > capacity) {
        store_release(&command_state, SLOT_EMPTY);
        return false;
    }
    memcpy(command, command_slot, length + 1u);
    *session = command_session;
    // Keep the exchange occupied until its response is published. Most
    // commands reply inline, but core-owned operations may complete on a later
    // task tick; a second GATT write must not slip into that deferred window.
    store_release(&command_state, SLOT_IN_PROGRESS);
    return true;
}

bool config_wireless_bridge_publish_response(
    uint32_t session, const char *response)
{
    if (!response || session != load_acquire(&active_session) ||
        load_acquire(&response_state) != SLOT_EMPTY) {
        return false;
    }

    size_t length = strlen(response);
    if (length + 1u > CONFIG_WIRELESS_RESPONSE_CAPACITY) {
        return false;
    }
    memcpy(response_slot, response, length);
    response_slot[length++] = '\n';
    response_length = (uint16_t)length;
    response_offset = 0;
    response_session = session;
    store_release(&response_state, SLOT_READY);
    store_release(&command_state, SLOT_EMPTY);
    return true;
}

bool config_wireless_bridge_session_active(uint32_t session)
{
    return session == load_acquire(&active_session);
}

size_t config_wireless_bridge_peek_response(
    uint8_t *buffer, size_t capacity)
{
    if (!buffer || capacity == 0 ||
        load_acquire(&response_state) != SLOT_READY ||
        response_session != load_acquire(&active_session) ||
        response_offset >= response_length) {
        return 0;
    }

    size_t remaining = (size_t)response_length - response_offset;
    size_t length = remaining < capacity ? remaining : capacity;
    memcpy(buffer, response_slot + response_offset, length);
    return length;
}

void config_wireless_bridge_consume_response(size_t length)
{
    if (load_acquire(&response_state) != SLOT_READY) {
        return;
    }
    size_t remaining = (size_t)response_length - response_offset;
    if (length > remaining) {
        length = remaining;
    }
    response_offset = (uint16_t)(response_offset + length);
    if (response_offset >= response_length) {
        response_length = 0;
        response_offset = 0;
        store_release(&response_state, SLOT_EMPTY);
    }
}

bool config_wireless_bridge_response_pending(void)
{
    return load_acquire(&response_state) == SLOT_READY &&
           response_session == load_acquire(&active_session) &&
           response_offset < response_length;
}

bool config_wireless_command_allowed(const char *command)
{
    if (!command) {
        return false;
    }

    return strcmp(command, "info") == 0 ||
           strcmp(command, "ping") == 0 ||
           strcmp(command, "get") == 0 ||
           strcmp(command, "device") == 0 ||
           strcmp(command, "input sources") == 0 ||
           strncmp(command, "input active ", 13) == 0 ||
           // Keyboard / Keyboard + Mouse configuration. Mutating forms are
           // included deliberately: wireless RX has already passed
           // mgmt_allow_write() (bonded, encrypted, enabled), and the whole
           // point of this surface is that a remapping UI can use it over the
           // same transport as every other setting.
           strcmp(command, "kbm") == 0 ||
           strncmp(command, "kbm ", 4) == 0 ||
           strcmp(command, "personality") == 0 ||
           strncmp(command, "personality ", 12) == 0 ||
           strcmp(command, "reenumerate") == 0 ||
           strcmp(command, "wake") == 0 ||
           strcmp(command, "wake status") == 0 ||
           strcmp(command, "mgmt") == 0 ||
           strncmp(command, "mgmt ", 5) == 0 ||
           strncmp(command, "bonds ", 6) == 0 ||
           // Peer inventory and selective forget. Carries identity and role,
           // never key material. `peers forget` IS a mutating form -- included
           // deliberately, on the same reasoning as the KB/M writes above:
           // wireless RX has already passed mgmt_allow_write(), and removing a
           // controller pairing from the companion is the whole point of the
           // feature. The firmware refuses to forget the management companion
           // itself, so this cannot be turned against the session issuing it.
           strncmp(command, "peers ", 6) == 0 ||
           // Remote controller pairing. Mutating (start/cancel) and read
           // (status). Opens the SAME window the adapter's own pairing button
           // opens, and grants no management bonding authority, so the worst a
           // holder of an authorised session can do is make the adapter
           // discoverable for its bounded, firmware-owned window.
           strncmp(command, "pairing ", 8) == 0 ||
           strcmp(command, "save") == 0 ||
           strcmp(command, "save status") == 0 ||
           strncmp(command, "amiibo ", 7) == 0 ||
           strncmp(command, "body ", 5) == 0 ||
           strncmp(command, "jcl ", 4) == 0 ||
           strncmp(command, "jcr ", 4) == 0 ||
           strncmp(command, "lb ", 3) == 0;
}
