#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_firmware_profile.h"

static void test_default_profile(void) {
    static const uint8_t expected_controller[3] = {
        NS2_PRO_FW_MAJOR, NS2_PRO_FW_MINOR, NS2_PRO_FW_MICRO};
    static const uint8_t expected_bluetooth[3] = {
        NS2_PRO_BT_MAJOR, NS2_PRO_BT_MINOR, NS2_PRO_BT_MICRO};
    static const uint8_t expected_dsp[3] = {
        NS2_PRO_DSP_MAJOR, NS2_PRO_DSP_MINOR, NS2_PRO_DSP_MICRO};
    assert(memcmp(ns2_firmware_profile.controller, expected_controller, 3) == 0);
    assert(memcmp(ns2_firmware_profile.bluetooth, expected_bluetooth, 3) == 0);
    assert(memcmp(ns2_firmware_profile.dsp, expected_dsp, 3) == 0);
}

static void test_version_surfaces_match(void) {
    static const uint8_t unit_id[6] = {0x9E, 0x2B, 0xAB, 0xAB, 0xA9, 0x3C};
    uint8_t ep0[16];
    uint8_t command[12];
    ns2_firmware_build_ep0_info(ep0, unit_id);
    ns2_firmware_build_command_info(0x02, command);

    assert(memcmp(&ep0[0], &command[0], 3) == 0);
    assert(ep0[6] == command[4]);
    assert(command[3] == 0x02);
    assert(memcmp(&command[4], ns2_firmware_profile.bluetooth, 3) == 0);
    assert(memcmp(&command[8], ns2_firmware_profile.dsp, 3) == 0);
    assert(memcmp(&ep0[10], unit_id, 6) == 0);
}

static void test_runtime_profile(void) {
    static const uint8_t controller[3] = {255, 254, 253};
    static const uint8_t bluetooth[3] = {252, 251, 250};
    static const uint8_t dsp[3] = {249, 248, 247};
    static const uint8_t unit_id[6] = {1, 2, 3, 4, 5, 6};
    uint8_t ep0[16];
    uint8_t command[12];

    assert(!ns2_firmware_profile_runtime_override_active());
    ns2_firmware_profile_set_runtime(controller, bluetooth, dsp);
    assert(ns2_firmware_profile_runtime_override_active());
    assert(memcmp(ns2_firmware_profile_active()->controller, controller, 3) == 0);
    assert(memcmp(ns2_firmware_profile_active()->bluetooth, bluetooth, 3) == 0);
    assert(memcmp(ns2_firmware_profile_active()->dsp, dsp, 3) == 0);

    ns2_firmware_build_ep0_info(ep0, unit_id);
    ns2_firmware_build_command_info(0x02, command);
    assert(memcmp(ep0, controller, 3) == 0);
    assert(ep0[6] == bluetooth[0]);
    assert(memcmp(command, controller, 3) == 0);
    assert(memcmp(&command[4], bluetooth, 3) == 0);
    assert(memcmp(&command[8], dsp, 3) == 0);

    char json[512];
    ns2_firmware_diagnostics_format_json(json, sizeof(json));
    assert(strstr(json, "\"controller\":\"255.254.253\"") != NULL);
    assert(strstr(json, "\"runtime_override\":true") != NULL);

    ns2_firmware_profile_reset_runtime();
    assert(!ns2_firmware_profile_runtime_override_active());
    assert(memcmp(ns2_firmware_profile_active(), &ns2_firmware_profile,
                  sizeof(ns2_firmware_profile)) == 0);
}

static void test_sparse_updated_state(void) {
    uint8_t value = 0xA5;
    assert(!ns2_firmware_profile_flash_byte(0x1FD00Fu, &value));
    assert(value == 0xA5);
    for (uint32_t address = 0x1FD010u; address < 0x1FD014u; address++) {
        value = 0xA5;
#if NS2_PRO_UPDATED_STATE
        assert(ns2_firmware_profile_flash_byte(address, &value));
        assert(value == 0x00);
#else
        assert(!ns2_firmware_profile_flash_byte(address, &value));
        assert(value == 0xA5);
#endif
    }
    value = 0xA5;
    assert(!ns2_firmware_profile_flash_byte(0x1FD014u, &value));
    assert(value == 0xA5);
    assert(!ns2_firmware_profile_flash_byte(0x175000u, &value));
}

static void test_read_diagnostics(void) {
    ns2_firmware_diagnostics_t diag;
    ns2_firmware_diagnostics_reset();
    ns2_firmware_diagnostics_record_ep0();
    ns2_firmware_diagnostics_record_command();
    ns2_firmware_diagnostics_record_command();
    ns2_firmware_diagnostics_record_read(4, 0x1FD010u, 4);
    ns2_firmware_diagnostics_record_read(4, 0x1FD010u, 4);
    ns2_firmware_diagnostics_record_read(1, 0x175000u, 0x40);
    ns2_firmware_diagnostics_snapshot(&diag);

    assert(diag.ep0_info_queries == 1);
    assert(diag.command_info_queries == 2);
    assert(diag.dropped_reads == 0);
    assert(diag.read_count == 2);
    assert(diag.reads[0].subcommand == 4);
    assert(diag.reads[0].address == 0x1FD010u);
    assert(diag.reads[0].length == 4);
    assert(diag.reads[0].count == 2);
    assert(diag.reads[1].subcommand == 1);
    assert(diag.reads[1].address == 0x175000u);
    assert(diag.reads[1].length == 0x40);
    assert(diag.reads[1].count == 1);

    char json[1024];
    size_t length = ns2_firmware_diagnostics_format_json(json, sizeof(json));
    assert(length == strlen(json));
    char expected_version[64];
    snprintf(expected_version, sizeof(expected_version),
             "\"controller\":\"%u.%u.%u\"", NS2_PRO_FW_MAJOR,
             NS2_PRO_FW_MINOR, NS2_PRO_FW_MICRO);
    assert(strstr(json, expected_version) != NULL);
    assert(strstr(json, "\"runtime_override\":false") != NULL);
    assert(strstr(json, "\"ep0\":1") != NULL);
    assert(strstr(json, "\"command10\":2") != NULL);
    assert(strstr(json, "\"address\":\"0x001FD010\"") != NULL);
    assert(strstr(json, "\"count\":2") != NULL);
    assert(strstr(json, "\"address\":\"0x00175000\"") != NULL);

    char tiny[16];
    length = ns2_firmware_diagnostics_format_json(tiny, sizeof(tiny));
    assert(length == sizeof(tiny) - 1);
    assert(tiny[sizeof(tiny) - 1] == '\0');
}

int main(void) {
    test_default_profile();
    test_version_surfaces_match();
    test_runtime_profile();
    test_sparse_updated_state();
    test_read_diagnostics();
    puts("ns2_firmware_profile: all tests passed");
    return 0;
}
