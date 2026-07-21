/*
 * Coherent Switch 2 Pro Controller firmware identity.
 *
 * The console can obtain the controller, Bluetooth and DSP versions through
 * more than one protocol surface.  Keep the selected versions here so a build
 * cannot accidentally identify itself differently over EP0 and command 0x10.
 * CMake supplies these macros for normal firmware builds; the defaults also
 * make the pure-C host test self-contained.
 */
#ifndef NS2_FIRMWARE_PROFILE_H
#define NS2_FIRMWARE_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef NS2_PRO_FW_MAJOR
#define NS2_PRO_FW_MAJOR 2
#endif
#ifndef NS2_PRO_FW_MINOR
#define NS2_PRO_FW_MINOR 1
#endif
#ifndef NS2_PRO_FW_MICRO
#define NS2_PRO_FW_MICRO 4
#endif

#ifndef NS2_PRO_BT_MAJOR
#define NS2_PRO_BT_MAJOR 12
#endif
#ifndef NS2_PRO_BT_MINOR
#define NS2_PRO_BT_MINOR 0
#endif
#ifndef NS2_PRO_BT_MICRO
#define NS2_PRO_BT_MICRO 0
#endif

#ifndef NS2_PRO_DSP_MAJOR
#define NS2_PRO_DSP_MAJOR 0
#endif
#ifndef NS2_PRO_DSP_MINOR
#define NS2_PRO_DSP_MINOR 2
#endif
#ifndef NS2_PRO_DSP_MICRO
#define NS2_PRO_DSP_MICRO 3
#endif

/* A DSP-bearing profile represents post-update firmware. */
#ifndef NS2_PRO_UPDATED_STATE
#define NS2_PRO_UPDATED_STATE 1
#endif

typedef struct {
    uint8_t controller[3];
    uint8_t bluetooth[3];
    uint8_t dsp[3];
} ns2_firmware_profile_t;

#define NS2_FIRMWARE_READ_TRACE_CAPACITY 24

typedef struct {
    uint32_t address;
    uint16_t count;
    uint8_t subcommand;
    uint8_t length;
} ns2_firmware_read_trace_entry_t;

typedef struct {
    uint16_t ep0_info_queries;
    uint16_t command_info_queries;
    uint16_t dropped_reads;
    uint8_t read_count;
    ns2_firmware_read_trace_entry_t reads[NS2_FIRMWARE_READ_TRACE_CAPACITY];
} ns2_firmware_diagnostics_t;

extern const ns2_firmware_profile_t ns2_firmware_profile;

/*
 * Active identity used by both version-bearing replies. The active profile
 * normally equals ns2_firmware_profile. UART diagnostics may replace it in
 * RAM for a controlled console A/B test; it is never written to flash and a
 * power cycle always restores the compiled profile.
 */
const ns2_firmware_profile_t *ns2_firmware_profile_active(void);
bool ns2_firmware_profile_runtime_override_active(void);
void ns2_firmware_profile_set_runtime(const uint8_t controller[3],
                                      const uint8_t bluetooth[3],
                                      const uint8_t dsp[3]);
void ns2_firmware_profile_reset_runtime(void);

/* Build the two confirmed version-bearing replies from the same profile. */
void ns2_firmware_build_ep0_info(uint8_t out[16], const uint8_t unit_id[6]);
void ns2_firmware_build_command_info(uint8_t controller_type, uint8_t out[12]);

/*
 * Return true when the selected profile defines a byte at this flash address.
 * This deliberately exposes only observed state bytes, never synthetic image
 * or DSP data that could imply we hold a complete, verifiable firmware image.
 */
bool ns2_firmware_profile_flash_byte(uint32_t address, uint8_t *value);

/* Read-only prompt diagnostics; no command 0x0D payload or memory data is retained. */
void ns2_firmware_diagnostics_reset(void);
void ns2_firmware_diagnostics_record_ep0(void);
void ns2_firmware_diagnostics_record_command(void);
void ns2_firmware_diagnostics_record_read(uint8_t subcommand, uint32_t address,
                                          uint8_t length);
void ns2_firmware_diagnostics_snapshot(ns2_firmware_diagnostics_t *out);
size_t ns2_firmware_diagnostics_format_json(char *out, size_t out_size);

#endif
