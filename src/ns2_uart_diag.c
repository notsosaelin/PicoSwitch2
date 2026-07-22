#include "ns2_uart_diag.h"

#ifdef NS2_UART_DIAG

#include "ns2_firmware_profile.h"
#include "ns2_bt_version_probe.h"
#include "ns2_protocol_trace.h"
#include "sw2_capture.h"

#include <hardware/gpio.h>
#include <hardware/uart.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NS2_UART_ID uart0
#define NS2_UART_BAUD 115200u
#define NS2_UART_TX_PIN 0u
#define NS2_UART_RX_PIN 1u
#define NS2_UART_RX_LINE_SIZE 96u
#define NS2_UART_TX_BUFFER_SIZE 2304u
#define NS2_UART_TASK_RX_BUDGET 16u
#define NS2_UART_TASK_TX_BUDGET 8u

static char rx_line[NS2_UART_RX_LINE_SIZE];
static size_t rx_length;
static bool rx_overflow;
static char tx_buffer[NS2_UART_TX_BUFFER_SIZE];
static size_t tx_length;
static size_t tx_position;
static bool tx_wait_idle;
static bool reenumerate_requested;
static ns2_protocol_trace_record_t trace_format_record;
static char trace_format_payload[NS2_PROTOCOL_TRACE_PAYLOAD_MAX * 2u + 1u];
static char trace_format_response[384];
static sw2_cap_entry_t ble_format_record;
static char ble_format_payload[SW2_CAP_MAX_DATA * 2u + 1u];
static char ble_format_response[384];

static bool tx_pending(void) {
    return tx_position < tx_length;
}

static void queue_text(const char *text) {
    size_t length = strlen(text);
    if (length > sizeof(tx_buffer) - 2) length = sizeof(tx_buffer) - 2;
    memcpy(tx_buffer, text, length);
    tx_buffer[length++] = '\r';
    tx_buffer[length++] = '\n';
    tx_length = length;
    tx_position = 0;
}

static bool parse_profile(const char *text, uint8_t controller[3],
                          uint8_t bluetooth[3], uint8_t dsp[3]) {
    unsigned int values[9];
    int consumed = 0;
    int matched = sscanf(text,
                         "%u.%u.%u %u.%u.%u %u.%u.%u %n",
                         &values[0], &values[1], &values[2],
                         &values[3], &values[4], &values[5],
                         &values[6], &values[7], &values[8], &consumed);
    if (matched != 9 || text[consumed] != '\0') return false;
    for (size_t i = 0; i < 9; i++) {
        if (values[i] > UINT8_MAX) return false;
    }
    for (size_t i = 0; i < 3; i++) {
        controller[i] = (uint8_t)values[i];
        bluetooth[i] = (uint8_t)values[i + 3];
        dsp[i] = (uint8_t)values[i + 6];
    }
    return true;
}

static void queue_bt_version(void) {
    ns2_bt_version_result_t result;
    ns2_bt_version_probe_snapshot(&result);
    if (result.state == NS2_BT_VERSION_READY && result.length == 12) {
        char raw[25];
        for (size_t i = 0; i < 12; i++)
            snprintf(&raw[i * 2], 3, "%02X", result.raw[i]);
        snprintf(tx_buffer, sizeof(tx_buffer),
                 "{\"state\":\"ready\",\"raw\":\"%s\","
                 "\"controller\":\"%u.%u.%u\",\"type\":%u,"
                 "\"bluetooth\":\"%u.%u.%u\",\"dsp\":\"%u.%u.%u\"}",
                 raw, result.raw[0], result.raw[1], result.raw[2], result.raw[3],
                 result.raw[4], result.raw[5], result.raw[6],
                 result.raw[8], result.raw[9], result.raw[10]);
    } else {
        snprintf(tx_buffer, sizeof(tx_buffer),
                 "{\"state\":\"%s\",\"length\":%u}",
                 ns2_bt_version_state_name(result.state), result.length);
    }
    char response[320];
    snprintf(response, sizeof(response), "%s", tx_buffer);
    queue_text(response);
}

static const char *trace_personality_name(uint8_t personality) {
    switch (personality) {
        case 0: return "pro2";
        case 1: return "gc";
        case 2: return "joycon_l";
        case 3: return "joycon_r";
        case 4: return "config";
        default: return "unknown";
    }
}

static const char *trace_kind_name(uint8_t kind) {
    switch ((ns2_protocol_trace_kind_t)kind) {
        case NS2_TRACE_EP0_SETUP: return "ep0_setup";
        case NS2_TRACE_EP0_RESPONSE: return "ep0_response";
        case NS2_TRACE_BULK_COMMAND: return "bulk_command";
        case NS2_TRACE_BULK_RESPONSE: return "bulk_response";
        case NS2_TRACE_HID_OUTPUT: return "hid_output";
        default: return "unknown";
    }
}

static void queue_trace_status(const char *event) {
    ns2_protocol_trace_status_t status;
    char response[256];
    ns2_protocol_trace_get_status(&status);
    snprintf(response, sizeof(response),
             "{\"trace\":\"%s\",\"enabled\":%s,\"count\":%u,"
             "\"capacity\":%u,\"overwritten\":%lu,\"next_sequence\":%lu}",
             event, status.enabled ? "true" : "false", status.count,
             status.capacity, (unsigned long)status.overwritten,
             (unsigned long)status.next_sequence);
    queue_text(response);
}

static void queue_trace_record(uint16_t index) {
    if (!ns2_protocol_trace_get(index, &trace_format_record)) {
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"trace\":\"error\",\"error\":\"record out of range\","
                 "\"index\":%u}", index);
        queue_text(trace_format_response);
        return;
    }

    for (size_t i = 0; i < trace_format_record.captured_length; i++)
        snprintf(&trace_format_payload[i * 2u], 3, "%02X",
                 trace_format_record.payload[i]);
    trace_format_payload[trace_format_record.captured_length * 2u] = '\0';

    snprintf(trace_format_response, sizeof(trace_format_response),
             "{\"trace\":\"record\",\"seq\":%lu,\"t_us\":%lu,"
             "\"personality\":\"%s\",\"kind\":\"%s\",\"dir\":\"%s\","
             "\"id\":%u,\"sub\":%u,\"length\":%u,\"captured\":%u,"
             "\"payload\":\"%s\"}",
             (unsigned long)trace_format_record.sequence,
             (unsigned long)trace_format_record.timestamp_us,
             trace_personality_name(trace_format_record.personality),
             trace_kind_name(trace_format_record.kind),
             trace_format_record.direction == NS2_TRACE_DEVICE_TO_CONSOLE ?
                 "device_to_console" : "console_to_device",
             trace_format_record.id, trace_format_record.subcommand,
             trace_format_record.total_length,
             trace_format_record.captured_length, trace_format_payload);
    queue_text(trace_format_response);
}

static void queue_ble_status(const char *event) {
    char response[256];
    snprintf(response, sizeof(response),
             "{\"blecap\":\"%s\",\"enabled\":%s,\"count\":%u,\"dropped\":%lu,\"variant\":%u}",
             event, sw2_capture_get_enabled() ? "true" : "false",
             sw2_capture_buffered_count(), (unsigned long)sw2_capture_dropped_count(),
             sw2_get_v2_variant());
    queue_text(response);
}

static void queue_ble_record(void) {
    if (!sw2_capture_drain_one(&ble_format_record)) {
        queue_text("{\"blecap\":\"empty\"}");
        return;
    }
    for (size_t i = 0; i < ble_format_record.len; i++)
        snprintf(&ble_format_payload[i * 2u], 3, "%02X", ble_format_record.data[i]);
    ble_format_payload[ble_format_record.len * 2u] = '\0';
    snprintf(ble_format_response, sizeof(ble_format_response),
             "{\"blecap\":\"record\",\"t_us\":%llu,\"kind\":\"%s\","
             "\"handle\":\"0x%04X\",\"length\":%u,\"captured\":%u,\"payload\":\"%s\"}",
             (unsigned long long)ble_format_record.us,
             sw2_capture_kind_name(ble_format_record.kind), ble_format_record.handle,
             ble_format_record.orig_len, ble_format_record.len, ble_format_payload);
    queue_text(ble_format_response);
}

static void handle_command(void) {
    rx_line[rx_length] = '\0';
    if (rx_overflow) {
        queue_text("{\"error\":\"command too long\"}");
    } else if (strcmp(rx_line, "ping") == 0) {
        queue_text("{\"ok\":true,\"transport\":\"uart0\",\"baud\":115200}");
    } else if (strcmp(rx_line, "fwreads") == 0 || strcmp(rx_line, "status") == 0) {
        size_t length = ns2_firmware_diagnostics_format_json(tx_buffer,
                                                              sizeof(tx_buffer) - 2);
        tx_buffer[length++] = '\r';
        tx_buffer[length++] = '\n';
        tx_length = length;
        tx_position = 0;
    } else if (strcmp(rx_line, "clear") == 0) {
        ns2_firmware_diagnostics_reset();
        queue_text("{\"ok\":true,\"cleared\":true}");
    } else if (strcmp(rx_line, "profile") == 0) {
        size_t length = ns2_firmware_diagnostics_format_json(tx_buffer,
                                                              sizeof(tx_buffer) - 2);
        tx_buffer[length++] = '\r';
        tx_buffer[length++] = '\n';
        tx_length = length;
        tx_position = 0;
    } else if (strcmp(rx_line, "profile default") == 0) {
        ns2_firmware_profile_reset_runtime();
        reenumerate_requested = true;
        queue_text("{\"ok\":true,\"profile\":\"default\",\"reenumerate\":true}");
    } else if (strncmp(rx_line, "profile ", 8) == 0) {
        uint8_t controller[3], bluetooth[3], dsp[3];
        if (!parse_profile(rx_line + 8, controller, bluetooth, dsp)) {
            queue_text("{\"error\":\"usage: profile C.M.m B.M.m D.M.m (each 0..255)\"}");
        } else {
            char response[256];
            ns2_firmware_profile_set_runtime(controller, bluetooth, dsp);
            reenumerate_requested = true;
            snprintf(response, sizeof(response),
                     "{\"ok\":true,\"profile\":{\"controller\":\"%u.%u.%u\","
                     "\"bluetooth\":\"%u.%u.%u\",\"dsp\":\"%u.%u.%u\"},"
                     "\"runtime_override\":true,\"reenumerate\":true}",
                     controller[0], controller[1], controller[2],
                     bluetooth[0], bluetooth[1], bluetooth[2],
                     dsp[0], dsp[1], dsp[2]);
            queue_text(response);
        }
    } else if (strcmp(rx_line, "btversion request") == 0) {
        ns2_bt_version_probe_request();
        queue_text("{\"ok\":true,\"state\":\"requested\"}");
    } else if (strcmp(rx_line, "btversion") == 0) {
        queue_bt_version();
    } else if (strcmp(rx_line, "trace") == 0 || strcmp(rx_line, "trace status") == 0) {
        queue_trace_status("status");
    } else if (strcmp(rx_line, "trace clear") == 0) {
        ns2_protocol_trace_clear();
        queue_trace_status("cleared");
    } else if (strcmp(rx_line, "trace start") == 0) {
        ns2_protocol_trace_set_enabled(true);
        queue_trace_status("started");
    } else if (strcmp(rx_line, "trace stop") == 0) {
        ns2_protocol_trace_set_enabled(false);
        queue_trace_status("stopped");
    } else if (strcmp(rx_line, "trace dump") == 0) {
        ns2_protocol_trace_status_t status;
        ns2_protocol_trace_set_enabled(false);
        ns2_protocol_trace_get_status(&status);
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"trace\":\"dump\",\"count\":%u,\"overwritten\":%lu}",
                 status.count, (unsigned long)status.overwritten);
        queue_text(trace_format_response);
    } else if (strncmp(rx_line, "trace read ", 11) == 0) {
        unsigned int index;
        char trailing;
        if (sscanf(rx_line + 11, "%u%c", &index, &trailing) != 1 ||
            index > UINT16_MAX) {
            queue_text("{\"trace\":\"error\",\"error\":\"usage: trace read N\"}");
        } else {
            queue_trace_record((uint16_t)index);
        }
    } else if (strcmp(rx_line, "blecap") == 0 || strcmp(rx_line, "blecap status") == 0) {
        queue_ble_status("status");
    } else if (strcmp(rx_line, "blecap start") == 0) {
        sw2_capture_set_enabled(true);
        queue_ble_status("started");
    } else if (strcmp(rx_line, "blecap stop") == 0) {
        sw2_capture_set_enabled(false);
        queue_ble_status("stopped");
    } else if (strcmp(rx_line, "blecap dump") == 0) {
        sw2_capture_set_enabled(false);
        queue_ble_status("dump");
    } else if (strcmp(rx_line, "blecap read") == 0) {
        queue_ble_record();
    } else if (strcmp(rx_line, "blecap gattdisc on") == 0) {
        sw2_set_gatt_discovery_enabled(true);
        queue_text("{\"blecap\":\"gattdisc\",\"enabled\":true}");
    } else if (strcmp(rx_line, "blecap gattdisc off") == 0) {
        sw2_set_gatt_discovery_enabled(false);
        queue_text("{\"blecap\":\"gattdisc\",\"enabled\":false}");
    } else if (strcmp(rx_line, "blecap gattdisc status") == 0) {
        queue_text(sw2_get_gatt_discovery_enabled()
            ? "{\"blecap\":\"gattdisc\",\"enabled\":true}"
            : "{\"blecap\":\"gattdisc\",\"enabled\":false}");
    } else if (strncmp(rx_line, "blecap variant ", 15) == 0) {
        unsigned int variant;
        char trailing;
        if (sscanf(rx_line + 15, "%u%c", &variant, &trailing) != 1 || variant > 9) {
            queue_text("{\"blecap\":\"error\",\"error\":\"usage: blecap variant 0-9\"}");
        } else {
            sw2_set_v2_variant((uint8_t)variant);
            queue_ble_status("variant");
        }
    } else if (strncmp(rx_line, "blecap mark ", 12) == 0) {
        const char *label = rx_line + 12;
        size_t length = strlen(label);
        if (length > SW2_CAP_MAX_DATA) length = SW2_CAP_MAX_DATA;
        sw2_capture_mark((const uint8_t *)label, (uint16_t)length);
        queue_ble_status("marked");
    } else if (strcmp(rx_line, "motionauto") == 0) {
        sw2_native_auto_diag_t d;
        sw2_native_auto_diag_snapshot(&d);
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"motionauto\":true,\"checks\":%lu,\"starts\":%lu,"
                 "\"wait_ms\":%lu,\"pid\":\"0x%04X\",\"personality\":%u,"
                 "\"init_state\":%u,\"v2_state\":%u,\"fired\":%s,"
                 "\"armed\":%u,\"gattdisc\":%s,\"block_mask\":\"0x%02X\"}",
                 (unsigned long)d.checks, (unsigned long)d.starts,
                 (unsigned long)d.wait_elapsed_ms, d.source_pid, d.personality,
                 d.init_state, d.v2_state, d.auto_fired ? "true" : "false",
                 d.armed_variant, d.gatt_discovery ? "true" : "false", d.block_mask);
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "reenumerate") == 0) {
        reenumerate_requested = true;
        queue_text("{\"ok\":true,\"reenumerate\":true}");
    } else if (strcmp(rx_line, "help") == 0) {
        queue_text("{\"commands\":[\"ping\",\"fwreads\",\"status\",\"clear\","
                   "\"profile\",\"profile default\","
                   "\"profile C.M.m B.M.m D.M.m\",\"btversion request\","
                   "\"btversion\",\"trace status\",\"trace clear\","
                   "\"trace start\",\"trace stop\",\"trace dump\","
                   "\"trace read N\","
                   "\"blecap status\",\"blecap start\",\"blecap stop\","
                   "\"blecap dump\",\"blecap read\",\"blecap variant 0-9\","
                   "\"blecap gattdisc on|off|status\","
                   "\"blecap mark TEXT\","
                   "\"motionauto\",\"reenumerate\",\"help\"]}");
    } else if (rx_length != 0) {
        queue_text("{\"error\":\"unknown command\"}");
    }
    rx_length = 0;
    rx_overflow = false;
}

void ns2_uart_diag_init(void) {
    uart_init(NS2_UART_ID, NS2_UART_BAUD);
    gpio_set_function(NS2_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(NS2_UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_hw_flow(NS2_UART_ID, false, false);
    uart_set_format(NS2_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(NS2_UART_ID, true);

    rx_length = 0;
    rx_overflow = false;
    tx_length = 0;
    tx_position = 0;
    tx_wait_idle = false;
    reenumerate_requested = false;
    ns2_protocol_trace_set_enabled(false);
    ns2_protocol_trace_clear();
    while (uart_is_readable(NS2_UART_ID)) (void)uart_getc(NS2_UART_ID);
}

bool ns2_uart_diag_take_reenumerate_request(void) {
    bool requested = reenumerate_requested;
    reenumerate_requested = false;
    return requested;
}

void ns2_uart_diag_task(void) {
    if (tx_wait_idle) {
        if (uart_get_hw(NS2_UART_ID)->fr & UART_UARTFR_BUSY_BITS) return;
        tx_wait_idle = false;
    }

    uint8_t tx_budget = NS2_UART_TASK_TX_BUDGET;
    uint8_t tx_sent = 0;
    while (tx_budget-- && tx_pending() && uart_is_writable(NS2_UART_ID)) {
        uart_putc_raw(NS2_UART_ID, tx_buffer[tx_position++]);
        tx_sent++;
    }

    // Deliberately allow the FIFO and shift register to drain after each small
    // chunk. Continuous full-rate JSON exceeded the reliable sustained receive
    // behavior of the bench CP2102 path despite correct framing and large PC
    // buffers. This remains nonblocking and affects UART diagnostics only.
    if (tx_sent) {
        tx_wait_idle = true;
        return;
    }

    if (tx_pending()) return;
    tx_length = 0;
    tx_position = 0;

    uint8_t rx_budget = NS2_UART_TASK_RX_BUDGET;
    while (rx_budget-- && !tx_pending() && uart_is_readable(NS2_UART_ID)) {
        char c = (char)uart_getc(NS2_UART_ID);
        if (c == '\n' || c == '\r') {
            if (rx_length != 0 || rx_overflow) handle_command();
        } else if (!rx_overflow) {
            if (rx_length < sizeof(rx_line) - 1)
                rx_line[rx_length++] = c;
            else
                rx_overflow = true;
        }
    }
}

#else

void ns2_uart_diag_init(void) {}
void ns2_uart_diag_task(void) {}
bool ns2_uart_diag_take_reenumerate_request(void) { return false; }

#endif
