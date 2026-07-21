#include "ns2_firmware_profile.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if NS2_PRO_FW_MAJOR > 255 || NS2_PRO_FW_MINOR > 255 || NS2_PRO_FW_MICRO > 255
#error "Switch 2 controller firmware version components must fit in one byte"
#endif
#if NS2_PRO_BT_MAJOR > 255 || NS2_PRO_BT_MINOR > 255 || NS2_PRO_BT_MICRO > 255
#error "Switch 2 Bluetooth firmware version components must fit in one byte"
#endif
#if NS2_PRO_DSP_MAJOR > 255 || NS2_PRO_DSP_MINOR > 255 || NS2_PRO_DSP_MICRO > 255
#error "Switch 2 DSP firmware version components must fit in one byte"
#endif

const ns2_firmware_profile_t ns2_firmware_profile = {
    .controller = {NS2_PRO_FW_MAJOR, NS2_PRO_FW_MINOR, NS2_PRO_FW_MICRO},
    .bluetooth = {NS2_PRO_BT_MAJOR, NS2_PRO_BT_MINOR, NS2_PRO_BT_MICRO},
    .dsp = {NS2_PRO_DSP_MAJOR, NS2_PRO_DSP_MINOR, NS2_PRO_DSP_MICRO},
};

static ns2_firmware_profile_t active_profile = {
    .controller = {NS2_PRO_FW_MAJOR, NS2_PRO_FW_MINOR, NS2_PRO_FW_MICRO},
    .bluetooth = {NS2_PRO_BT_MAJOR, NS2_PRO_BT_MINOR, NS2_PRO_BT_MICRO},
    .dsp = {NS2_PRO_DSP_MAJOR, NS2_PRO_DSP_MINOR, NS2_PRO_DSP_MICRO},
};
static bool runtime_override_active;

static ns2_firmware_diagnostics_t diagnostics;

static void increment_saturating_u16(uint16_t *value) {
    if (*value != UINT16_MAX) (*value)++;
}

const ns2_firmware_profile_t *ns2_firmware_profile_active(void) {
    return &active_profile;
}

bool ns2_firmware_profile_runtime_override_active(void) {
    return runtime_override_active;
}

void ns2_firmware_profile_set_runtime(const uint8_t controller[3],
                                      const uint8_t bluetooth[3],
                                      const uint8_t dsp[3]) {
    memcpy(active_profile.controller, controller, 3);
    memcpy(active_profile.bluetooth, bluetooth, 3);
    memcpy(active_profile.dsp, dsp, 3);
    runtime_override_active = true;
}

void ns2_firmware_profile_reset_runtime(void) {
    active_profile = ns2_firmware_profile;
    runtime_override_active = false;
}

void ns2_firmware_build_ep0_info(uint8_t out[16], const uint8_t unit_id[6]) {
    memset(out, 0, 16);
    memcpy(&out[0], active_profile.controller, 3);
    out[6] = active_profile.bluetooth[0];
    memcpy(&out[10], unit_id, 6);
}

void ns2_firmware_build_command_info(uint8_t controller_type, uint8_t out[12]) {
    memset(out, 0, 12);
    memcpy(&out[0], active_profile.controller, 3);
    out[3] = controller_type;
    memcpy(&out[4], active_profile.bluetooth, 3);
    memcpy(&out[8], active_profile.dsp, 3);
}

bool ns2_firmware_profile_flash_byte(uint32_t address, uint8_t *value) {
#if NS2_PRO_UPDATED_STATE
    /*
     * Observed as 00 00 00 00 on an updated retail controller and erased on
     * factory firmware.  Its exact semantic name remains unknown, so do not
     * infer or fabricate any surrounding flash contents.
     */
    if (address >= 0x1FD010u && address < 0x1FD014u) {
        *value = 0x00;
        return true;
    }
#else
    (void)address;
#endif
    (void)value;
    return false;
}

void ns2_firmware_diagnostics_reset(void) {
    memset(&diagnostics, 0, sizeof(diagnostics));
}

void ns2_firmware_diagnostics_record_ep0(void) {
    increment_saturating_u16(&diagnostics.ep0_info_queries);
}

void ns2_firmware_diagnostics_record_command(void) {
    increment_saturating_u16(&diagnostics.command_info_queries);
}

void ns2_firmware_diagnostics_record_read(uint8_t subcommand, uint32_t address,
                                          uint8_t length) {
    for (uint8_t i = 0; i < diagnostics.read_count; i++) {
        ns2_firmware_read_trace_entry_t *entry = &diagnostics.reads[i];
        if (entry->subcommand == subcommand && entry->address == address &&
            entry->length == length) {
            increment_saturating_u16(&entry->count);
            return;
        }
    }

    if (diagnostics.read_count == NS2_FIRMWARE_READ_TRACE_CAPACITY) {
        increment_saturating_u16(&diagnostics.dropped_reads);
        return;
    }

    ns2_firmware_read_trace_entry_t *entry = &diagnostics.reads[diagnostics.read_count++];
    entry->subcommand = subcommand;
    entry->address = address;
    entry->length = length;
    entry->count = 1;
}

void ns2_firmware_diagnostics_snapshot(ns2_firmware_diagnostics_t *out) {
    *out = diagnostics;
}

static size_t json_append(char *out, size_t out_size, size_t used,
                          const char *format, ...) {
    if (out_size == 0 || used >= out_size - 1) return used;

    va_list args;
    va_start(args, format);
    int written = vsnprintf(out + used, out_size - used, format, args);
    va_end(args);
    if (written < 0) return used;

    size_t available = out_size - used;
    if ((size_t)written >= available) return out_size - 1;
    return used + (size_t)written;
}

size_t ns2_firmware_diagnostics_format_json(char *out, size_t out_size) {
    if (out_size == 0) return 0;
    out[0] = '\0';

    ns2_firmware_diagnostics_t snapshot;
    ns2_firmware_diagnostics_snapshot(&snapshot);
    size_t used = json_append(
        out, out_size, 0,
        "{\"profile\":{\"controller\":\"%u.%u.%u\",\"bluetooth\":\"%u.%u.%u\","
        "\"dsp\":\"%u.%u.%u\",\"updated_state\":%s,\"runtime_override\":%s},"
        "\"ep0\":%u,\"command10\":%u,"
        "\"dropped\":%u,\"reads\":[",
        active_profile.controller[0], active_profile.controller[1],
        active_profile.controller[2], active_profile.bluetooth[0],
        active_profile.bluetooth[1], active_profile.bluetooth[2],
        active_profile.dsp[0], active_profile.dsp[1],
        active_profile.dsp[2], NS2_PRO_UPDATED_STATE ? "true" : "false",
        runtime_override_active ? "true" : "false",
        snapshot.ep0_info_queries, snapshot.command_info_queries,
        snapshot.dropped_reads);

    for (uint8_t i = 0; i < snapshot.read_count; i++) {
        const ns2_firmware_read_trace_entry_t *entry = &snapshot.reads[i];
        used = json_append(
            out, out_size, used,
            "%s{\"sub\":%u,\"address\":\"0x%08lX\",\"length\":%u,\"count\":%u}",
            i ? "," : "", entry->subcommand, (unsigned long)entry->address,
            entry->length, entry->count);
    }
    return json_append(out, out_size, used, "]}");
}
