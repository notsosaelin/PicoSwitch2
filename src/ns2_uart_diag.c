#include "ns2_uart_diag.h"

#ifdef NS2_UART_DIAG

#include "ns2_firmware_profile.h"
#include "ns2_bt_version_probe.h"

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
#define NS2_UART_TASK_TX_BUDGET 32u

static char rx_line[NS2_UART_RX_LINE_SIZE];
static size_t rx_length;
static bool rx_overflow;
static char tx_buffer[NS2_UART_TX_BUFFER_SIZE];
static size_t tx_length;
static size_t tx_position;
static bool reenumerate_requested;

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
    } else if (strcmp(rx_line, "help") == 0) {
        queue_text("{\"commands\":[\"ping\",\"fwreads\",\"status\",\"clear\","
                   "\"profile\",\"profile default\","
                   "\"profile C.M.m B.M.m D.M.m\",\"btversion request\","
                   "\"btversion\",\"help\"]}");
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
    reenumerate_requested = false;
    while (uart_is_readable(NS2_UART_ID)) (void)uart_getc(NS2_UART_ID);
}

bool ns2_uart_diag_take_reenumerate_request(void) {
    bool requested = reenumerate_requested;
    reenumerate_requested = false;
    return requested;
}

void ns2_uart_diag_task(void) {
    uint8_t tx_budget = NS2_UART_TASK_TX_BUDGET;
    while (tx_budget-- && tx_pending() && uart_is_writable(NS2_UART_ID))
        uart_putc_raw(NS2_UART_ID, tx_buffer[tx_position++]);

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
