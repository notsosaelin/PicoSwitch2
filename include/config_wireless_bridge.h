#ifndef CONFIG_WIRELESS_BRIDGE_H
#define CONFIG_WIRELESS_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Cross-core, single-command bridge used by Config and in-band BLE management.
//
// Core 1 owns BLE RX and response notification draining. Core 0 owns the
// existing configuration parser and publishes exactly one JSON-line response.
// The browser already serializes commands, so a bounded one-command/one-reply
// exchange is both sufficient and preferable to an unbounded queue.
#define CONFIG_WIRELESS_COMMAND_CAPACITY 128u
#define CONFIG_WIRELESS_RESPONSE_CAPACITY 512u

typedef enum {
    CONFIG_WIRELESS_RX_OK = 0,
    CONFIG_WIRELESS_RX_COMMAND_READY,
    CONFIG_WIRELESS_RX_BUSY,
    CONFIG_WIRELESS_RX_TOO_LONG,
} config_wireless_rx_result_t;

void config_wireless_bridge_init(void);

// Starts a new management session and invalidates commands/responses belonging
// to a disconnected browser. Safe to call from core 1 at service enter,
// connection replacement, disconnect, and service exit.
void config_wireless_bridge_reset_session(void);

// Feed an arbitrary GATT write fragment. Newline terminates a command; CR is
// ignored. Commands larger than the CDC protocol's existing 127-byte payload
// limit are discarded through the next newline.
config_wireless_rx_result_t config_wireless_bridge_receive(
    const uint8_t *data, size_t length);

// Core 0 takes a complete command and its session token. The token prevents a
// late parser response from crossing into a newly connected browser session.
bool config_wireless_bridge_take_command(
    char *command, size_t capacity, uint32_t *session);

// Core 0 publishes one response. A trailing newline is added for the same
// JSON-lines framing used by USB CDC.
bool config_wireless_bridge_publish_response(
    uint32_t session, const char *response);

// Lets a deferred core-0 operation discard its late result after the BLE
// client disconnects and reset_session invalidates the exchange.
bool config_wireless_bridge_session_active(uint32_t session);

// Core 1 drains the response in ATT-MTU-sized pieces. peek does not advance;
// consume advances only after att_server_notify() succeeds.
size_t config_wireless_bridge_peek_response(
    uint8_t *buffer, size_t capacity);
void config_wireless_bridge_consume_response(size_t length);
bool config_wireless_bridge_response_pending(void);

// Config BLE intentionally exposes only production settings and Virtual
// Amiibo operations. Research/capture/audio/motion diagnostics stay on the
// wired USB/UART paths.
bool config_wireless_command_allowed(const char *command);

#endif
