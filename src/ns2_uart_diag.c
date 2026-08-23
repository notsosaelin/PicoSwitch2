#include "ns2_uart_diag.h"

#ifdef NS2_UART_DIAG

#include "ns2_firmware_profile.h"
#include "ns2_bt_version_probe.h"
#include "ns2_protocol_trace.h"
#include "ns2_nfc_mirror.h"
#include "sw2_capture.h"
#include "ns2_native_motion.h"
#include "ns2_motion_probe.h"
#include "ns2_motion_pdu.h"
#include "ns2_motion_hybrid_live.h"
#include "ns2_ds5_motion40.h"
#include "ns2_diag_input.h"
#include "ns2_rumble_trace.h"
#include "bt/bthid/devices/generic/bthid_android_bridge.h"
#include "fixtures/android_controller_hid.h" // ANDROID_BRIDGE_CONTRACT_VERSION
#include "ns2_active_input.h"
#include "ns2_kbm.h"
#include "ns2_kbm_runtime.h"
#include "ns2_kbm_status.h"
#include "ns2_owner_led.h"
#include "ns2_bt_health.h"
#include "ns2_bt_lifecycle.h"  // auth-observation naming + peer-led security verdict
#include "ns2_bt_recovery_runtime.h"
#include "config.h"  // config_request_save (the shared deferred settings write)
#include "bt/bthid/bthid.h"                                  // live device table (btdev)
#include "bt/bthid/devices/generic/bthid_gamepad.h"           // generic-fallback identity
#include "bt/bthid/devices/generic/bthid_keyboard.h"          // structural keyboard test
#include "bt/bthid/devices/generic/bthid_mouse.h"             // structural mouse test
#include "ds5_audio_bridge.h"
#include "ds5_motion_pair_capture.h"
#include "controller_headset.h"
#include "virtual_amiibo_store.h"
#include "ns2_virtual_nfc.h"
#include "report.h"
#include "switch_pro2.h"
#include "usb.h"  // dev-only persona/mgmt transition triggers (g_usb_* flags, g_mgmt_enabled)
#include "bt/btstack/btstack_host.h"

#include <hardware/gpio.h>
#include <hardware/uart.h>
#include <pico/bootrom.h>  // dev-only `bootsel`: removes the physical BOOTSEL dependency
#include <pico/time.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t ns2_audio_core1_stack_free_bytes(void);

#define NS2_UART_ID uart0
#define NS2_UART_BAUD 115200u
#define NS2_UART_TX_PIN 0u
#define NS2_UART_RX_PIN 1u
#define NS2_UART_RX_LINE_SIZE 96u
#define NS2_UART_TX_BUFFER_SIZE 2304u
#define NS2_UART_TASK_RX_BUDGET 16u
#define NS2_UART_TASK_TX_BUDGET 8u
#define NS2_UART_Y_PULSE_US 120000u
#define NS2_UART_AMIIBO_READ_MAX 64u
// Bounded by the 96-byte RX line, not by the bridge: "nfcmirror send " plus two
// hex characters per byte. Ample for every read-path command (the longest, the
// 0x06 read descriptor, is 27 bytes); tag writes are longer and stay on the
// console path.
#define NS2_UART_NFC_COMMAND_MAX 40u

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
// Shared response scratch. btstate is the largest producer and grows whenever a
// subsystem adds a counter; a silent snprintf truncation there emits invalid
// JSON, which a soak harness reads as a parse failure rather than as the
// diagnostic loss it actually is. queue_btstate() checks for truncation
// explicitly, and this stays comfortably ahead of the tx buffer's 2304.
static char trace_format_response[2048];
static sw2_cap_entry_t ble_format_record;
static char ble_format_payload[SW2_CAP_MAX_DATA * 2u + 1u];
static char ble_format_response[512];
static uint8_t pcm_format_data[DS5_AUDIO_PCM_CAPTURE_READ_MAX];
static char pcm_format_payload[DS5_AUDIO_PCM_CAPTURE_READ_MAX * 2u + 1u];
static char pcm_format_response[896];
static ds5_motion_pair_record_t motion_pair_format_record;
static char motion_pair_format_payload[DS5_MOTION_PAIR_NATIVE_MAX * 2u + 1u];
static char motion_pair_format_response[768];
static char rumble_format_response[320];
static ns2_motion_hybrid_capture_record_t motion_hybrid_format_record;
static char motion_hybrid_base[NS2_MOTION_PDU40_LENGTH * 2u + 1u];
static char motion_hybrid_xor[NS2_MOTION_PDU40_LENGTH * 2u + 1u];
static char motion_hybrid_format_response[1024];
_Static_assert(NS2_MOTION_HYBRID_REASON_COUNT == 13u,
               "update motionhybrid UART reason counters");

static bool tx_pending(void) {
    return tx_position < tx_length;
}

static void queue_text(const char *text);

static void queue_active_input_status(void) {
    ns2_input_arbiter_status_t status;
    ns2_active_input_status(&status);
    int j = snprintf(trace_format_response, sizeof(trace_format_response),
                     "{\"active_input\":{\"active\":%lu,\"pending\":%lu,"
                     "\"explicit\":%s,\"fresh\":%s,\"transitions\":%lu,\"sources\":[",
                     (unsigned long)status.active_id,
                     (unsigned long)status.pending_id,
                     status.explicit_active ? "true" : "false",
                     status.awaiting_fresh ? "true" : "false",
                     (unsigned long)status.transition_count);
    for (unsigned i = 0; i < status.source_count && j < (int)sizeof(trace_format_response) - 96; ++i) {
        const ns2_input_source_info_t *source = &status.sources[i];
        char name[33];
        unsigned n = 0;
        for (; source->name[n] && n < sizeof(name) - 1u; ++n) {
            unsigned char c = (unsigned char)source->name[n];
            name[n] = (c < 0x20u || c == '"' || c == '\\') ? ' ' : (char)c;
        }
        name[n] = '\0';
        j += snprintf(trace_format_response + j,
                      sizeof(trace_format_response) - (size_t)j,
                      "%s{\"id\":%lu,\"conn\":%u,\"transport\":%u,"
                      "\"generation\":%lu,\"vid\":%u,\"pid\":%u,\"name\":\"%s\"}",
                      i ? "," : "", (unsigned long)source->id,
                      source->key.dev_addr, source->key.transport,
                      (unsigned long)source->generation,
                      source->vendor_id, source->product_id, name);
        if (j < 0) j = 0;
        if ((size_t)j >= sizeof(trace_format_response)) {
            j = (int)sizeof(trace_format_response) - 1;
            break;
        }
    }
    snprintf(trace_format_response + j,
             sizeof(trace_format_response) - (size_t)j,
             "],\"more\":false}}}");
    queue_text(trace_format_response);
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
             "\"capacity\":%u,\"overwritten\":%lu,\"next_sequence\":%lu,"
             "\"filter\":\"%s\"}",
             event, status.enabled ? "true" : "false", status.count,
             status.capacity, (unsigned long)status.overwritten,
             (unsigned long)status.next_sequence,
             status.filter == NS2_TRACE_FILTER_NFC ? "nfc" :
             status.filter == NS2_TRACE_FILTER_BULK ? "bulk" : "all");
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
             "{\"blecap\":\"%s\",\"enabled\":%s,\"count\":%u,\"dropped\":%lu,"
             "\"variant\":%u,\"filter\":\"%s\"}",
             event, sw2_capture_get_enabled() ? "true" : "false",
             sw2_capture_buffered_count(), (unsigned long)sw2_capture_dropped_count(),
             sw2_get_v2_variant(),
             sw2_capture_get_filter() == SW2_CAPTURE_FILTER_NFC ? "nfc" : "all");
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

// In-band management / BLE coexistence snapshot: live radio + config_ble service
// state plus scan-suppression cause counters. `suppress.mgmt_armed` climbing is
// the fingerprint of the confirmed coexistence bug (management-armed permanently
// blocks controller reconnect). See docs/bluetooth/in-band-management-plan.md.
static void queue_btstate(void) {
    btstack_host_mgmt_diag_t d;
    btstack_host_get_mgmt_diag(&d);
    int btstate_len = snprintf(trace_format_response, sizeof(trace_format_response),
        "{\"btstate\":\"status\",\"mgmt_enabled\":%s,\"config_mode\":%s,"
        "\"personality\":\"%s\",\"powered_on\":%s,\"hid_state\":%u,"
        "\"scan_active\":%s,\"inquiry_active\":%s,\"wake_adv\":%s,"
        "\"controller_connected\":%s,"
        "\"connections\":{\"classic_raw\":%u,\"classic_ready\":%u,"
        "\"ble_raw\":%u,\"ble_ready\":%u},"
        "\"pairing\":{\"window_open\":%s,\"close_deferred\":%s,"
        "\"lockout\":%s},"
        "\"cble\":{\"available\":%s,\"armed\":%s,\"advertising\":%s,"
        "\"client\":%s,\"closing\":%s,\"notify\":%s,"
        "\"fresh_bond\":%s},"
        "\"events\":{\"count\":%u,\"dropped\":%lu},"
        "\"scan\":{\"starts\":%lu,\"stops\":%lu},"
        "\"adv\":{\"starts\":%lu,\"stops\":%lu},"
        "\"suppress\":{\"config_mode\":%lu,\"mgmt_armed\":%lu,\"wake\":%lu,"
        "\"other\":%lu},"
        "\"mgmt\":{\"connects\":%lu,\"disconnects\":%lu},"
        "\"admission\":{\"fresh_accepted\":%lu,\"reject_window\":%lu,"
        "\"reject_lockout\":%lu},"
        "\"enc\":{\"deferrals\":%lu,\"peer_completed\":%lu,\"collisions\":%lu,"
        "\"unencrypted_active\":%lu},"
        "\"auth\":{\"deferrals\":%lu,\"collisions\":%lu},"
        "\"clink\":{\"handle\":\"0x%04X\",\"refused_no_mgmt\":%lu,"
        "\"mgmt_teardowns\":%lu},"
        "\"wipe_completions\":%lu,"
        "\"disc\":{\"ctrl\":%lu,\"hci\":%lu,\"state_losses\":%lu,"
        "\"last_handle\":\"0x%04X\","
        "\"last_reason\":\"0x%02X\"},"
        "\"owner_led\":{\"reason\":\"%s\",\"on\":%s,"
        "\"last_transition_ms\":%lu,\"timer_max_gap_ms\":%lu}}",
        d.mgmt_enabled ? "true" : "false", d.config_mode ? "true" : "false",
        trace_personality_name(d.personality), d.powered_on ? "true" : "false",
        d.hid_state, d.scan_active ? "true" : "false",
        d.inquiry_active ? "true" : "false", d.wake_adv_active ? "true" : "false",
        d.controller_connected ? "true" : "false",
        d.connected_classic_count, d.ready_classic_count,
        d.connected_ble_count, d.ready_ble_count,
        d.pairing_window_open ? "true" : "false",
        d.pairing_close_deferred ? "true" : "false",
        d.pairing_lockout ? "true" : "false",
        d.cble_service_available ? "true" : "false",
        d.cble_mode_active ? "true" : "false",
        d.cble_advertising ? "true" : "false", d.cble_has_client ? "true" : "false",
        d.cble_closing ? "true" : "false", d.cble_notifications ? "true" : "false",
        d.cble_fresh_bond_admitted ? "true" : "false",
        d.event_count, (unsigned long)d.event_dropped,
        (unsigned long)d.scan_starts, (unsigned long)d.scan_stops,
        (unsigned long)d.adv_starts, (unsigned long)d.adv_stops,
        (unsigned long)d.suppress_config_mode, (unsigned long)d.suppress_mgmt_armed,
        (unsigned long)d.suppress_wake, (unsigned long)d.suppress_other,
        (unsigned long)d.mgmt_connects, (unsigned long)d.mgmt_disconnects,
        (unsigned long)d.fresh_admission_accepts,
        (unsigned long)d.fresh_admission_reject_window,
        (unsigned long)d.fresh_admission_reject_lockout,
        (unsigned long)d.classic_encryption_deferrals,
        (unsigned long)d.classic_encryption_peer_completed,
        (unsigned long)d.classic_encryption_collisions,
        (unsigned long)d.classic_encryption_unencrypted_active,
        (unsigned long)d.classic_authentication_deferrals,
        (unsigned long)d.classic_authentication_collisions,
        d.classic_companion_handle,
        (unsigned long)d.classic_companion_refused_no_mgmt,
        (unsigned long)d.classic_companion_mgmt_teardowns,
        (unsigned long)d.wipe_completions,
        (unsigned long)d.ctrl_disconnects, (unsigned long)d.hci_disconnects,
        (unsigned long)d.hci_state_losses,
        d.last_disc_handle, d.last_disc_reason,
        ns2_owner_led_reason_name(
            (ns2_owner_led_reason_t)d.owner_led_reason),
        d.owner_led_output_on ? "true" : "false",
        (unsigned long)d.owner_led_last_transition_ms,
        (unsigned long)d.owner_led_timer_max_gap_ms);
    // Say so rather than emitting half an object: a truncated btstate is
    // indistinguishable from a corrupt link at the reader, and this snapshot is
    // what a soak run judges every cycle by.
    if (btstate_len < 0 || (size_t)btstate_len >= sizeof(trace_format_response)) {
        queue_text("{\"btstate\":\"error\",\"error\":\"response truncated\"}");
        return;
    }
    queue_text(trace_format_response);
}

static void queue_bthealth(void) {
    btstack_host_mgmt_diag_t bt;
    ns2_bt_recovery_runtime_diag_t runtime;
    btstack_host_get_mgmt_diag(&bt);
    ns2_bt_recovery_runtime_get_diag(&runtime);
    snprintf(trace_format_response, sizeof(trace_format_response),
        "{\"bthealth\":\"status\",\"core1\":{\"sequence\":%lu,"
        "\"age_ms\":%lu,\"control_tick_age_ms\":%lu,"
        "\"control_tick_max_gap_ms\":%lu},"
        "\"hci\":{\"state\":%u,\"phase\":\"%s\",\"last_event_age_ms\":%lu,"
        "\"probe_handle\":\"0x%04X\",\"probes\":{\"sent\":%lu,"
        "\"ok\":%lu,\"failed\":%lu,\"timeouts\":%lu},"
        "\"recovery\":{\"attempts\":%lu,\"completions\":%lu}},"
        "\"reboot\":{\"pending\":%s,\"suppressed\":%s,"
        "\"requests\":%lu,\"consecutive_boots\":%u,\"last_boot_cause\":%u},"
        // What the radio was doing when the PREVIOUS boot escalated. Survives
        // the reboot in watchdog scratch; "valid":false means this boot was not
        // entered through recovery.
        "\"last_escalation\":{\"valid\":%s,\"phase\":\"%s\","
        "\"probes_sent\":%u,\"probe_failures\":%u,\"recovery_attempts\":%u,"
        "\"uptime_s\":%u,\"pairing_window\":%s,\"mgmt_client\":%s,"
        "\"classic_link\":%s,\"ble_link\":%s,\"discovery\":%s}}",
        (unsigned long)runtime.core1_heartbeat_sequence,
        (unsigned long)runtime.core1_heartbeat_age_ms,
        (unsigned long)runtime.control_tick_age_ms,
        (unsigned long)runtime.control_tick_max_gap_ms,
        bt.hci_state,
        ns2_bt_health_phase_name((ns2_bt_health_phase_t)bt.hci_health_phase),
        (unsigned long)bt.hci_last_event_age_ms, bt.hci_probe_handle,
        (unsigned long)bt.hci_probes_sent, (unsigned long)bt.hci_probes_ok,
        (unsigned long)bt.hci_probes_failed,
        (unsigned long)bt.hci_probe_timeouts,
        (unsigned long)bt.hci_recovery_attempts,
        (unsigned long)bt.hci_recovery_completions,
        runtime.reboot_pending ? "true" : "false",
        runtime.reboot_suppressed ? "true" : "false",
        (unsigned long)runtime.reboot_requests,
        runtime.consecutive_recovery_boots, runtime.last_boot_cause,
        runtime.last_escalation.valid ? "true" : "false",
        ns2_bt_health_phase_name(
            (ns2_bt_health_phase_t)runtime.last_escalation.phase),
        runtime.last_escalation.probes_sent,
        runtime.last_escalation.probe_failures,
        runtime.last_escalation.recovery_attempts,
        runtime.last_escalation.uptime_s,
        runtime.last_escalation.pairing_window_open ? "true" : "false",
        runtime.last_escalation.management_client ? "true" : "false",
        runtime.last_escalation.classic_link ? "true" : "false",
        runtime.last_escalation.ble_link ? "true" : "false",
        runtime.last_escalation.discovery_active ? "true" : "false");
    queue_text(trace_format_response);
}

// Bond inventory, read from the core-1 snapshot. `bonds list` reaches the same
// data but only over CDC or the BLE management bridge, and neither is available
// while the adapter's USB-C is on the console -- which is exactly when a
// reconnect test runs. Bounded to the LE device DB capacity.
static void queue_btbonds(void) {
    size_t n = 0;
    n += (size_t)snprintf(trace_format_response, sizeof(trace_format_response),
                          "{\"btbonds\":[");
    for (uint8_t i = 0; i < 16u && n < sizeof(trace_format_response); i++) {
        btstack_host_bond_entry_t e;
        if (!btstack_host_bond_snapshot_get(i, &e)) break;
        n += (size_t)snprintf(trace_format_response + n, sizeof(trace_format_response) - n,
                              "%s{\"i\":%u,\"type\":%u,"
                              "\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
                              i ? "," : "", i, e.addr_type,
                              e.addr[0], e.addr[1], e.addr[2],
                              e.addr[3], e.addr[4], e.addr[5]);
    }
    if (n < sizeof(trace_format_response))
        snprintf(trace_format_response + n, sizeof(trace_format_response) - n, "]}");
    queue_text(trace_format_response);
}

// One lifecycle event (0 = oldest). `a` is the scan-suppress cause (see `cause`)
// for scan_suppress, else the HCI disconnect reason / 0|1 config-vs-mgmt tag.
static void queue_btlife(uint16_t index) {
    btstack_host_life_record_t e;
    if (!btstack_host_life_get(index, &e)) {
        queue_text("{\"btlife\":\"empty\"}");
        return;
    }
    char flags[9];
    btstack_host_life_flag_names(e.flags, flags);
    char response[256];
    snprintf(response, sizeof(response),
        "{\"btlife\":\"record\",\"i\":%u,\"t_ms\":%lu,\"code\":\"%s\","
        "\"cause\":\"%s\",\"a\":%u,\"handle\":\"0x%04X\","
        "\"radio\":\"%s\",\"addr\":\"%02X:%02X:%02X\"}",
        index, (unsigned long)e.t_ms, btstack_host_life_code_name(e.code),
        btstack_host_life_cause_name(e.a), e.a, e.b,
        flags, e.addr3[0], e.addr3[1], e.addr3[2]);
    queue_text(response);
}

// Bulk read. The ring holds 1024 entries so a soak can be reconstructed after
// the fact; one record per UART round trip would take about twenty minutes to
// drain, which is not a usable diagnostic. Emits a compact positional array --
// [t_ms, code, a, handle, radio, addr] -- for up to BTLIFE_DUMP_SPAN entries.
#define BTLIFE_DUMP_SPAN 24u
static void queue_btlife_dump(uint16_t first) {
    int n = snprintf(trace_format_response, sizeof(trace_format_response),
                     "{\"btlife\":\"dump\",\"first\":%u,\"events\":[", first);
    uint16_t emitted = 0;
    for (uint16_t i = 0; i < BTLIFE_DUMP_SPAN; i++) {
        btstack_host_life_record_t e;
        if (!btstack_host_life_get((uint16_t)(first + i), &e)) break;
        char flags[9];
        btstack_host_life_flag_names(e.flags, flags);
        int written = snprintf(
            trace_format_response + n, sizeof(trace_format_response) - (size_t)n,
            "%s[%lu,\"%s\",%u,%u,\"%s\",\"%02X%02X%02X\"]",
            emitted ? "," : "", (unsigned long)e.t_ms,
            btstack_host_life_code_name(e.code), e.a, e.b, flags,
            e.addr3[0], e.addr3[1], e.addr3[2]);
        if (written < 0 || (size_t)(n + written) >= sizeof(trace_format_response) - 32u) break;
        n += written;
        emitted++;
    }
    snprintf(trace_format_response + n, sizeof(trace_format_response) - (size_t)n,
             "],\"count\":%u}", emitted);
    queue_text(trace_format_response);
}

static void queue_nfc_mirror_status(const char *event) {
    ns2_nfc_mirror_diag_t status;
    char response[512];
    ns2_nfc_mirror_snapshot(&status);
    snprintf(response, sizeof(response),
             "{\"nfcmirror\":\"%s\",\"requested\":%s,\"active\":%s,"
             "\"initiator\":%s,\"reply_ready\":%s,"
             "\"pending\":%s,\"awaiting_response\":%s,\"state\":%u,"
             "\"att_status\":%u,"
             "\"send_status\":%u,\"source_pid\":\"0x%04X\","
             "\"handle\":\"0x%04X\",\"last_command\":%u,\"last_sub\":%u,"
             "\"report_state\":%u,\"last_response_length\":%u,"
             "\"submitted\":%lu,\"sent\":%lu,\"notifications\":%lu,"
             "\"state_transitions\":%lu,\"timeouts\":%lu,\"rejected\":%lu}",
             event, status.requested ? "true" : "false",
             status.active ? "true" : "false",
             status.initiator ? "true" : "false",
             status.response_ready ? "true" : "false",
             status.command_pending ? "true" : "false",
             status.awaiting_response ? "true" : "false", status.state,
             status.last_att_status, status.last_send_status,
             status.source_pid, status.connection_handle,
             status.last_command, status.last_subcommand,
             status.report_state, status.last_response_length,
             (unsigned long)status.commands_submitted,
             (unsigned long)status.commands_sent,
             (unsigned long)status.notifications,
             (unsigned long)status.report_state_transitions,
             (unsigned long)status.response_timeouts,
             (unsigned long)status.rejected);
    queue_text(response);
}

static void queue_amiibo_status(const char *event) {
    virtual_amiibo_status_t status;
    char response[384];
    virtual_amiibo_store_status(&status);
    snprintf(response, sizeof(response),
             "{\"amiibo\":\"%s\",\"loaded\":%s,\"dirty\":%s,"
             "\"persisted\":%s,\"persist_pending\":%s,"
             "\"has_signature\":%s,\"has_used\":%s,\"using_used\":%s,"
             "\"size\":%u,"
             "\"generation\":%lu,\"payload_crc\":\"%08lX\",\"v3loaded\":%s,"
             "\"uid\":\"%02X%02X%02X%02X%02X%02X%02X\"}",
             event, status.loaded ? "true" : "false",
             status.dirty ? "true" : "false",
             status.persisted ? "true" : "false",
             virtual_amiibo_store_persist_pending() ? "true" : "false",
             status.has_originality_signature ? "true" : "false",
             status.has_used_copy ? "true" : "false",
             status.using_used_copy ? "true" : "false",
             status.size, (unsigned long)status.generation,
             (unsigned long)status.payload_crc,
             status.v3_loaded ? "true" : "false",
             status.uid[0], status.uid[1], status.uid[2], status.uid[3],
             status.uid[4], status.uid[5], status.uid[6]);
    queue_text(response);
}

static void queue_amiibo_read(uint16_t offset) {
    virtual_amiibo_status_t before;
    virtual_amiibo_status_t after;
    uint8_t data[NS2_UART_AMIIBO_READ_MAX];
    char payload[NS2_UART_AMIIBO_READ_MAX * 2u + 1u];
    char response[384];
    virtual_amiibo_store_status(&before);
    if ((!before.loaded && !before.v3_loaded) || offset >= before.size) {
        snprintf(response, sizeof(response),
                 "{\"amiibo\":\"error\",\"error\":\"read out of range\","
                 "\"offset\":%u,\"total\":%u}",
                 offset, before.size);
        queue_text(response);
        return;
    }

    size_t length = before.size - offset;
    if (length > sizeof(data)) length = sizeof(data);
    const virtual_amiibo_result_t result = before.v3_loaded
        ? virtual_amiibo_store_v3_read(offset, data, length)
        : virtual_amiibo_store_read(offset, data, length);
    virtual_amiibo_store_status(&after);
    if (result != VIRTUAL_AMIIBO_OK ||
        (!after.loaded && !after.v3_loaded) ||
        after.v3_loaded != before.v3_loaded ||
        after.size != before.size ||
        after.generation != before.generation) {
        snprintf(response, sizeof(response),
                 "{\"amiibo\":\"error\",\"error\":\"%s\","
                 "\"offset\":%u,\"generation\":%lu}",
                 result == VIRTUAL_AMIIBO_OK ? "image changed during read" :
                     virtual_amiibo_result_string(result),
                 offset, (unsigned long)after.generation);
        queue_text(response);
        return;
    }

    for (size_t i = 0; i < length; ++i)
        snprintf(&payload[i * 2u], 3, "%02X", data[i]);
    payload[length * 2u] = '\0';
    snprintf(response, sizeof(response),
             "{\"amiibo\":\"data\",\"offset\":%u,\"length\":%u,"
             "\"total\":%u,\"generation\":%lu,\"payload\":\"%s\"}",
             offset, (unsigned)length, before.size,
             (unsigned long)before.generation, payload);
    queue_text(response);
}

static void queue_motion_pair_status(const char *event) {
    ds5_motion_chart_trigger_t chart;
    ds5_motion_pair_chart_status(&chart);
    char response[384];
    snprintf(response, sizeof(response),
             "{\"motionpair\":\"%s\",\"enabled\":%s,\"count\":%u,"
             "\"capacity\":%u,\"dropped\":%lu,"
             "\"chart\":{\"armed\":%s,\"baseline_valid\":%s,"
             "\"triggered\":%s,\"complete\":%s,\"target_mask\":%u,"
             "\"baseline\":%u,"
             "\"transition\":%u,\"pre\":%u,\"post\":%u}}",
             event, ds5_motion_pair_get_enabled() ? "true" : "false",
             ds5_motion_pair_buffered_count(), DS5_MOTION_PAIR_CAPACITY,
             (unsigned long)ds5_motion_pair_dropped_count(),
             chart.armed ? "true" : "false",
             chart.baseline_valid ? "true" : "false",
             chart.triggered ? "true" : "false",
             chart.complete ? "true" : "false",
             chart.target_mask, chart.baseline_state, chart.trigger_state,
             chart.pre_records, chart.post_records);
    queue_text(response);
}

static void queue_motion_pair_record(void) {
    if (!ds5_motion_pair_drain_one(&motion_pair_format_record)) {
        queue_text("{\"motionpair\":\"empty\"}");
        return;
    }
    for (size_t i = 0; i < motion_pair_format_record.native_length; ++i)
        snprintf(&motion_pair_format_payload[i * 2u], 3, "%02X",
                 motion_pair_format_record.native[i]);
    motion_pair_format_payload[motion_pair_format_record.native_length * 2u] = '\0';

    uint32_t age_us = motion_pair_format_record.ds5_valid
        ? motion_pair_format_record.native_us - motion_pair_format_record.ds5_us
        : UINT32_MAX;
    snprintf(motion_pair_format_response, sizeof(motion_pair_format_response),
             "{\"motionpair\":\"record\",\"t_us\":%lu,"
             "\"native_len\":%u,\"native\":\"%s\","
             "\"ds5_valid\":%s,\"ds5_seq\":%lu,\"ds5_t_us\":%lu,"
             "\"ds5_age_us\":%lu,\"ds5_sensor\":%lu,\"cal_state\":%u,"
             "\"raw_g\":[%d,%d,%d],\"raw_a\":[%d,%d,%d],"
             "\"cal_g\":[%d,%d,%d],\"cal_a\":[%d,%d,%d]}",
             (unsigned long)motion_pair_format_record.native_us,
             motion_pair_format_record.native_length,
             motion_pair_format_payload,
             motion_pair_format_record.ds5_valid ? "true" : "false",
             (unsigned long)motion_pair_format_record.ds5_sequence,
             (unsigned long)motion_pair_format_record.ds5_us,
             (unsigned long)age_us,
             (unsigned long)motion_pair_format_record.ds5_sensor_timestamp,
             motion_pair_format_record.calibration_state,
             motion_pair_format_record.raw_gyro[0],
             motion_pair_format_record.raw_gyro[1],
             motion_pair_format_record.raw_gyro[2],
             motion_pair_format_record.raw_accel[0],
             motion_pair_format_record.raw_accel[1],
             motion_pair_format_record.raw_accel[2],
             motion_pair_format_record.calibrated_gyro[0],
             motion_pair_format_record.calibrated_gyro[1],
             motion_pair_format_record.calibrated_gyro[2],
             motion_pair_format_record.calibrated_accel[0],
             motion_pair_format_record.calibrated_accel[1],
             motion_pair_format_record.calibrated_accel[2]);
    queue_text(motion_pair_format_response);
}

static void queue_motion_hybrid_status(const char *event) {
    ns2_motion_hybrid_live_diag_t d;
    ns2_motion_hybrid_live_get_diag(&d);
    const uint32_t donor_age_us = d.source_last_us
        ? time_us_32() - d.source_last_us : UINT32_MAX;
    const char *last_reason = d.native_packets
        ? ns2_motion_hybrid_live_reason_name(
              (ns2_motion_hybrid_live_reason_t)d.last_reason)
        : "none";
    snprintf(motion_hybrid_format_response,
             sizeof(motion_hybrid_format_response),
             "{\"motionhybrid\":\"%s\",\"requested\":\"%s\","
             "\"active\":\"%s\",\"capture\":%s,\"count\":%u,"
             "\"capacity\":%u,\"dropped\":%lu,\"pose_aligned\":%s,"
             "\"cal_state\":%u,\"last_reason\":\"%s\","
             "\"last_length\":%u,\"last_groups\":%lu,"
             "\"last_changed_bits\":%u,\"last_ds5_age_us\":%lu,"
             "\"last_ds5_seq\":%lu,"
             "\"donor_seen\":%s,\"donor_age_us\":%lu,"
             "\"donor_seq\":%lu,\"donor_cal_state\":%u,"
             "\"packets\":{\"native\":%lu,\"hybrid\":%lu,"
             "\"genuine\":%lu,\"fallback\":%lu},"
             "\"saturation\":{\"accel\":%lu,\"gyro\":%lu},"
             "\"reasons\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu]}",
             event, ns2_motion_hybrid_mode_name(d.requested_mode),
             ns2_motion_hybrid_mode_name(d.active_mode),
             ns2_motion_hybrid_live_capture_get_enabled() ? "true" : "false",
             ns2_motion_hybrid_live_capture_count(),
             DS5_MOTION_PAIR_CAPACITY,
             (unsigned long)ns2_motion_hybrid_live_capture_dropped(),
             d.pose_aligned ? "true" : "false", d.calibration_state,
             last_reason,
             d.last_length, (unsigned long)d.last_groups,
             d.last_changed_bits, (unsigned long)d.last_ds5_age_us,
             (unsigned long)d.last_ds5_sequence,
             d.source_last_us ? "true" : "false",
             (unsigned long)donor_age_us,
             (unsigned long)d.source_sequence,
             d.source_calibration_state,
             (unsigned long)d.native_packets,
             (unsigned long)d.hybrid_packets,
             (unsigned long)d.genuine_controls,
             (unsigned long)d.fallback_packets,
             (unsigned long)d.saturated_accel,
             (unsigned long)d.saturated_gyro,
             (unsigned long)d.reasons[0], (unsigned long)d.reasons[1],
             (unsigned long)d.reasons[2], (unsigned long)d.reasons[3],
             (unsigned long)d.reasons[4], (unsigned long)d.reasons[5],
             (unsigned long)d.reasons[6], (unsigned long)d.reasons[7],
             (unsigned long)d.reasons[8], (unsigned long)d.reasons[9],
             (unsigned long)d.reasons[10], (unsigned long)d.reasons[11],
             (unsigned long)d.reasons[12]);
    queue_text(motion_hybrid_format_response);
}

static void queue_motion_hybrid_record(void) {
    if (!ns2_motion_hybrid_live_capture_drain(
            &motion_hybrid_format_record)) {
        queue_text("{\"motionhybrid\":\"empty\"}");
        return;
    }
    for (size_t i = 0; i < motion_hybrid_format_record.length; ++i) {
        snprintf(&motion_hybrid_base[i * 2u], 3, "%02X",
                 motion_hybrid_format_record.base[i]);
        snprintf(&motion_hybrid_xor[i * 2u], 3, "%02X",
                 motion_hybrid_format_record.output_xor[i]);
    }
    motion_hybrid_base[motion_hybrid_format_record.length * 2u] = '\0';
    motion_hybrid_xor[motion_hybrid_format_record.length * 2u] = '\0';
    snprintf(motion_hybrid_format_response,
             sizeof(motion_hybrid_format_response),
             "{\"motionhybrid\":\"record\",\"t_us\":%lu,"
             "\"native_len\":%u,\"mode\":\"%s\","
             "\"reason\":\"%s\",\"requested_groups\":%lu,"
             "\"changed_bits\":%u,\"ds5_age_us\":%lu,"
             "\"ds5_seq\":%lu,\"cal_state\":%u,"
             "\"pose_aligned\":%s,\"base\":\"%s\","
             "\"output_xor\":\"%s\"}",
             (unsigned long)motion_hybrid_format_record.native_us,
             motion_hybrid_format_record.length,
             ns2_motion_hybrid_mode_name(motion_hybrid_format_record.mode),
             ns2_motion_hybrid_live_reason_name(
                 (ns2_motion_hybrid_live_reason_t)
                     motion_hybrid_format_record.reason),
             (unsigned long)motion_hybrid_format_record.requested_groups,
             motion_hybrid_format_record.changed_bits,
             (unsigned long)motion_hybrid_format_record.ds5_age_us,
             (unsigned long)motion_hybrid_format_record.ds5_sequence,
             motion_hybrid_format_record.calibration_state,
             motion_hybrid_format_record.pose_aligned ? "true" : "false",
             motion_hybrid_base, motion_hybrid_xor);
    queue_text(motion_hybrid_format_response);
}

static bool parse_motion_hybrid_mode(const char *name, uint8_t *mode) {
    if (!name || !mode) return false;
    for (uint8_t candidate = 0;
         candidate < NS2_MOTION_HYBRID_MODE_COUNT; ++candidate) {
        if (strcmp(name, ns2_motion_hybrid_mode_name(candidate)) == 0) {
            *mode = candidate;
            return true;
        }
    }
    return false;
}

static void queue_motion_probe_status(const char *event) {
    ns2_motion_probe_status_t d;
    ns2_motion_probe_get_status(&d);
    snprintf(trace_format_response, sizeof(trace_format_response),
             "{\"motionprobe\":\"%s\",\"latched\":%s,\"enabled\":%s,"
             "\"g\":[%lu,%lu,%lu],\"baseline\":[%lu,%lu,%lu],"
             "\"rate\":[%ld,%ld,%ld],\"updates\":%lu}",
             event, d.latched ? "true" : "false", d.enabled ? "true" : "false",
             (unsigned long)d.orientation[0], (unsigned long)d.orientation[1],
             (unsigned long)d.orientation[2], (unsigned long)d.baseline[0],
             (unsigned long)d.baseline[1], (unsigned long)d.baseline[2],
             (long)d.rate[0], (long)d.rate[1], (long)d.rate[2],
             (unsigned long)d.updates);
    queue_text(trace_format_response);
}

static void queue_pcm_status(const char *event) {
    ds5_audio_pcm_capture_status_t status;
    ds5_audio_pcm_capture_get_status(&status);
    snprintf(pcm_format_response, sizeof(pcm_format_response),
             "{\"pro2audio_pcm\":\"%s\",\"armed\":%s,\"complete\":%s,"
             "\"captured\":%u,\"capacity\":%u,\"packets\":%u,"
             "\"start_us\":%lu,\"end_us\":%lu,\"frames\":%u,"
             "\"crc32\":\"%08lX\",\"peak_l\":%u,\"peak_r\":%u,"
             "\"sum_l\":%lld,\"sum_r\":%lld,"
             "\"sum_sq_l\":%llu,\"sum_sq_r\":%llu}",
             event, status.armed ? "true" : "false",
             status.complete ? "true" : "false", status.captured_bytes,
             status.capacity_bytes, status.packets,
             (unsigned long)status.start_us, (unsigned long)status.end_us,
             status.captured_bytes / 4u, (unsigned long)status.crc32,
             status.peak_left, status.peak_right,
             (long long)status.sum_left, (long long)status.sum_right,
             (unsigned long long)status.sum_squares_left,
             (unsigned long long)status.sum_squares_right);
    queue_text(pcm_format_response);
}

static void queue_pcm_record(uint16_t offset) {
    ds5_audio_pcm_capture_status_t status;
    ds5_audio_pcm_capture_get_status(&status);
    if (status.armed) {
        queue_text("{\"pro2audio_pcm\":\"error\",\"error\":\"capture still armed\"}");
        return;
    }
    uint16_t const length = ds5_audio_pcm_capture_read(
        offset, pcm_format_data, sizeof(pcm_format_data));
    if (length == 0) {
        queue_text("{\"pro2audio_pcm\":\"error\",\"error\":\"offset out of range\"}");
        return;
    }
    for (uint16_t i = 0; i < length; ++i)
        snprintf(&pcm_format_payload[i * 2u], 3, "%02X", pcm_format_data[i]);
    pcm_format_payload[length * 2u] = '\0';
    snprintf(pcm_format_response, sizeof(pcm_format_response),
             "{\"pro2audio_pcm\":\"data\",\"offset\":%u,\"length\":%u,"
             "\"total\":%u,\"payload\":\"%s\"}",
             offset, length, status.captured_bytes, pcm_format_payload);
    queue_text(pcm_format_response);
}

// Parse an even-length hex string into bytes. Local to the diagnostic channel
// rather than shared with config.c's parser: this channel deliberately reaches
// for subsystem APIs, not for the config command surface. (config.c itself is
// in every image -- CMakeLists globs src/*.c -- and this file is the optional
// one, guarded by NS2_UART_DIAG, so the one dependency it does take,
// config_request_save(), cannot break a -NoUartDiag build.)
static void queue_amiibo_result(virtual_amiibo_result_t result)
{
    if (result == VIRTUAL_AMIIBO_OK) {
        queue_text("{\"amiibo\":\"ok\"}");
    } else {
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"amiibo\":\"error\",\"code\":%d}", (int)result);
        queue_text(trace_format_response);
    }
}

static bool diag_parse_hex(const char *hex, uint8_t *out, size_t capacity,
                           size_t *length)
{
    size_t n = 0;
    while (*hex == ' ') hex++;
    while (hex[0] != 0 && hex[0] != ' ') {
        if (hex[1] == 0 || n >= capacity) return false;
        int high = -1, low = -1;
        for (int pass = 0; pass < 2; ++pass) {
            const char c = hex[pass];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return false;
            if (pass == 0) high = v; else low = v;
        }
        out[n++] = (uint8_t)((high << 4) | low);
        hex += 2;
    }
    *length = n;
    return n != 0;
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
    } else if (strcmp(rx_line, "rumble") == 0 ||
               strcmp(rx_line, "rumble status") == 0) {
        // Firmware-side end-to-end rumble trace. Read this BEFORE touching any
        // amplitude: it separates "the console never asked", "we decoded it but
        // never handed it to the bridge", and "we sent it and the handheld is
        // still silent" -- three different bugs. See include/ns2_rumble_trace.h.
        ns2_rumble_trace_t r;
        ns2_rumble_trace_get(&r);
        snprintf(rumble_format_response, sizeof(rumble_format_response),
                 "{\"rumble\":{\"console\":{\"reports\":%lu,\"nonzero\":%lu,"
                 "\"last\":[%u,%u]},\"bridge\":{\"sent\":%lu,\"failed\":%lu,"
                 "\"nonzero\":%lu,\"last\":[%u,%u],\"player\":%u,"
                 "\"motion_wanted\":%u}}}",
                 (unsigned long)r.console_reports,
                 (unsigned long)r.console_nonzero,
                 r.console_left, r.console_right,
                 (unsigned long)r.bridge_sent,
                 (unsigned long)r.bridge_failed,
                 (unsigned long)r.bridge_nonzero,
                 r.bridge_left, r.bridge_right, r.bridge_player,
                 r.bridge_motion_wanted);
        queue_text(rumble_format_response);
    } else if (strcmp(rx_line, "bridge") == 0 ||
               strcmp(rx_line, "bridge status") == 0) {
        // Direct answer to "did the adapter recognize the companion bridge".
        // Battery, motion, rumble and the player LED are ALL gated on one exact
        // descriptor match, so a v2 feature loss with working buttons means this
        // returned false. Read it here instead of inferring it from missing
        // feedback, which cannot tell "never called" from "rejected" from
        // "matched but the console asked for nothing".
        const android_bridge_identify_trace_t *t = android_bridge_identify_trace();
        snprintf(rumble_format_response, sizeof(rumble_format_response),
                 "{\"bridge_identify\":{\"contract\":%u,\"build\":\"%s\","
                 "\"calls\":%lu,\"matched\":%lu,"
                 "\"rejected\":{\"null\":%lu,\"length\":%lu,\"content\":%lu},"
                 "\"last_len\":%u,\"expected_len\":%u,\"first_mismatch\":%ld,"
                 "\"expected_byte\":%u,\"actual_byte\":%u,\"profile\":\"%s\","
                 "\"suspected_skew\":%s}}",
                 (unsigned)ANDROID_BRIDGE_CONTRACT_VERSION,
                 PICOSWITCH_BUILD_ID,
                 (unsigned long)t->calls,
                 (unsigned long)t->matched,
                 (unsigned long)t->rejected_null,
                 (unsigned long)t->rejected_length,
                 (unsigned long)t->rejected_content,
                 t->last_len, t->expected_len,
                 (long)t->first_mismatch,
                 t->expected_byte, t->actual_byte,
                 t->active_profile == 2u ? "v2-bridge"
                     : (t->active_profile == 1u ? "v1-generic" : "none"),
                 // Bounded, evidence-based: a peer that presented a descriptor of
                 // exactly the expected LENGTH but different CONTENT is almost
                 // certainly this bridge built against a different contract, not
                 // an unrelated gamepad that happens to be 161 bytes. Reported as
                 // a suspicion, never used to authorize anything.
                 (t->rejected_content > 0u && t->last_len == t->expected_len)
                     ? "true" : "false");
        queue_text(rumble_format_response);
    } else if (strcmp(rx_line, "bridge clear") == 0) {
        android_bridge_identify_trace_reset();
        queue_text("{\"ok\":true,\"cleared\":\"bridge\"}");
    } else if (strcmp(rx_line, "rumble clear") == 0) {
        ns2_rumble_trace_reset();
        queue_text("{\"ok\":true,\"cleared\":\"rumble\"}");
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
    } else if (strcmp(rx_line, "trace start nfc") == 0) {
        ns2_protocol_trace_set_filter(NS2_TRACE_FILTER_NFC);
        ns2_protocol_trace_set_enabled(true);
        queue_trace_status("started");
    } else if (strcmp(rx_line, "trace start bulk") == 0) {
        ns2_protocol_trace_set_filter(NS2_TRACE_FILTER_BULK);
        ns2_protocol_trace_set_enabled(true);
        queue_trace_status("started");
    } else if (strcmp(rx_line, "trace start") == 0) {
        ns2_protocol_trace_set_filter(NS2_TRACE_FILTER_ALL);
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
    } else if (strcmp(rx_line, "blecap nfc start") == 0) {
        sw2_capture_set_filter(SW2_CAPTURE_FILTER_NFC);
        sw2_capture_set_enabled(true);
        queue_ble_status("started");
    } else if (strcmp(rx_line, "blecap start") == 0) {
        sw2_capture_set_filter(SW2_CAPTURE_FILTER_ALL);
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
    } else if (strcmp(rx_line, "btstate") == 0 ||
               strcmp(rx_line, "btlife") == 0 ||
               strcmp(rx_line, "btlife status") == 0) {
        // Live BLE/management coexistence snapshot + suppression counters.
        queue_btstate();
    } else if (strcmp(rx_line, "btreject") == 0) {
        // Attribution for the most recent Classic admission rejection. The
        // reject counters in `btstate` are anonymous; this says WHICH peer was
        // rejected and what the trust lookup returned for it, which is the
        // difference between "something was rejected" and evidence.
        uint8_t a[6]; bool trust = false;
        if (!btstack_host_last_reject(a, &trust)) {
            queue_text("{\"btreject\":\"none\"}");
        } else {
            snprintf(trace_format_response, sizeof(trace_format_response),
                "{\"btreject\":\"last\",\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                "\"trust_present\":%s}",
                a[0], a[1], a[2], a[3], a[4], a[5], trust ? "true" : "false");
            queue_text(trace_format_response);
        }
    } else if (strcmp(rx_line, "btrefuse") == 0) {
        // Which term of the Controller-Link admission predicate was false.
        // `clink.refused_no_mgmt` counts refusals; this says why the last one
        // happened, which is the difference between a number and a diagnosis.
        btstack_host_link_refusal_t r;
        if (!btstack_host_last_link_refusal(&r)) {
            queue_text("{\"btrefuse\":\"none\"}");
        } else {
            snprintf(trace_format_response, sizeof(trace_format_response),
                "{\"btrefuse\":\"last\",\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                "\"cross_transport\":%s,\"mgmt_connected\":%s,"
                "\"mgmt_addr_known\":%s,\"mgmt_raw_matches\":%s,"
                "\"mgmt_identity_known\":%s,\"mgmt_identity_matches\":%s,"
                "\"mgmt_link_trusted\":%s}",
                r.addr[0], r.addr[1], r.addr[2], r.addr[3], r.addr[4], r.addr[5],
                r.cross_transport ? "true" : "false",
                r.mgmt_connected ? "true" : "false",
                r.mgmt_addr_known ? "true" : "false",
                r.mgmt_raw_matches ? "true" : "false",
                r.mgmt_identity_known ? "true" : "false",
                r.mgmt_identity_matches ? "true" : "false",
                r.mgmt_link_trusted ? "true" : "false");
            queue_text(trace_format_response);
        }
    } else if (strcmp(rx_line, "btauth") == 0) {
        // Why the last Classic Authentication Complete did or did not stand
        // down from BTstack's automatic encryption request.
        btstack_host_auth_decision_t a;
        if (!btstack_host_last_auth_decision(&a)) {
            queue_text("{\"btauth\":\"none\"}");
        } else {
            snprintf(trace_format_response, sizeof(trace_format_response),
                "{\"btauth\":\"last\",\"handle\":\"0x%04X\","
                "\"mgmt_connected\":%s,\"mgmt_addr_known\":%s,"
                "\"mgmt_addr_matches\":%s,\"mgmt_link_trusted\":%s,"
                "\"we_own_fresh_pairing\":%s,\"request_was_pending\":%s,"
                "\"stood_down\":%s,"
                "\"auth_deferred\":%s,\"auth_had_stored_key\":%s,"
                "\"auth_outcome\":\"%s\",\"encrypted_ok\":%s,"
                "\"key_size\":%u,\"key_size_valid\":%s,\"hid_ready\":%s,"
                "\"security_ok\":%s,\"link_closed\":%s,"
                "\"auth_deferrals\":%lu}",
                a.handle,
                a.mgmt_connected ? "true" : "false",
                a.mgmt_addr_known ? "true" : "false",
                a.mgmt_addr_matches ? "true" : "false",
                a.mgmt_link_trusted ? "true" : "false",
                a.we_own_fresh_pairing ? "true" : "false",
                a.request_was_pending ? "true" : "false",
                a.stood_down ? "true" : "false",
                a.auth_deferred ? "true" : "false",
                a.auth_had_stored_key ? "true" : "false",
                ns2_bt_auth_observation_name(a.auth_outcome),
                a.encrypted_ok ? "true" : "false",
                a.encryption_key_size,
                a.key_size_valid ? "true" : "false",
                a.hid_ready ? "true" : "false",
                ns2_bt_companion_security_satisfied(
                    a.auth_outcome, a.encrypted_ok, a.key_size_valid,
                    a.encryption_key_size, a.hid_ready) ? "true" : "false",
                a.link_closed ? "true" : "false",
                (unsigned long)btstack_host_authentication_deferrals());
            queue_text(trace_format_response);
        }
    } else if (strcmp(rx_line, "bthealth") == 0) {
        queue_bthealth();
    } else if (strcmp(rx_line, "btbonds") == 0) {
        queue_btbonds();
    } else if (strcmp(rx_line, "btlife clear") == 0) {
        btstack_host_life_clear();
        queue_text("{\"btlife\":\"cleared\"}");
    } else if (strncmp(rx_line, "btlife dump ", 12) == 0) {
        unsigned int first;
        char trailing;
        if (sscanf(rx_line + 12, "%u%c", &first, &trailing) != 1 ||
            first > UINT16_MAX) {
            queue_text("{\"btlife\":\"error\",\"error\":\"usage: btlife dump N\"}");
        } else {
            queue_btlife_dump((uint16_t)first);
        }
    } else if (strncmp(rx_line, "btlife read ", 12) == 0) {
        unsigned int index;
        char trailing;
        if (sscanf(rx_line + 12, "%u%c", &index, &trailing) != 1 ||
            index > UINT16_MAX) {
            queue_text("{\"btlife\":\"error\",\"error\":\"usage: btlife read N\"}");
        } else {
            queue_btlife((uint16_t)index);
        }
    } else if (strcmp(rx_line, "expmode") == 0 ||
               strcmp(rx_line, "expmode status") == 0 ||
               strcmp(rx_line, "expmode inquiry on") == 0 ||
               strcmp(rx_line, "expmode inquiry off") == 0) {
        // EXPERIMENT ONLY. Runtime arm selection for the inquiry-suppression
        // A/B comparison, so both arms run from ONE binary against ONE pairing
        // and neither build, flash, nor bond is a variable in the result.
        //
        // arm A = production behaviour, unchanged.
        // arm B = Classic inquiry RESTARTS postponed between page acceptance
        //         and HCI Connection Complete. Nothing else.
        //
        // The arm is echoed on every query so a run's artifacts state which arm
        // produced them rather than depending on the operator's notes.
        if (strcmp(rx_line, "expmode inquiry on") == 0) {
            btstack_host_set_experiment_inquiry_suppression(true);
        } else if (strcmp(rx_line, "expmode inquiry off") == 0) {
            btstack_host_set_experiment_inquiry_suppression(false);
        }
        bool on = btstack_host_experiment_inquiry_suppression();
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"expmode\":\"status\",\"experiment\":\"inquiry-suppression\","
                 "\"inquiry_suppression\":%s,\"arm\":\"%s\"}",
                 on ? "true" : "false",
                 on ? "B-experimental" : "A-production");
        queue_text(trace_format_response);
    } else if (strncmp(rx_line, "expmode", 7) == 0) {
        queue_text("{\"expmode\":\"error\",\"error\":"
                   "\"usage: expmode [status|inquiry on|inquiry off]\"}");
    } else if (strcmp(rx_line, "pipe") == 0) {
        // core0 report pipeline vs. BT input freshness. reportAgeMs large =>
        // report loop stalled; reportAgeMs small + inputAgeMs large => BT stopped
        // feeding input (frozen/neutral to console). See the overnight investigation.
        char response[192];
        snprintf(response, sizeof(response),
                 "{\"pipe\":\"status\",\"reportCount\":%lu,\"reportAgeMs\":%lu,"
                 "\"inputAgeMs\":%lu}",
                 (unsigned long)ns2_pro2_report_count(),
                 (unsigned long)ns2_pro2_last_report_age_ms(),
                 (unsigned long)report_input_age_ms(0));
        queue_text(response);
    } else if (strcmp(rx_line, "mgmt") == 0 || strcmp(rx_line, "mgmt status") == 0) {
        char response[64];
        snprintf(response, sizeof(response), "{\"mgmt\":\"status\",\"enabled\":%s}",
                 g_mgmt_enabled ? "true" : "false");
        queue_text(response);
    } else if (strcmp(rx_line, "mgmt on") == 0) {
        g_mgmt_enabled = true;   // dev-only toggle over UART (no config mode needed)
        queue_text("{\"mgmt\":\"on\"}");
    } else if (strcmp(rx_line, "mgmt off") == 0) {
        g_mgmt_enabled = false;
        queue_text("{\"mgmt\":\"off\"}");
    } else if (strcmp(rx_line, "bootsel") == 0) {
        // Dev-only: reboot into the USB mass-storage bootloader so a build can
        // be flashed without physically holding BOOTSEL.
        //
        // Why this exists. Validating the Bluetooth lifecycle means soak runs of
        // hundreds of connect/disconnect cycles, and every code change between
        // runs otherwise costs a physical button press on the adapter. That made
        // the mandated soak workloads (see tools/controller_link_cycle.py)
        // impossible to run unattended, which is a worse outcome than exposing a
        // reboot on a wired debug header that already offers `persona` and
        // `mgmt off`. Same reasoning as the `persona` command below.
        //
        // Never reachable from the console, the companion, or any wireless
        // transport -- UART only, and only in NS2_UART_DIAG builds.
        // Written directly rather than through queue_text(): the queue is
        // drained by ns2_uart_diag_task() with a per-tick budget, and this
        // command never returns to that task.
        {
            static const char ack[] = "{\"bootsel\":\"entering\"}\r\n";
            for (size_t i = 0; i < sizeof(ack) - 1u; i++) {
                uart_putc_raw(NS2_UART_ID, ack[i]);
            }
            uart_tx_wait_blocking(NS2_UART_ID);
        }
        reset_usb_boot(0, 0);
    } else if (strncmp(rx_line, "persona ", 8) == 0) {
        // Dev-only: trigger a personality transition over UART (removes the
        // physical-BOOTSEL / config-mode dependency for future investigations).
        // Controller targets use the app-request flag; `config` toggles CDC Config
        // via the 2s-hold flag (the app path forbids CDC, so a dedicated toggle is
        // used). Consumed at the safe loop point in usb_core_task. See task 12.
        const char *t = rx_line + 8;
        if (strcmp(t, "config") == 0) {
            g_usb_config_mode_requested = true;
            queue_text("{\"persona\":\"ok\",\"target\":\"config-toggle\"}");
        } else {
            usb_personality_t p;
            bool ok = true;
            if (strcmp(t, "pro2") == 0)      p = USB_PERSONALITY_SWITCH2_PRO2;
            else if (strcmp(t, "gc") == 0)   p = USB_PERSONALITY_NSO_GAMECUBE;
            else if (strcmp(t, "jcl") == 0)  p = USB_PERSONALITY_JOYCON2_L;
            else if (strcmp(t, "jcr") == 0)  p = USB_PERSONALITY_JOYCON2_R;
            else { ok = false; p = USB_PERSONALITY_SWITCH2_PRO2; }
            if (!ok) {
                queue_text("{\"persona\":\"error\","
                           "\"error\":\"usage: persona pro2|gc|jcl|jcr|config\"}");
            } else {
                g_usb_requested_personality = p;
                g_usb_personality_request_pending = true;
                queue_text("{\"persona\":\"ok\"}");
            }
        }
    } else if (strcmp(rx_line, "nfcmirror on") == 0) {
        ns2_nfc_mirror_request(true);
        queue_nfc_mirror_status("requested");
    } else if (strcmp(rx_line, "nfcmirror off") == 0) {
        ns2_nfc_mirror_request(false);
        queue_nfc_mirror_status("requested");
    } else if (strcmp(rx_line, "nfcmirror initiator on") == 0) {
        ns2_nfc_mirror_set_initiator(true);
        queue_nfc_mirror_status("initiator");
    } else if (strcmp(rx_line, "nfcmirror initiator off") == 0) {
        ns2_nfc_mirror_set_initiator(false);
        queue_nfc_mirror_status("initiator");
    } else if (strncmp(rx_line, "nfcmirror send ", 15) == 0) {
        // Originate an NFC command at the genuine controller. Nonblocking by
        // design: the BLE round trip is tens of milliseconds and core0 also
        // drives 1 kHz USB, so the reply is collected separately.
        uint8_t command[NS2_UART_NFC_COMMAND_MAX];
        size_t length = 0;
        if (!diag_parse_hex(rx_line + 15, command, sizeof(command), &length) ||
            length < 8u) {
            queue_text("{\"nfcmirror\":\"send\",\"ok\":false,"
                       "\"error\":\"expected >=8 bytes of hex\"}");
        } else if (!ns2_nfc_mirror_initiator_submit(command, length)) {
            queue_text("{\"nfcmirror\":\"send\",\"ok\":false,"
                       "\"error\":\"not armed, no genuine pro2, or busy\"}");
        } else {
            char response[96];
            snprintf(response, sizeof(response),
                     "{\"nfcmirror\":\"send\",\"ok\":true,\"length\":%u,"
                     "\"sub\":%u}",
                     (unsigned)length, command[3]);
            queue_text(response);
        }
    } else if (strcmp(rx_line, "nfcmirror reply") == 0) {
        uint8_t response_bytes[NS2_NFC_MIRROR_RESPONSE_MAX];
        size_t length = 0;
        if (!ns2_nfc_mirror_initiator_take(
                response_bytes, sizeof(response_bytes), &length)) {
            queue_text("{\"nfcmirror\":\"reply\",\"ready\":false}");
        } else {
            char hex[NS2_NFC_MIRROR_RESPONSE_MAX * 2u + 1u];
            for (size_t i = 0; i < length; i++)
                snprintf(&hex[i * 2u], 3u, "%02X", response_bytes[i]);
            char response[NS2_NFC_MIRROR_RESPONSE_MAX * 2u + 96u];
            snprintf(response, sizeof(response),
                     "{\"nfcmirror\":\"reply\",\"ready\":true,\"length\":%u,"
                     "\"sub\":%u,\"payload\":\"%s\"}",
                     (unsigned)length, response_bytes[3], hex);
            queue_text(response);
        }
    } else if (strcmp(rx_line, "nfcmirror") == 0 ||
               strcmp(rx_line, "nfcmirror status") == 0) {
        queue_nfc_mirror_status("status");
    } else if (strcmp(rx_line, "amiibo") == 0 ||
               strcmp(rx_line, "amiibo status") == 0) {
        queue_amiibo_status("status");
    } else if (strcmp(rx_line, "amiibo journal") == 0) {
        // What is actually in the two flash journal banks, independent of the
        // in-RAM slots -- distinguishes "the write never happened" from "the
        // write happened but boot did not load it".
        virtual_amiibo_journal_debug_t dbg;
        virtual_amiibo_store_journal_debug(&dbg);
        char hdr[2][49];
        for (unsigned b = 0; b < 2u; ++b) {
            for (unsigned i = 0; i < 24u; ++i)
                snprintf(&hdr[b][i * 2u], 3, "%02X", dbg.bank[b].header[i]);
            hdr[b][48] = 0;
        }
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"amiibo\":\"journal\",\"active_bank\":%d,"
                 "\"persist_pending\":%s,\"v3_slot\":%s,"
                 "\"bank0\":{\"v3\":%s,\"v3gen\":%lu,\"v2\":%s,\"v2gen\":%lu,"
                 "\"hdr\":\"%s\"},"
                 "\"bank1\":{\"v3\":%s,\"v3gen\":%lu,\"v2\":%s,\"v2gen\":%lu,"
                 "\"hdr\":\"%s\"}}",
                 dbg.active_bank,
                 dbg.persist_pending ? "true" : "false",
                 dbg.v3_slot_loaded ? "true" : "false",
                 dbg.bank[0].v3_valid ? "true" : "false",
                 (unsigned long)dbg.bank[0].v3_generation,
                 dbg.bank[0].v2_valid ? "true" : "false",
                 (unsigned long)dbg.bank[0].v2_generation, hdr[0],
                 dbg.bank[1].v3_valid ? "true" : "false",
                 (unsigned long)dbg.bank[1].v3_generation,
                 dbg.bank[1].v2_valid ? "true" : "false",
                 (unsigned long)dbg.bank[1].v2_generation, hdr[1]);
        queue_text(trace_format_response);
    } else if (strncmp(rx_line, "v3hdr", 5) == 0) {
        // Sweep the read-buffer prefix; byte 18 is the NTAG model the console
        // uses to pick its page ranges.  e.g.  v3hdr 18 05
        const char *arg = rx_line + 5;
        while (*arg == ' ') arg++;
        if (*arg == 0) {
            snprintf(trace_format_response, sizeof(trace_format_response),
                     "{\"v3hdr\":\"status\",\"bytes\":%u}",
                     ns2_v3_hdr_probe_count());
            queue_text(trace_format_response);
        } else if (strcmp(arg, "clear") == 0) {
            ns2_v3_hdr_probe_clear();
            queue_text("{\"v3hdr\":\"cleared\",\"bytes\":0}");
        } else {
            unsigned int index;
            int consumed = 0;
            if (sscanf(arg, "%u %n", &index, &consumed) != 1 || consumed == 0 ||
                index > 59u) {
                queue_text("{\"v3hdr\":\"error\","
                           "\"error\":\"usage: v3hdr [clear|<0-59> <hex>]\"}");
            } else {
                uint8_t bytes[60];
                size_t length = 0;
                if (!diag_parse_hex(arg + consumed, bytes, sizeof(bytes),
                                    &length) || length == 0 ||
                    !ns2_v3_hdr_probe_set((uint8_t)index, bytes,
                                          (uint8_t)length)) {
                    queue_text("{\"v3hdr\":\"error\",\"error\":\"bad hex or range\"}");
                } else {
                    snprintf(trace_format_response, sizeof(trace_format_response),
                             "{\"v3hdr\":\"set\",\"index\":%u,\"length\":%u,"
                             "\"bytes\":%u}", index, (unsigned)length,
                             ns2_v3_hdr_probe_count());
                    queue_text(trace_format_response);
                }
            }
        }
    } else if (strncmp(rx_line, "v3reply", 7) == 0) {
        // v3reply                -> current target subcommand
        // v3reply clear          -> bare-ACK everything again
        // v3reply <sub> <hex>    -> answer <sub> with these bytes (e.g. v3reply 0C 0102)
        const char *arg = rx_line + 7;
        while (*arg == ' ') arg++;
        if (*arg == 0) {
            snprintf(trace_format_response, sizeof(trace_format_response),
                     "{\"v3reply\":\"status\",\"sub\":%u}",
                     ns2_v3_get_reply_sub());
            queue_text(trace_format_response);
        } else if (strcmp(arg, "clear") == 0) {
            ns2_v3_clear_reply();
            queue_text("{\"v3reply\":\"cleared\",\"sub\":0}");
        } else {
            unsigned int sub;
            int consumed = 0;
            if (sscanf(arg, "%x %n", &sub, &consumed) != 1 || consumed == 0 ||
                sub == 0 || sub > 0xFFu) {
                queue_text("{\"v3reply\":\"error\","
                           "\"error\":\"usage: v3reply [clear|<sub hex> <hex>]\"}");
            } else {
                uint8_t bytes[64];
                size_t length = 0;
                if (!diag_parse_hex(arg + consumed, bytes, sizeof(bytes),
                                    &length) || length == 0 ||
                    !ns2_v3_set_reply((uint8_t)sub, bytes, (uint8_t)length)) {
                    queue_text("{\"v3reply\":\"error\",\"error\":\"bad hex or length\"}");
                } else {
                    snprintf(trace_format_response, sizeof(trace_format_response),
                             "{\"v3reply\":\"set\",\"sub\":%u,\"length\":%u}",
                             sub, (unsigned)length);
                    queue_text(trace_format_response);
                }
            }
        }
    } else if (strncmp(rx_line, "v3probe", 7) == 0) {
        // Sweep the unknown region of the 0x05 NFC status payload on hardware:
        //   v3probe                -> report how many bytes are overridden
        //   v3probe clear          -> drop all overrides
        //   v3probe <index> <hex>  -> overlay bytes at index (e.g. v3probe 16 0004040502021503)
        const char *arg = rx_line + 7;
        while (*arg == ' ') arg++;
        if (*arg == '\0') {
            snprintf(trace_format_response, sizeof(trace_format_response),
                     "{\"v3probe\":\"status\",\"bytes\":%u}",
                     ns2_v3_status_probe_count());
            queue_text(trace_format_response);
        } else if (strcmp(arg, "clear") == 0) {
            ns2_v3_status_probe_clear();
            queue_text("{\"v3probe\":\"cleared\",\"bytes\":0}");
        } else {
            unsigned int index;
            int consumed = 0;
            if (sscanf(arg, "%u %n", &index, &consumed) != 1 || consumed == 0 ||
                index > 0xFFu) {
                queue_text("{\"v3probe\":\"error\","
                           "\"error\":\"usage: v3probe [clear|<index> <hex>]\"}");
            } else {
                uint8_t bytes[NS2_NFC_STATUS_PAYLOAD_SIZE];
                size_t length = 0;
                if (!diag_parse_hex(arg + consumed, bytes, sizeof(bytes),
                                    &length) || length == 0 ||
                    !ns2_v3_status_probe_set((uint8_t)index, bytes,
                                             (uint8_t)length)) {
                    queue_text("{\"v3probe\":\"error\","
                               "\"error\":\"bad hex or range past 61-byte payload\"}");
                } else {
                    snprintf(trace_format_response,
                             sizeof(trace_format_response),
                             "{\"v3probe\":\"set\",\"index\":%u,\"length\":%u,"
                             "\"bytes\":%u}", index, (unsigned)length,
                             ns2_v3_status_probe_count());
                    queue_text(trace_format_response);
                }
            }
        }
    } else if (strncmp(rx_line, "v3mode", 6) == 0) {
        // Select the NTAG I2C 2K console read-buffer layout at runtime so the
        // hardware experiment matrix does not need a reflash per attempt.
        unsigned int mode;
        char trailing;
        if (rx_line[6] == '\0') {
            snprintf(trace_format_response, sizeof(trace_format_response),
                     "{\"v3mode\":%u}", ns2_v3_get_serve_mode());
            queue_text(trace_format_response);
        } else if (sscanf(rx_line + 6, " %u%c", &mode, &trailing) != 1 ||
                   mode > 15u) {
            queue_text("{\"v3mode\":\"error\",\"error\":\"usage: v3mode [0-15] (advertised McuTagType)\"}");
        } else {
            ns2_v3_set_serve_mode((uint8_t)mode);
            snprintf(trace_format_response, sizeof(trace_format_response),
                     "{\"v3mode\":%u}", ns2_v3_get_serve_mode());
            queue_text(trace_format_response);
        }
    } else if (strncmp(rx_line, "amiibo read ", 12) == 0) {
        unsigned int offset;
        char trailing;
        if (sscanf(rx_line + 12, "%u%c", &offset, &trailing) != 1 ||
            offset > UINT16_MAX) {
            queue_text("{\"amiibo\":\"error\","
                       "\"error\":\"usage: amiibo read OFFSET\"}");
        } else {
            queue_amiibo_read((uint16_t)offset);
        }
    } else if (strncmp(rx_line, "amiibo begin ", 13) == 0) {
        // Portal-independent upload path. The BLE config bridge carries the
        // same three commands, but when it is unavailable this lets a v3 image
        // be loaded over UART with the dongle still attached to the console --
        // which is the only way to exercise ns2_v3_serve() during a trace.
        unsigned long size = 0, crc = 0;
        char trailing;
        if (sscanf(rx_line + 13, "%lu %lx %c", &size, &crc, &trailing) != 2) {
            queue_text("{\"amiibo\":\"error\",\"error\":"
                       "\"usage: amiibo begin <540|572|2048> <crc32hex>\"}");
        } else {
            queue_amiibo_result(size == 2048u
                ? virtual_amiibo_store_v3_upload_begin((size_t)size,
                                                       (uint32_t)crc)
                : virtual_amiibo_store_upload_begin((size_t)size,
                                                    (uint32_t)crc));
        }
    } else if (strcmp(rx_line, "amiibo v3sig clear") == 0) {
        ns2_v3_clear_signature();
        queue_text("{\"amiibo\":\"v3sig\",\"ok\":true,\"cleared\":true}");
    } else if (strncmp(rx_line, "amiibo v3sig ", 13) == 0) {
        uint8_t sig[32];
        size_t length = 0;
        if (!diag_parse_hex(rx_line + 13, sig, sizeof(sig), &length) ||
            length != sizeof(sig) ||
            !ns2_v3_set_signature(sig, length)) {
            queue_text("{\"amiibo\":\"v3sig\",\"ok\":false,"
                       "\"error\":\"expected 32 bytes of hex\"}");
        } else {
            queue_text("{\"amiibo\":\"v3sig\",\"ok\":true}");
        }
    } else if (strcmp(rx_line, "amiibo v3diag") == 0) {
        uint32_t staged = 0, results = 0;
        uint32_t chunks = 0, commits = 0, errors = 0;
        uint32_t extended_chunks = 0, extended_completions = 0;
        // The console renders every v3 failure as the same 2115-0096, because
        // the wire carries only status 0x07 / detail 0x41. These fields say
        // which internal rule actually fired, which is the difference between
        // a removal-timing bug and a fail-closed record rejection.
        uint8_t last_error = 0, last_sub = 0, last_result = 0;
        uint16_t last_offset = 0;
        uint32_t error_count = 0;
        const char *last_name = "none";
        char response[448];
        ns2_v3_device_cmd_counts(&staged, &results);
        ns2_v3_write_counts(&chunks, &commits, &errors);
        ns2_v3_extended_counts(&extended_chunks, &extended_completions);
        ns2_v3_last_error(&last_error, &last_name, &last_sub, &last_result,
                          &last_offset, &error_count);
        snprintf(response, sizeof(response),
                 "{\"amiibo\":\"v3diag\",\"signature_set\":%s,"
                 "\"dev_cmd_staged\":%lu,\"dev_results\":%lu,"
                 "\"write_chunks\":%lu,\"write_commits\":%lu,"
                 "\"write_errors\":%lu,\"extended_chunks\":%lu,"
                 "\"extended_completions\":%lu,"
                 "\"errors\":%lu,\"last_error\":\"%s\",\"last_error_code\":%u,"
                 "\"last_error_sub\":%u,\"last_error_result\":\"%s\","
                 "\"last_error_offset\":%u}",
                 ns2_v3_has_signature() ? "true" : "false",
                 (unsigned long)staged, (unsigned long)results,
                 (unsigned long)chunks, (unsigned long)commits,
                 (unsigned long)errors, (unsigned long)extended_chunks,
                 (unsigned long)extended_completions,
                 (unsigned long)error_count, last_name, last_error,
                 last_sub,
                 ns2_virtual_nfc_result_string(
                     (ns2_virtual_nfc_result_t)last_result),
                 last_offset);
        queue_text(response);
    } else if (strncmp(rx_line, "amiibo chunk ", 13) == 0) {
        char *hex = strchr(rx_line + 13, ' ');
        if (!hex) {
            queue_text("{\"amiibo\":\"error\",\"error\":"
                       "\"usage: amiibo chunk <offset> <hex>\"}");
        } else {
            *hex++ = '\0';
            char *end = NULL;
            unsigned long offset = strtoul(rx_line + 13, &end, 10);
            uint8_t bytes[64];
            size_t length = 0;
            if (!end || *end != '\0') {
                queue_text("{\"amiibo\":\"error\","
                           "\"error\":\"bad chunk offset\"}");
            } else if (!diag_parse_hex(hex, bytes, sizeof(bytes), &length) ||
                       length == 0) {
                queue_text("{\"amiibo\":\"error\","
                           "\"error\":\"chunk must be 1-64 hex bytes\"}");
            } else {
                queue_amiibo_result(
                    virtual_amiibo_store_v3_upload_active()
                        ? virtual_amiibo_store_v3_upload_chunk(
                              (size_t)offset, bytes, length)
                        : virtual_amiibo_store_upload_chunk(
                              (size_t)offset, bytes, length));
            }
        }
    } else if (strcmp(rx_line, "amiibo commit") == 0) {
        queue_amiibo_result(virtual_amiibo_store_v3_upload_active()
            ? virtual_amiibo_store_v3_upload_commit()
            : virtual_amiibo_store_upload_commit());
    } else if (strcmp(rx_line, "amiibo cancel") == 0) {
        virtual_amiibo_store_upload_cancel();
        queue_text("{\"amiibo\":\"cancelled\"}");
    } else if (strcmp(rx_line, "amiibo persist") == 0) {
        virtual_amiibo_store_request_persist();
        queue_text("{\"amiibo\":\"persist_requested\"}");
    } else if (strcmp(rx_line, "amiibo acknowledge") == 0) {
        virtual_amiibo_status_t status;
        virtual_amiibo_store_status(&status);
        if (!status.loaded && !status.v3_loaded) {
            queue_text("{\"amiibo\":\"error\",\"error\":\"no image loaded\"}");
        } else {
            virtual_amiibo_store_acknowledge_download();
            queue_amiibo_status("acknowledged");
        }
    } else if (strcmp(rx_line, "motionhybrid") == 0 ||
               strcmp(rx_line, "motionhybrid status") == 0) {
        queue_motion_hybrid_status("status");
    } else if (strncmp(rx_line, "motionhybrid mode ", 18) == 0) {
        uint8_t mode = NS2_MOTION_HYBRID_MODE_OFF;
        if (!parse_motion_hybrid_mode(rx_line + 18, &mode) ||
            !ns2_motion_hybrid_live_set_mode(mode)) {
            queue_text("{\"motionhybrid\":\"error\","
                       "\"error\":\"usage: motionhybrid mode off|genuine|accel|gyro|prefix|imu|all\"}");
        } else {
            queue_motion_hybrid_status("mode_requested");
        }
    } else if (strncmp(rx_line, "motionhybrid ", 13) == 0 &&
               strchr(rx_line + 13, ' ') == NULL) {
        uint8_t mode = NS2_MOTION_HYBRID_MODE_OFF;
        if (!parse_motion_hybrid_mode(rx_line + 13, &mode) ||
            !ns2_motion_hybrid_live_set_mode(mode)) {
            queue_text("{\"motionhybrid\":\"error\","
                       "\"error\":\"usage: motionhybrid off|genuine|accel|gyro|prefix|imu|all\"}");
        } else {
            queue_motion_hybrid_status("mode_requested");
        }
    } else if (strcmp(rx_line, "motionhybrid capture start") == 0) {
        if (ns2_motion_hybrid_live_get_mode() ==
            NS2_MOTION_HYBRID_MODE_OFF) {
            queue_text("{\"motionhybrid\":\"error\","
                       "\"error\":\"select genuine or a donor mode before capture\"}");
        } else {
            ns2_motion_hybrid_live_capture_set_enabled(true);
            queue_motion_hybrid_status("capture_started");
        }
    } else if (strcmp(rx_line, "motionhybrid capture stop") == 0) {
        ns2_motion_hybrid_live_capture_set_enabled(false);
        queue_motion_hybrid_status("capture_stopped");
    } else if (strcmp(rx_line, "motionhybrid capture dump") == 0) {
        ns2_motion_hybrid_live_capture_set_enabled(false);
        queue_motion_hybrid_status("dump");
    } else if (strcmp(rx_line, "motionhybrid capture read") == 0) {
        queue_motion_hybrid_record();
    } else if (strcmp(rx_line, "motionpair") == 0 ||
               strcmp(rx_line, "motionpair status") == 0) {
        queue_motion_pair_status("status");
    } else if (strcmp(rx_line, "motionpair start") == 0) {
        ds5_motion_pair_set_enabled(true);
        queue_motion_pair_status("started");
    } else if (strcmp(rx_line, "motionpair trigger") == 0) {
        ds5_motion_pair_arm_chart_trigger();
        queue_motion_pair_status("trigger_armed");
    } else if (strcmp(rx_line, "motionpair trigger unresolved") == 0) {
        ds5_motion_pair_arm_chart_trigger_mask(
            DS5_MOTION_CHART_UNRESOLVED_STATES_MASK);
        queue_motion_pair_status("trigger_armed");
    } else if (strcmp(rx_line, "motionpair stop") == 0) {
        ds5_motion_pair_set_enabled(false);
        queue_motion_pair_status("stopped");
    } else if (strcmp(rx_line, "motionpair dump") == 0) {
        ds5_motion_pair_set_enabled(false);
        queue_motion_pair_status("dump");
    } else if (strcmp(rx_line, "motionpair read") == 0) {
        queue_motion_pair_record();
    } else if (strcmp(rx_line, "motionprobe") == 0 ||
               strcmp(rx_line, "motionprobe status") == 0) {
        queue_motion_probe_status("status");
    } else if (strcmp(rx_line, "motionprobe latch") == 0) {
        if (ns2_motion_probe_latch())
            queue_motion_probe_status("latched");
        else
            queue_text("{\"motionprobe\":\"error\",\"reason\":\"no_fresh_0x1e\"}");
    } else if (strncmp(rx_line, "motionprobe seed ", 17) == 0) {
        unsigned int state = 0;
        char trailing;
        if (sscanf(rx_line + 17, "%u%c", &state, &trailing) == 1 &&
            state < 4u && ns2_motion_probe_seed((uint8_t)state)) {
            queue_motion_probe_status("seeded");
        } else {
            queue_text("{\"motionprobe\":\"error\",\"reason\":\"seed_requires_state_0_3\"}");
        }
    } else if (strcmp(rx_line, "motionprobe on") == 0) {
        if (ns2_motion_probe_set_enabled(true))
            queue_motion_probe_status("enabled");
        else
            queue_text("{\"motionprobe\":\"error\",\"reason\":\"not_latched\"}");
    } else if (strcmp(rx_line, "motionprobe off") == 0) {
        ns2_motion_probe_set_enabled(false);
        queue_motion_probe_status("disabled");
    } else if (strcmp(rx_line, "motionprobe reset") == 0) {
        ns2_motion_probe_reset();
        queue_motion_probe_status("reset");
    } else if (strcmp(rx_line, "button y") == 0) {
        ns2_diag_input_press_y(time_us_32(), NS2_UART_Y_PULSE_US);
        queue_text("{\"button\":\"y\",\"pressed_ms\":120}");
    } else if (strncmp(rx_line, "motionprobe set ", 16) == 0) {
        unsigned long values[3] = {0};
        int consumed = 0;
        if (sscanf(rx_line + 16, "%lu %lu %lu %n",
                   &values[0], &values[1], &values[2], &consumed) == 3 &&
            rx_line[16 + consumed] == '\0' &&
            values[0] <= NS2_MOTION_ORIENTATION_MASK &&
            values[1] <= NS2_MOTION_ORIENTATION_MASK &&
            values[2] <= NS2_MOTION_ORIENTATION_MASK) {
            const uint32_t orientation[3] = {
                (uint32_t)values[0], (uint32_t)values[1], (uint32_t)values[2]
            };
            if (ns2_motion_probe_set_orientation(orientation))
                queue_motion_probe_status("set");
            else
                queue_text("{\"motionprobe\":\"error\",\"reason\":\"set_requires_latched_disabled_probe\"}");
        } else {
            queue_text("{\"motionprobe\":\"error\",\"reason\":\"set_requires_three_u26_values\"}");
        }
    } else if (strncmp(rx_line, "motionprobe rate ", 17) == 0) {
        unsigned int axis = 0;
        long rate = 0;
        int consumed = 0;
        if (sscanf(rx_line + 17, "%u %ld %n", &axis, &rate, &consumed) == 2 &&
            rx_line[17 + consumed] == '\0' &&
            axis < 3u && rate >= -262144 && rate <= 262144 &&
            ns2_motion_probe_set_rate((uint8_t)axis, (int32_t)rate)) {
            queue_motion_probe_status("rate");
        } else {
            queue_text("{\"motionprobe\":\"error\",\"reason\":\"rate_requires_axis_0_2_and_value_-262144_262144\"}");
        }
    } else if (strncmp(rx_line, "motionprobe accel ", 18) == 0) {
        long values[3] = {0};
        int consumed = 0;
        if (sscanf(rx_line + 18, "%ld %ld %ld %n",
                   &values[0], &values[1], &values[2], &consumed) == 3 &&
            rx_line[18 + consumed] == '\0') {
            const int32_t accel[3] = {
                (int32_t)values[0], (int32_t)values[1], (int32_t)values[2]
            };
            if (ns2_motion_probe_set_accel(accel))
                queue_motion_probe_status("accel");
            else
                queue_text("{\"motionprobe\":\"error\",\"reason\":\"accel_requires_latched_disabled_probe\"}");
        } else {
            queue_text("{\"motionprobe\":\"error\",\"reason\":\"accel_requires_three_i32_values\"}");
        }
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
    } else if (strcmp(rx_line, "magraw on") == 0) {
        btstack_host_request_switch2_magraw(true);
        queue_text("{\"magraw\":\"requested\",\"enabled\":true}");
    } else if (strcmp(rx_line, "magraw off") == 0) {
        btstack_host_request_switch2_magraw(false);
        queue_text("{\"magraw\":\"requested\",\"enabled\":false}");
    } else if (strcmp(rx_line, "magraw") == 0 ||
               strcmp(rx_line, "magraw status") == 0) {
        btstack_host_magraw_diag_t d;
        btstack_host_get_switch2_magraw_diag(&d);
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"magraw\":true,\"requested\":%s,\"active\":%s,"
                 "\"transition\":%s,\"v2_state\":%u,\"pid\":\"0x%04X\","
                 "\"handle\":\"0x%04X\",\"step\":%u,\"steps\":%u,"
                 "\"result\":%u,\"response\":\"0x%02X\","
                 "\"input_ccc\":\"0x%02X\"}",
                 d.requested ? "true" : "false",
                 d.active ? "true" : "false",
                 d.transition_pending ? "true" : "false",
                 d.v2_state, d.source_pid, d.connection_handle,
                 d.reference_step, d.reference_steps,
                 d.reference_result, d.last_response_status,
                 d.input_ccc_status);
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "imuref on") == 0) {
        btstack_host_request_switch2_imuref(true);
        queue_text("{\"imuref\":\"requested\",\"enabled\":true}");
    } else if (strcmp(rx_line, "imuref off") == 0) {
        btstack_host_request_switch2_imuref(false);
        queue_text("{\"imuref\":\"requested\",\"enabled\":false}");
    } else if (strcmp(rx_line, "imuref dual on") == 0) {
        btstack_host_request_switch2_imuref_dual(true);
        queue_text("{\"imuref\":\"dual_requested\",\"enabled\":true}");
    } else if (strcmp(rx_line, "imuref dual off") == 0) {
        btstack_host_request_switch2_imuref_dual(false);
        queue_text("{\"imuref\":\"dual_requested\",\"enabled\":false}");
    } else if (strncmp(
                   rx_line, "imuref interval ",
                   sizeof("imuref interval ") - 1u) == 0) {
        unsigned int interval_units = 0;
        char trailing = '\0';
        if (sscanf(
                rx_line + sizeof("imuref interval ") - 1u,
                "%u%c", &interval_units, &trailing) == 1 &&
            interval_units >= 6u && interval_units <= 24u) {
            btstack_host_request_switch2_imuref_interval(
                (uint16_t)interval_units);
            snprintf(
                trace_format_response, sizeof(trace_format_response),
                "{\"imuref\":\"interval_requested\",\"units\":%u,"
                "\"microseconds\":%u}",
                interval_units, interval_units * 1250u);
            queue_text(trace_format_response);
        } else {
            queue_text(
                "{\"error\":\"imuref interval requires 6..24 units\"}");
        }
    } else if (strcmp(rx_line, "imuref") == 0 ||
               strcmp(rx_line, "imuref status") == 0) {
        btstack_host_imuref_diag_t d;
        btstack_host_get_switch2_imuref_diag(&d);
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"imuref\":true,\"requested\":%s,\"active\":%s,"
                 "\"transition\":%s,\"dual_requested\":%s,\"dual_active\":%s,"
                 "\"dual_transition\":%s,\"v2_state\":%u,\"pid\":\"0x%04X\","
                 "\"handle\":\"0x%04X\",\"att\":\"0x%02X\","
                 "\"dual_att\":\"0x%02X\",\"interval_target\":%u,"
                 "\"interval_actual\":%u,\"interval_status\":\"0x%02X\","
                 "\"common\":%lu,\"native\":%lu}",
                 d.requested ? "true" : "false",
                 d.active ? "true" : "false",
                 d.transition_pending ? "true" : "false",
                 d.dual_requested ? "true" : "false",
                 d.dual_active ? "true" : "false",
                 d.dual_transition_pending ? "true" : "false",
                 d.v2_state, d.source_pid, d.connection_handle,
                 d.last_att_status, d.dual_att_status,
                 d.interval_target_units, d.interval_actual_units,
                 d.interval_request_status,
                 (unsigned long)d.common_notifications,
                 (unsigned long)d.native_notifications);
        queue_text(trace_format_response);
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
    } else if (strcmp(rx_line, "motionusb") == 0) {
        uint8_t report_id = 0;
        uint8_t streaming = 0;
        uint8_t motion_len = 0;
        uint16_t source_vid = 0;
        uint16_t source_pid = 0;
        ns2_native_motion_snapshot_t motion;
        ns2_dbg_report_state(&report_id, &streaming, &motion_len);
        get_global_device(0, NULL, 0, &source_vid, &source_pid);
        bool fresh = ns2_native_motion_snapshot(&motion, time_us_32(), 50000u);
        bool owned = fresh && ns2_native_motion_output_slot(motion.source_conn_index) == 0 &&
                     source_vid == 0x057E && source_pid == 0x2069;
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"motionusb\":true,\"report_id\":%u,\"streaming\":%s,"
                 "\"emitted_len\":%u,\"vid\":\"0x%04X\",\"pid\":\"0x%04X\","
                 "\"native_fresh\":%s,\"native_owned\":%s,\"native_len\":%u,"
                 "\"source_conn\":%u,\"held\":%s}",
                 report_id, streaming ? "true" : "false", motion_len,
                 source_vid, source_pid, fresh ? "true" : "false",
                 owned ? "true" : "false", fresh ? motion.length : 0u,
                 fresh ? motion.source_conn_index : 0xFFu,
                 (fresh && motion.held_after_disconnect) ? "true" : "false");
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "ds5motion") == 0 ||
               strcmp(rx_line, "ds5motion status") == 0) {
        ns2_ds5_motion_diag_t d;
        uint8_t report_id = 0;
        uint8_t streaming = 0;
        uint8_t motion_len = 0;
        ns2_dbg_ds5_motion(&d);
        ns2_dbg_report_state(&report_id, &streaming, &motion_len);
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"ds5motion\":true,\"enabled\":%s,\"active\":%s,\"initialized\":%s,"
                 "\"has_sample\":%s,\"probe_active\":%s,"
                 "\"probe_gyro\":[%d,%d,%d],"
                 "\"input_gyro\":[%d,%d,%d],"
                 "\"bias_gyro\":[%ld,%ld,%ld],"
                 "\"corrected_gyro\":[%ld,%ld,%ld],"
                 "\"jitter\":[%ld,%ld,%ld],"
                 "\"frame\":\"%s\",\"carrier\":\"%s\",\"map\":[%d,%d,%d],"
                 "\"q_million\":[%ld,%ld,%ld,%ld],"
                 "\"updates\":%lu,\"representation_rejects\":%lu,"
                 "\"host_dt_us\":%lu,\"sensor_dt_us\":%lu,"
                 "\"sensor_dt_max_us\":%lu,"
                 "\"timestamp_fallbacks\":%lu,\"timestamp_invalid\":%lu,"
                 "\"sequence_gaps\":%lu,\"integration_substeps\":%lu,"
                 "\"report_id\":%u,\"streaming\":%s,\"emitted_len\":%u}",
                 d.enabled ? "true" : "false",
                 d.source_active ? "true" : "false",
                 d.initialized ? "true" : "false",
                 d.has_sample ? "true" : "false",
                 d.probe_active ? "true" : "false",
                 d.probe_gyro[0], d.probe_gyro[1], d.probe_gyro[2],
                 d.input_gyro[0], d.input_gyro[1], d.input_gyro[2],
                 (long)d.bias_gyro[0], (long)d.bias_gyro[1],
                 (long)d.bias_gyro[2],
                 (long)d.corrected_gyro[0],
                 (long)d.corrected_gyro[1],
                 (long)d.corrected_gyro[2],
                 (long)d.jitter[0], (long)d.jitter[1],
                 (long)d.jitter[2],
                 d.body_frame ? "body" : "world",
                 d.carrier == 0u ? "switch2" :
                 (d.carrier == 1u ? "dscale" : "legacy"),
                 d.gyro_map[0], d.gyro_map[1], d.gyro_map[2],
                 (long)d.quaternion_million[0],
                 (long)d.quaternion_million[1],
                 (long)d.quaternion_million[2],
                 (long)d.quaternion_million[3],
                 (unsigned long)d.updates,
                 (unsigned long)d.representation_rejects,
                 (unsigned long)d.host_dt_us,
                 (unsigned long)d.sensor_dt_us,
                 (unsigned long)d.sensor_dt_max_us,
                 (unsigned long)d.timestamp_fallbacks,
                 (unsigned long)d.timestamp_invalid,
                 (unsigned long)d.sequence_gaps,
                 (unsigned long)d.integration_substeps,
                 report_id, streaming ? "true" : "false", motion_len);
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "ds5motion on") == 0) {
        ns2_dbg_ds5_motion_set_enabled(true);
        queue_text("{\"ds5motion\":\"enabled\",\"enabled\":true}");
    } else if (strcmp(rx_line, "ds5motion off") == 0) {
        ns2_dbg_ds5_motion_set_enabled(false);
        queue_text("{\"ds5motion\":\"disabled\",\"enabled\":false}");
    } else if (strcmp(rx_line, "ds5motion pdu40 on") == 0) {
        // Enables the coherent mixed stream: one shared tick/elapsed timeline
        // and one held native-rate PDU across intervening USB polls.
        ns2_ds5_motion40_set_enabled(true);
        queue_text("{\"ds5motion\":\"pdu40\",\"enabled\":true,"
                   "\"mode\":\"interleaved\",\"layout\":\"high_rate\"}");
    } else if (strcmp(rx_line, "ds5motion pdu40 off") == 0) {
        ns2_ds5_motion40_set_enabled(false);
        queue_text("{\"ds5motion\":\"pdu40\",\"enabled\":false,"
                   "\"mode\":\"0x1E\"}");
    } else if (strcmp(rx_line, "ds5motion pdu40 fill empty") == 0) {
        // Each 0x28 reaches the console exactly once, as on a genuine wire.
        ns2_ds5_motion40_set_fill(NS2_PDU40_FILL_EMPTY);
        queue_text("{\"ds5motion\":\"pdu40\",\"fill\":\"empty\","
                   "\"delivered\":\"once\"}");
    } else if (strcmp(rx_line, "ds5motion pdu40 fill repeat") == 0) {
        // Known bad on hardware 2026-07-31: violent erratic motion.
        ns2_ds5_motion40_set_fill(NS2_PDU40_FILL_REPEAT);
        queue_text("{\"ds5motion\":\"pdu40\",\"fill\":\"repeat\","
                   "\"delivered\":\"~20x\",\"known\":\"erratic-2026-07-31\"}");
    } else if (strcmp(rx_line, "ds5motion pdu40 fill carrier") == 0) {
        // Interleaved: select and hold complete 0x1E/0x28 frames at the native
        // cadence. This is the only fill that models the genuine USB bridge.
        ns2_ds5_motion40_set_fill(NS2_PDU40_FILL_CARRIER);
        queue_text("{\"ds5motion\":\"pdu40\",\"fill\":\"carrier\","
                   "\"mode\":\"interleaved\"}");
    } else if (strcmp(rx_line, "ds5motion pdu40 accel live") == 0) {
        ns2_ds5_motion40_set_accel_mode(NS2_DS5_MOTION40_ACCEL_LIVE);
        queue_text("{\"ds5motion\":\"pdu40\",\"accel\":\"live\","
                   "\"source_counts_per_g\":4096,"
                   "\"output_counts_per_g\":4310.1875,"
                   "\"matches\":\"0x1E\"}");
    } else if (strcmp(rx_line, "ds5motion pdu40 accel half") == 0) {
        ns2_ds5_motion40_set_accel_mode(NS2_DS5_MOTION40_ACCEL_HALF);
        queue_text("{\"ds5motion\":\"pdu40\",\"accel\":\"half\","
                   "\"counts_per_g\":2048}");
    } else if (strcmp(rx_line, "ds5motion pdu40 accel zero") == 0) {
        ns2_ds5_motion40_set_accel_mode(NS2_DS5_MOTION40_ACCEL_ZERO);
        queue_text("{\"ds5motion\":\"pdu40\",\"accel\":\"zero\","
                   "\"diagnostic_only\":true}");
    } else if (strcmp(rx_line, "ds5motion pdu40") == 0 ||
               strcmp(rx_line, "ds5motion pdu40 status") == 0) {
        uint32_t emitted = 0, starved = 0, overlong = 0, sat_a = 0, sat_g = 0;
        uint32_t carriers = 0, held = 0, fallbacks = 0;
        uint8_t output_length = 0;
        uint16_t last_tick = 0;
        ns2_ds5_motion40_get_counters(&emitted, &starved, &overlong,
                                      &sat_a, &sat_g);
        ns2_ds5_motion40_get_schedule(&carriers, &held, &fallbacks,
                                      &output_length, &last_tick);
        static const char *const fills[] = {"empty", "repeat", "carrier"};
        static const char *const accel_modes[] = {"live", "half", "zero"};
        const uint8_t fill = ns2_ds5_motion40_get_fill();
        const uint8_t accel_mode = ns2_ds5_motion40_get_accel_mode();
        // starved > 0 means the emit interval outran the source sample rate;
        // saturation means the scaling is wrong or the motion exceeded the
        // high-rate wire range (~2 g, ~999 dps). Both distinguish "well-formed but
        // wrong" from "working".
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"ds5motion\":\"pdu40\",\"enabled\":%s,\"fill\":\"%s\","
                 "\"accel\":\"%s\","
                 "\"emitted\":%lu,\"carriers\":%lu,\"held_polls\":%lu,"
                 "\"fallback_carriers\":%lu,\"output_length\":%u,"
                 "\"last_tick\":%u,"
                 "\"starved\":%lu,\"overlong\":%lu,\"saturated_accel\":%lu,"
                 "\"saturated_gyro\":%lu}",
                 ns2_ds5_motion40_get_enabled() ? "true" : "false",
                 fills[fill < 3u ? fill : 0u],
                 accel_modes[accel_mode <= NS2_DS5_MOTION40_ACCEL_ZERO
                                 ? accel_mode : NS2_DS5_MOTION40_ACCEL_LIVE],
                 (unsigned long)emitted, (unsigned long)carriers,
                 (unsigned long)held, (unsigned long)fallbacks,
                 output_length, last_tick, (unsigned long)starved,
                 (unsigned long)overlong,
                 (unsigned long)sat_a, (unsigned long)sat_g);
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "ds5motion probe off") == 0) {
        ns2_dbg_ds5_motion_probe_off();
        queue_text("{\"ds5motion\":\"probe\",\"active\":false}");
    } else if (strcmp(rx_line, "ds5motion frame body") == 0) {
        ns2_dbg_ds5_motion_set_body_frame(true);
        queue_text("{\"ds5motion\":\"frame\",\"value\":\"body\"}");
    } else if (strcmp(rx_line, "ds5motion frame world") == 0) {
        ns2_dbg_ds5_motion_set_body_frame(false);
        queue_text("{\"ds5motion\":\"frame\",\"value\":\"world\"}");
    } else if (strcmp(rx_line, "ds5motion carrier switch2") == 0) {
        ns2_dbg_ds5_motion_set_carrier(0u);
        queue_text("{\"ds5motion\":\"carrier\",\"value\":\"switch2\",\"orientation_reset\":true}");
    } else if (strcmp(rx_line, "ds5motion carrier dscale") == 0) {
        ns2_dbg_ds5_motion_set_carrier(1u);
        queue_text("{\"ds5motion\":\"carrier\",\"value\":\"dscale\",\"orientation_reset\":true}");
    } else if (strcmp(rx_line, "ds5motion carrier legacy") == 0) {
        ns2_dbg_ds5_motion_set_carrier(2u);
        queue_text("{\"ds5motion\":\"carrier\",\"value\":\"legacy\",\"orientation_reset\":true}");
    } else if (strncmp(rx_line, "ds5motion map ", 14) == 0) {
        int values[3] = {0, 0, 0};
        int consumed = 0;
        int8_t map[3];
        if (sscanf(rx_line + 14, "%d %d %d %n",
                   &values[0], &values[1], &values[2], &consumed) == 3 &&
            rx_line[14 + consumed] == '\0' &&
            values[0] >= -3 && values[0] <= 3 && values[0] != 0 &&
            values[1] >= -3 && values[1] <= 3 && values[1] != 0 &&
            values[2] >= -3 && values[2] <= 3 && values[2] != 0) {
            for (unsigned i = 0; i < 3; ++i)
                map[i] = (int8_t)values[i];
            if (ns2_dbg_ds5_motion_set_map(map)) {
                queue_text("{\"ds5motion\":\"map\",\"updated\":true}");
            } else {
                queue_text("{\"ds5motion\":\"error\",\"reason\":\"map_axes_must_be_unique\"}");
            }
        } else {
            queue_text("{\"ds5motion\":\"error\",\"reason\":\"map_requires_three_signed_axes_1_2_3\"}");
        }
    } else if (strncmp(rx_line, "ds5motion probe rate ", 21) == 0) {
        unsigned int axis = 0;
        long rate = 0;
        int consumed = 0;
        if (sscanf(rx_line + 21, "%u %ld %n",
                   &axis, &rate, &consumed) == 2 &&
            rx_line[21 + consumed] == '\0' &&
            axis < 3u && rate >= -4096 && rate <= 4096 &&
            ns2_dbg_ds5_motion_probe_rate((uint8_t)axis,
                                          (int16_t)rate)) {
            queue_text("{\"ds5motion\":\"probe\",\"active\":true}");
        } else {
            queue_text("{\"ds5motion\":\"error\",\"reason\":\"probe_rate_requires_axis_0_2_and_value_-4096_4096\"}");
        }
    } else if (strcmp(rx_line, "btdev") == 0) {
        // Bounded snapshot of the live BTHID device table.
        //
        // Exists because "which driver is actually bound to this peer?" was not
        // answerable over UART, and every identity/classification question --
        // late VID/PID promotion, descriptor-time reclassification, generic
        // fallback -- resolves to exactly that. Names are truncated and the
        // table is capped at BTHID_MAX_DEVICES, so the reply stays bounded.
        //
        // `kbcap`/`mousecap` re-run the structural descriptor tests against the
        // cached descriptor. bthid caches one descriptor at a time, so they are
        // reported only for the connection that descriptor belongs to; a `null`
        // means "not this peer's descriptor", not "capability absent".
        const uint8_t *cached = NULL;
        uint16_t cached_len = 0;
        uint8_t cached_conn = 0;
        bool have_cached = bthid_get_cached_descriptor(&cached, &cached_len,
                                                       &cached_conn);
        int j = snprintf(trace_format_response, sizeof(trace_format_response),
                         "{\"btdev\":[");
        for (uint8_t slot = 0; slot < BTHID_MAX_DEVICES; ++slot) {
            const bthid_device_t *dev = bthid_get_device_slot(slot);
            if (!dev) continue;
            const bthid_driver_t *drv = (const bthid_driver_t *)dev->driver;
            char name[17];
            unsigned n = 0;
            for (; dev->name[n] && n < sizeof(name) - 1u; ++n) {
                unsigned char c = (unsigned char)dev->name[n];
                name[n] = (c < 0x20u || c == '"' || c == '\\') ? ' ' : (char)c;
            }
            name[n] = '\0';
            bool desc_is_ours = have_cached && cached_conn == dev->conn_index &&
                                cached_len > 0;
            char kbcap[8] = "null";
            char mousecap[8] = "null";
            if (desc_is_ours) {
                snprintf(kbcap, sizeof(kbcap), "%s",
                         bthid_keyboard_descriptor_is_keyboard(cached, cached_len)
                             ? "true" : "false");
                snprintf(mousecap, sizeof(mousecap), "%s",
                         bthid_mouse_descriptor_is_mouse(cached, cached_len)
                             ? "true" : "false");
            }
            j += snprintf(trace_format_response + j,
                          sizeof(trace_format_response) - (size_t)j,
                          "%s{\"conn\":%u,\"gen\":%lu,\"ble\":%s,"
                          "\"name\":\"%.16s\",\"vid\":\"0x%04X\",\"pid\":\"0x%04X\","
                          "\"driver\":\"%.24s\",\"type\":%u,"
                          "\"desc_len\":%u,\"desc_mine\":%s,"
                          "\"kbcap\":%s,\"mousecap\":%s,\"generic\":%s}",
                          j > 10 ? "," : "",
                          dev->conn_index,
                          (unsigned long)dev->connection_generation,
                          dev->is_ble ? "true" : "false",
                          name, dev->vendor_id, dev->product_id,
                          drv && drv->name ? drv->name : "none",
                          (unsigned)dev->type,
                          desc_is_ours ? cached_len : 0u,
                          desc_is_ours ? "true" : "false",
                          kbcap, mousecap,
                          drv == &bthid_gamepad_driver ? "true" : "false");
            if (j < 0 || (size_t)j >= sizeof(trace_format_response)) {
                j = (int)sizeof(trace_format_response) - 1;
                break;
            }
        }
        snprintf(trace_format_response + j,
                 sizeof(trace_format_response) - (size_t)j, "]}");
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "kbm") == 0 ||
               strcmp(rx_line, "kbm status") == 0) {
        // One bounded snapshot answering the cross-layer questions for a KB/M
        // session: which mode is selected, which roles are filled, whether
        // reports are arriving, and why a peer was refused.
        ns2_kbm_runtime_status_t kbm;
        ns2_kbm_runtime_status(&kbm);
        // Shared with the management surface and pinned by a host test. Keeping
        // a second copy of this format string here is what produced a
        // diagnostic reporting `"override":"kb","profile":"false"` and a
        // garbage counter -- worse than no diagnostic, because it invites wrong
        // conclusions about hardware.
        (void)ns2_kbm_status_format(&kbm, trace_format_response,
                                    sizeof(trace_format_response));
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "kbm mouse") == 0) {
        // Mouse-translation settings, rendered by the same formatter the
        // management surface uses. This channel deliberately owns no copy of
        // the schema, the field set, or the accepted values.
        ns2_kbm_mouse_config_t mouse;
        ns2_kbm_runtime_get_mouse(&mouse);
        (void)ns2_kbm_mouse_format(&mouse, trace_format_response,
                                   sizeof(trace_format_response));
        queue_text(trace_format_response);
    } else if (strncmp(rx_line, "kbm mouse ", 10) == 0) {
        // Apply to a copy, then store: ns2_kbm_runtime_set_mouse() is what
        // validates the range, and it REJECTS rather than clamping, so a bad
        // value is reported instead of silently becoming a different one. The
        // new value applies live on the next mouse report and stays in RAM
        // until an explicit `save`.
        ns2_kbm_mouse_config_t mouse;
        ns2_kbm_runtime_get_mouse(&mouse);
        if (!ns2_kbm_mouse_command_apply(&mouse, rx_line + 10) ||
            !ns2_kbm_runtime_set_mouse(&mouse)) {
            queue_text("{\"kbm\":\"error\",\"reason\":"
                       "\"mouse_field_or_value_out_of_range\"}");
        } else {
            (void)ns2_kbm_mouse_format(&mouse, trace_format_response,
                                       sizeof(trace_format_response));
            queue_text(trace_format_response);
        }
    } else if (strncmp(rx_line, "kbm mode ", 9) == 0) {
        ns2_kbm_mode_t mode;
        if (!ns2_kbm_mode_from_name(rx_line + 9, &mode) ||
            !ns2_kbm_runtime_set_mode(mode)) {
            queue_text("{\"kbm\":\"error\",\"reason\":"
                       "\"mode_must_be_auto_controller_keyboard_or_kbmouse\"}");
        } else {
            snprintf(trace_format_response, sizeof(trace_format_response),
                     "{\"kbm\":\"mode\",\"value\":\"%s\"}",
                     ns2_kbm_mode_name(mode));
            queue_text(trace_format_response);
        }
    } else if (strcmp(rx_line, "input sources") == 0) {
        queue_active_input_status();
    } else if (strncmp(rx_line, "input active ", 13) == 0) {
        const char *arg = rx_line + 13;
        uint32_t id = 0;
        if (strcmp(arg, "none") != 0) {
            char *end = NULL;
            unsigned long parsed = strtoul(arg, &end, 10);
            if (!arg[0] || !end || *end != '\0' || parsed > UINT32_MAX) {
                queue_text("{\"active_input\":\"error\",\"reason\":\"bad_source_id\"}");
                goto command_done;
            }
            id = (uint32_t)parsed;
        }
        if (!ns2_active_input_request(id)) {
            queue_text("{\"active_input\":\"error\",\"reason\":\"unknown_source\"}");
        } else {
            snprintf(trace_format_response, sizeof(trace_format_response),
                     "{\"active_input\":\"queued\",\"active\":%lu}",
                     (unsigned long)id);
            queue_text(trace_format_response);
        }
    } else if (strcmp(rx_line, "input") == 0 ||
               strcmp(rx_line, "input status") == 0) {
        switch_pro_input_t in;
        uint16_t vid = 0;
        uint16_t pid = 0;
        get_global_gamepad_input(0, &in);
        get_global_device(0, NULL, 0, &vid, &pid);
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"input\":true,\"raw\":\"0x%08lX\","
                 "\"mapped\":[%u,%u,%u],\"extra\":%u,"
                 "\"sticks\":[%u,%u,%u,%u,%u,%u],"
                 "\"has_motion\":%s,\"motion_source\":%u,"
                 "\"gyro\":[%d,%d,%d],\"accel\":[%d,%d,%d],"
                 "\"vid\":\"0x%04X\","
                 "\"pid\":\"0x%04X\"}",
                 (unsigned long)get_global_raw_buttons(0),
                 in.buttons[0], in.buttons[1], in.buttons[2], in.extra,
                 in.left_stick[0], in.left_stick[1], in.left_stick[2],
                 in.right_stick[0], in.right_stick[1],
                 in.right_stick[2],
                 in.has_motion ? "true" : "false", in.motion_source,
                 // Accel is the axis-mapping evidence that needs no movement:
                 // a resting controller reads gravity, which identifies the
                 // face-normal (yaw) axis and its sign directly.
                 in.gyro[0], in.gyro[1], in.gyro[2],
                 in.accel[0], in.accel[1], in.accel[2], vid, pid);
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "audio headset") == 0) {
        switch_pro_input_t in;
        uint16_t vid = 0;
        uint16_t pid = 0;
        uint8_t headset_state = CONTROLLER_HEADSET_NONE;
        get_global_gamepad_input(0, &in);
        get_global_device(0, NULL, 0, &vid, &pid);
#ifdef NS2_DS5_AUDIO
        headset_state = in.headset_state;
#endif
        const char *kind =
            headset_state == CONTROLLER_HEADSET_HEADSET ? "headset" :
            headset_state == CONTROLLER_HEADSET_HEADPHONES ? "headphones" :
            "none";
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"headset\":true,\"state\":%u,\"kind\":\"%s\","
                 "\"vid\":\"0x%04X\",\"pid\":\"0x%04X\"}",
                 headset_state, kind, vid, pid);
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "audio") == 0 ||
               strcmp(rx_line, "audio status") == 0) {
        ds5_audio_diag_t d;
        ds5_audio_diag_get(&d);
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"audio\":true,\"usb_active\":%s,"
                 "\"send_max_us\":%lu,\"send_over_40ms\":%lu,"
                 "\"sends\":%lu,\"hci_max_us\":%lu,"
                 "\"hci_over_40ms\":%lu,\"hci_events\":%lu,"
                 "\"hci_packets\":%lu,\"hci_max_batch\":%lu,"
                 "\"pcm_packets\":%lu,\"pcm_nonzero\":%lu,"
                 "\"pcm_short\":%lu,\"pcm_dropped\":%lu,"
                 "\"pcm_max_gap_us\":%lu,\"pcm_over_2ms\":%lu,"
                 "\"pcm_queue_max\":%lu,"
                 "\"opus_frames\":%lu,\"opus_errors\":%lu,"
                 "\"opus_encode_max_us\":%lu,\"opus_gap_max_us\":%lu,"
                 "\"opus_over_20ms\":%lu,\"pipeline_resets\":%lu,"
                 "\"codec_calls\":%lu,\"codec_blocks\":%lu,"
                 "\"codec_no_encoder\":%lu,\"codec_no_pcm\":%lu,"
                 "\"codec_disconnected\":%lu,\"codec_usb_inactive\":%lu,"
                 "\"codec_gap_max_us\":%lu,\"codec_over_10ms\":%lu,"
                 "\"codec_hist_3_7_12_25_over\":[%lu,%lu,%lu,%lu,%lu],"
                 "\"core1_gap_max_us\":%lu,\"core1_over_10ms\":%lu,"
                 "\"usb_edges_on_off\":[%lu,%lu],"
                 "\"usb_active_us\":%lu,"
                 "\"core1_stack_free\":%lu}",
                 d.usb_speaker_active ? "true" : "false",
                 (unsigned long)d.send_max_gap_us,
                 (unsigned long)d.send_gaps_over_40ms,
                 (unsigned long)d.sends_total,
                 (unsigned long)d.hci_complete_max_gap_us,
                 (unsigned long)d.hci_complete_gaps_over_40ms,
                 (unsigned long)d.hci_complete_events,
                 (unsigned long)d.hci_completed_packets,
                 (unsigned long)d.hci_complete_max_batch,
                 (unsigned long)d.pcm_packets_total,
                 (unsigned long)d.pcm_nonzero_packets,
                 (unsigned long)d.pcm_short_packets,
                 (unsigned long)d.pcm_dropped_packets,
                 (unsigned long)d.pcm_max_gap_us,
                 (unsigned long)d.pcm_gaps_over_2ms,
                 (unsigned long)d.pcm_queue_max_depth,
                 (unsigned long)d.opus_frames_total,
                 (unsigned long)d.opus_encode_errors,
                 (unsigned long)d.opus_encode_max_us,
                 (unsigned long)d.opus_max_gap_us,
                 (unsigned long)d.opus_gaps_over_20ms,
                 (unsigned long)d.pipeline_resets,
                 (unsigned long)d.codec_calls_total,
                 (unsigned long)d.codec_blocks_dequeued,
                 (unsigned long)d.codec_no_encoder,
                 (unsigned long)d.codec_no_pcm,
                 (unsigned long)d.codec_disconnected,
                 (unsigned long)d.codec_usb_inactive,
                 (unsigned long)d.codec_call_max_gap_us,
                 (unsigned long)d.codec_call_gaps_over_10ms,
                 (unsigned long)d.codec_gap_le_3ms,
                 (unsigned long)d.codec_gap_le_7ms,
                 (unsigned long)d.codec_gap_le_12ms,
                 (unsigned long)d.codec_gap_le_25ms,
                 (unsigned long)d.codec_gap_over_25ms,
                 (unsigned long)d.core1_max_gap_us,
                 (unsigned long)d.core1_gaps_over_10ms,
                 (unsigned long)d.usb_speaker_on_edges,
                 (unsigned long)d.usb_speaker_off_edges,
                 (unsigned long)d.usb_speaker_active_us,
                 (unsigned long)ns2_audio_core1_stack_free_bytes());
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "audio clear") == 0) {
        ds5_audio_diag_reset();
        queue_text("{\"audio\":\"cleared\"}");
    } else if (strcmp(rx_line, "ds5codec") == 0 ||
               strcmp(rx_line, "ds5codec status") == 0) {
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"ds5codec\":true,\"bitrate\":160000,"
                 "\"format\":\"fullband_stereo\","
                 "\"exclusive_encode\":%s,"
                 "\"transport_bytes\":200}",
                 ds5_audio_bridge_ds5_exclusive_encode()
                     ? "true" : "false");
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "ds5codec lock on") == 0) {
        ds5_audio_bridge_set_ds5_exclusive_encode(true);
        queue_text("{\"ds5codec\":\"exclusive_encode\",\"enabled\":true}");
    } else if (strcmp(rx_line, "ds5codec lock off") == 0) {
        ds5_audio_bridge_set_ds5_exclusive_encode(false);
        queue_text("{\"ds5codec\":\"exclusive_encode\",\"enabled\":false}");
    } else if (strcmp(rx_line, "pro2audio on") == 0) {
        btstack_host_set_switch2_pro2_audio_capture(true);
        queue_text("{\"pro2audio\":\"requested\",\"enabled\":true}");
    } else if (strcmp(rx_line, "pro2audio off") == 0) {
        btstack_host_set_switch2_pro2_audio_capture(false);
        queue_text("{\"pro2audio\":\"requested\",\"enabled\":false}");
    } else if (strcmp(rx_line, "pro2audio live on") == 0) {
        btstack_host_request_switch2_pro2_audio_live(true);
        queue_text("{\"pro2audio\":\"live_requested\",\"enabled\":true}");
    } else if (strcmp(rx_line, "pro2audio live off") == 0) {
        btstack_host_request_switch2_pro2_audio_live(false);
        queue_text("{\"pro2audio\":\"live_requested\",\"enabled\":false}");
    } else if (strncmp(rx_line, "pro2audio complexity ", 21) == 0) {
        unsigned int complexity;
        char trailing;
        if (sscanf(rx_line + 21, "%u%c", &complexity, &trailing) != 1 ||
            complexity > 10u) {
            queue_text("{\"pro2audio\":\"error\",\"error\":\"usage: pro2audio complexity 0-10\"}");
        } else {
            ds5_audio_bridge_set_switch2_pro2_complexity(
                (uint8_t)complexity);
            snprintf(trace_format_response, sizeof(trace_format_response),
                     "{\"pro2audio\":\"complexity\",\"value\":%u}",
                     complexity);
            queue_text(trace_format_response);
        }
    } else if (strcmp(rx_line, "pro2audio analysis on") == 0) {
        ds5_audio_bridge_set_switch2_pro2_analysis(true);
        queue_text("{\"pro2audio\":\"analysis\",\"enabled\":true}");
    } else if (strcmp(rx_line, "pro2audio analysis off") == 0) {
        ds5_audio_bridge_set_switch2_pro2_analysis(false);
        queue_text("{\"pro2audio\":\"analysis\",\"enabled\":false}");
    } else if (strcmp(rx_line, "pro2audio replay") == 0) {
        btstack_host_request_switch2_pro2_audio_replay(true);
        queue_text("{\"pro2audio\":\"replay_requested\"}");
    } else if (strcmp(rx_line, "pro2audio replay stop") == 0) {
        btstack_host_request_switch2_pro2_audio_replay(false);
        queue_text("{\"pro2audio\":\"replay_stopped\"}");
    } else if (strcmp(rx_line, "pro2audio pcm capture") == 0) {
        ds5_audio_pcm_capture_arm();
        queue_pcm_status("armed");
    } else if (strcmp(rx_line, "pro2audio pcm stop") == 0) {
        ds5_audio_pcm_capture_stop();
        queue_pcm_status("stopped");
    } else if (strcmp(rx_line, "pro2audio pcm") == 0 ||
               strcmp(rx_line, "pro2audio pcm status") == 0) {
        queue_pcm_status("status");
    } else if (strncmp(rx_line, "pro2audio pcm read ", 19) == 0) {
        unsigned int offset;
        char trailing;
        if (sscanf(rx_line + 19, "%u%c", &offset, &trailing) != 1 ||
            offset > UINT16_MAX) {
            queue_text("{\"pro2audio_pcm\":\"error\",\"error\":\"usage: pro2audio pcm read OFFSET\"}");
        } else {
            queue_pcm_record((uint16_t)offset);
        }
    } else if (strcmp(rx_line, "pro2audio") == 0 ||
               strcmp(rx_line, "pro2audio status") == 0) {
        btstack_host_pro2_audio_diag_t d;
        btstack_host_get_switch2_pro2_audio_diag(&d);
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"pro2audio\":true,\"requested\":%s,\"active\":%s,"
                 "\"state\":%u,\"att_status\":\"0x%02X\","
                 "\"pid\":\"0x%04X\",\"handle\":\"0x%04X\","
                 "\"notifications\":%lu,\"last_len\":%u,\"max_len\":%u,"
                 "\"headset_raw\":\"0x%02X\",\"audio_len\":%u,"
                 "\"compact_failures\":%lu,"
                 "\"replay_requested\":%s,\"replay_active\":%s,"
                 "\"replay_state\":%u,\"replay_status\":\"0x%02X\","
                 "\"replay_frames\":%u,"
                 "\"live_requested\":%s,\"live_active\":%s,"
                 "\"live_state\":%u,\"live_status\":\"0x%02X\","
                 "\"live_toc\":\"0x%02X\","
                 "\"live_prefix\":\"%02X%02X%02X%02X%02X%02X\","
                 "\"complexity\":%u,"
                 "\"analysis\":%s,"
                 "\"live_prime\":%u,\"live_frames\":%lu,"
                 "\"live_underruns\":%lu}",
                 d.requested ? "true" : "false", d.active ? "true" : "false",
                 d.state, d.last_att_status, d.source_pid, d.connection_handle,
                 (unsigned long)d.notifications, d.last_report_length,
                 d.max_report_length, d.last_headset_raw, d.last_audio_length,
                 (unsigned long)d.compact_failures,
                 d.replay_requested ? "true" : "false",
                 d.replay_active ? "true" : "false", d.replay_state,
                 d.replay_last_send_status, d.replay_frames_sent,
                 d.live_requested ? "true" : "false",
                 d.live_active ? "true" : "false", d.live_state,
                 d.live_last_send_status, d.live_last_toc,
                 d.live_prefix[0], d.live_prefix[1], d.live_prefix[2],
                 d.live_prefix[3], d.live_prefix[4], d.live_prefix[5],
                 ds5_audio_bridge_switch2_pro2_complexity(),
                 ds5_audio_bridge_switch2_pro2_analysis()
                    ? "true" : "false",
                 d.live_prime_count,
                 (unsigned long)d.live_frames_sent,
                 (unsigned long)d.live_underruns);
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "btreconnect") == 0) {
        btstack_host_reconnect_diag_t d;
        btstack_host_get_reconnect_diag(&d);
        snprintf(trace_format_response, sizeof(trace_format_response),
                 "{\"btreconnect\":true,\"powered\":%s,\"state\":%u,"
                 "\"scanning\":%s,\"connected_ble\":%u,"
                 "\"has_target\":%s,\"ltk_ready\":%s,\"fresh_fallback\":%s,"
                 "\"attempt_active\":%s,\"attempts\":%u,"
                 "\"addr_type\":%u,\"ble_strategy\":%u,"
                 "\"vid\":\"0x%04X\",\"pid\":\"0x%04X\","
                 "\"local_addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                 "\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                 "\"name\":\"%s\",\"adv\":%lu,\"target_adv\":%lu,"
                 "\"switch2_adv\":%lu,\"target_adv_type\":%u,"
                 "\"bonds\":%u,\"bond_cap\":%u,\"bonded_adv\":%lu,"
                 "\"nontarget_adv\":%lu,\"rpa_adv\":%lu,"
                 "\"target_connects\":%lu,\"target_success\":%lu,"
                 "\"target_fail\":%lu,\"last_status\":\"0x%02X\","
                 "\"reencrypt_start\":%lu,\"reencrypt_ok\":%lu,"
                 "\"reencrypt_fail\":%lu,\"reencrypt_status\":\"0x%02X\","
                 "\"direct_active\":%s,\"direct_handle\":\"0x%04X\","
                 "\"direct_ms\":%lu,\"encrypt_phase\":%u,\"link_key_size\":%u,\"hci_cmd_ready\":%s,"
                 "\"cmd_status_n\":%lu,\"cmd_status_opcode\":\"0x%04X\",\"cmd_status\":\"0x%02X\","
                 "\"cmd_complete_n\":%lu,\"cmd_complete_opcode\":\"0x%04X\",\"cmd_complete_status\":\"0x%02X\","
                 "\"encrypt_evt_n\":%lu,\"encrypt_evt_status\":\"0x%02X\",\"encrypt_enabled\":%u,"
                 "\"disconnect_n\":%lu,\"disconnect_reason\":\"0x%02X\","
                 "\"ltk_reads\":%lu,\"spi_ltk_valid\":%s,\"spi_ltk_phase\":%u,"
                 "\"spi_norm_matches_derived\":%s,\"spi_raw_matches_derived\":%s}",
                 d.powered_on ? "true" : "false", d.state,
                 d.scan_active ? "true" : "false", d.connected_ble_count,
                 d.has_last_connected ? "true" : "false",
                 d.has_last_connected_ltk ? "true" : "false",
                 d.force_fresh_custom_pairing ? "true" : "false",
                 d.connect_attempt_active ? "true" : "false", d.reconnect_attempts,
                 d.last_connected_addr_type, d.last_connected_ble_strategy,
                 d.last_connected_vid, d.last_connected_pid,
                 d.local_addr[0], d.local_addr[1], d.local_addr[2],
                 d.local_addr[3], d.local_addr[4], d.local_addr[5],
                 d.last_connected_addr[0], d.last_connected_addr[1],
                 d.last_connected_addr[2], d.last_connected_addr[3],
                 d.last_connected_addr[4], d.last_connected_addr[5],
                 d.last_connected_name,
                 (unsigned long)d.advertising_reports,
                 (unsigned long)d.target_advertising_reports,
                 (unsigned long)d.switch2_advertising_reports,
                 d.last_target_advertising_event_type,
                 d.bond_count, d.bond_capacity,
                 (unsigned long)d.bonded_advertising_reports,
                 (unsigned long)d.nontarget_advertising_reports,
                 (unsigned long)d.rpa_advertising_reports,
                 (unsigned long)d.target_connect_attempts,
                 (unsigned long)d.target_connect_successes,
                 (unsigned long)d.target_connect_failures,
                 d.last_target_connect_status,
                 (unsigned long)d.reencryption_started,
                 (unsigned long)d.reencryption_successes,
                 (unsigned long)d.reencryption_failures,
                 d.last_reencryption_status,
                 d.direct_reencrypt_active ? "true" : "false",
                 d.direct_reencrypt_handle,
                 (unsigned long)d.direct_reencrypt_elapsed_ms,
                 d.direct_encrypt_phase,
                 d.direct_link_key_size,
                 d.hci_command_ready ? "true" : "false",
                 (unsigned long)d.direct_cmd_status_events,
                 d.last_direct_cmd_status_opcode, d.last_direct_cmd_status,
                 (unsigned long)d.direct_cmd_complete_events,
                 d.last_direct_cmd_complete_opcode, d.last_direct_cmd_complete_status,
                 (unsigned long)d.direct_encrypt_events,
                 d.last_direct_encrypt_status, d.last_direct_encrypt_enabled,
                 (unsigned long)d.switch2_disconnect_events,
                 d.last_switch2_disconnect_reason,
                 (unsigned long)d.pairing_ltk_reads,
                 d.pairing_ltk_valid ? "true" : "false", d.pairing_ltk_phase,
                 d.pairing_ltk_matches_derived ? "true" : "false",
                 d.pairing_ltk_raw_matches_derived ? "true" : "false");
        queue_text(trace_format_response);
    } else if (strcmp(rx_line, "btfresh") == 0) {
        btstack_host_force_switch2_fresh_pairing();
        queue_text("{\"ok\":true,\"btfresh\":\"scheduled\"}");
    } else if (strcmp(rx_line, "reenumerate") == 0) {
        reenumerate_requested = true;
        queue_text("{\"ok\":true,\"reenumerate\":true}");
    } else if (strcmp(rx_line, "save") == 0) {
        // Arms the SAME deferred write the `save` command arms on every other
        // surface; core1's control tick performs it within ~30 ms. Acked
        // immediately rather than waited on, matching the in-band management
        // path -- the flash erase parks core0 and must not be blocked on here.
        // Persists the complete settings record, not just KB/M.
        config_request_save();
        queue_text("{\"ok\":true,\"save\":\"queued\"}");
    } else if (strcmp(rx_line, "help") == 0) {
        queue_text("{\"commands\":[\"ping\",\"fwreads\",\"status\",\"clear\","
                   "\"profile\",\"profile default\","
                   "\"profile C.M.m B.M.m D.M.m\",\"btversion request\","
                   "\"btversion\",\"trace status\",\"trace clear\","
                   "\"trace start\",\"trace start bulk\",\"trace start nfc\",\"trace stop\",\"trace dump\","
                   "\"trace read N\","
                   "\"blecap status\",\"blecap start\",\"blecap nfc start\",\"blecap stop\","
                   "\"blecap dump\",\"blecap read\",\"blecap variant 0-9\","
                   "\"blecap gattdisc on|off|status\","
                   "\"blecap mark TEXT\","
                   "\"nfcmirror on|off|status\","
                   "\"nfcmirror initiator on|off\",\"nfcmirror send HEX\","
                   "\"nfcmirror reply\","
                   "\"amiibo status|read OFFSET|acknowledge|dump (PC helper)\","
                   "\"amiibo v3sig HEX32|v3sig clear\",\"amiibo v3diag|journal\","
                   "\"motionhybrid status|off|genuine|accel|gyro|prefix|imu|all|capture start|stop|dump|read\","
                   "\"motionpair status|start|trigger|stop|dump|read\",\"magraw on|off|status\","
                   "\"imuref on|off|status\",\"imuref dual on|off\","
                   "\"imuref interval 6-24\","
                   "\"motionprobe status|latch|seed STATE|on|off|reset|set G0 G1 G2|rate AXIS VALUE|accel X Y Z\","
                   "\"button y\","
                   "\"motionauto\",\"motionusb\",\"ds5motion status|on|off|frame body|world|carrier switch2|dscale|legacy|map SX SY SZ|probe rate AXIS VALUE|probe off|pdu40 on|off|status|accel live|half|zero\",\"input status\",\"input sources\",\"input active ID|none\",\"audio status|clear|headset\",\"ds5codec status|lock on|lock off\","
                   "\"pro2audio on|off|status|live on|live off|complexity 0-10|analysis on|analysis off|replay|replay stop\","
                   "\"kbm status\",\"kbm mode auto|controller|keyboard|kbmouse\","
                   "\"kbm mouse\",\"kbm mouse sensitivity|sensitivityx|"
                   "sensitivityy|recenter|invertx|inverty|antideadzone "
                   "<value>\",\"btdev\","
                   "\"btreconnect\",\"btbonds\",\"btfresh\",\"btreject\",\"btrefuse\","
                   "\"btlife dump N\","
                   "\"btauth\","
                   "\"expmode [status|inquiry on|inquiry off] (EXPERIMENT)\","
                   "\"reenumerate\",\"bootsel\",\"save\",\"help\"]}");
    } else if (rx_length != 0) {
        queue_text("{\"error\":\"unknown command\"}");
    }
command_done:
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
    ns2_diag_input_reset();
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
