#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Persistent settings + the configuration-mode command protocol.

// Load settings from flash into RAM (or defaults if absent/invalid).
// Call once from core0 before launching core1.
void config_load(void);

// True only on the boot that consumed a newly flashed install-reset marker.
// Core1 uses this after BTstack initializes its now-empty TLV store to persist
// the post-install pairing lock before enabling controller admission.
bool config_install_reset_performed(void);

// Service the configuration-mode CDC serial link (read command lines, reply
// JSON). Called from the USB core (core0) while in config mode.
void config_cdc_task(void);

// Run ONE management command and capture its reply into `out_buffer`.
//
// Returns the reply's TRUE length, even when longer than `capacity` — a
// diagnostic that reported the truncated size would hide an oversized reply,
// which is precisely what it exists to catch.
//
// Exists so the always-available UART console can reach the full management
// surface. The UART dispatcher (src/ns2_uart_diag.c) knows only a few `kbm`
// verbs; everything else lives in src/config.c behind BLE or the CDC Config
// personality. That gap meant a wire-format defect in a management reply could
// not be measured on the bench at all.
//
// core0 only, from the same task loop as config_cdc_task(): it borrows that
// path's line buffer.
size_t config_execute_captured(const char *command, char *out_buffer,
                               size_t capacity);

// Execute at most one pending wireless (BLE) management command on core0, using
// the same parser/persistence as the CDC path. Self-gating: a no-op unless a
// complete command is queued (only happens when the config/management BLE
// service is armed and a client wrote). Called UNCONDITIONALLY from the core0
// main loop so in-band management works in a normal controller personality, not
// just CDC Config. docs/bluetooth/in-band-management-plan.md C2.
void config_wireless_task(void);

// Arm the deferred settings flash write, exactly as the `save` command does.
// For surfaces that are not the config command parser (the UART diagnostic
// channel); the write itself is still performed by config_service_save() on
// core1, so there is one persistence path and one record composition. Saves the
// COMPLETE settings record, including the live keyboard/mouse configuration.
uint32_t config_request_save(void);

// Perform a pending flash save, if requested. MUST be called from core1 (the
// Bluetooth core), which holds the multicore-lockout requester role used to park
// core0 during the flash write. Safe to call every control tick.
void config_service_save(void);

// Read the configured Pro Controller 2 body colour.
void config_get_body_color(uint8_t rgb[3]);

// Read one configured Joy-Con 2 highlight/accent colour. Each side advertises
// its own value in the factory identity; the active Joy-Con personality also
// uses that value for supported physical-controller lightbars.
void config_get_joycon2_accent(bool right, uint8_t rgb[3]);

// Switch 2 wake identity learned from the console's ordinary USB Bluetooth-
// pairing command (0x15/01). Addresses are kept in Nintendo's wire order:
// least-significant byte first, exactly as they appear in the pairing command
// and wake advertisement payload. The controller address is reversed only at
// the BTstack boundary, where bd_addr_t uses display order.
#define CONFIG_WAKE_MAX_HOSTS 2
typedef struct {
    uint16_t product_id;
    uint8_t controller_addr_wire[6];
    uint8_t host_addr_wire[CONFIG_WAKE_MAX_HOSTS][6];
    uint8_t host_count;
} config_wake_identity_t;

// Returns false until a complete Switch 2 USB pairing exchange has supplied a
// usable wake identity. Safe across cores.
bool config_get_wake_identity(config_wake_identity_t *out);

// ---------------------------------------------------------------------------
// Management companions
// ---------------------------------------------------------------------------
// The adapter's only DURABLE role evidence. Everything else about a peer's role
// is live: a bond alone cannot say whether it belongs to a controller or to a
// management client, so before this a management bond that was not connected
// right now was reported as role `unknown` -- which a companion correctly reads
// as a paired controller.

// Remember an authenticated management companion by its durable identity.
// Persisted only when the set actually changes, so reconnecting is free.
void config_note_management_companion(const uint8_t addr[6], uint8_t addr_type);

// Is this durable identity a remembered management companion?
bool config_is_management_companion(const uint8_t addr[6]);

// Drop a remembered companion, because its credential was deleted. The
// remembered role must never outlive the bond it describes.
void config_forget_management_companion(const uint8_t addr[6]);

// Enumerate remembered companions. Returns false when `index` holds none, so a
// caller can walk 0..CONFIG_MGMT_COMPANIONS_MAX-1 without knowing the layout.
bool config_management_companion_at(uint8_t index, uint8_t addr[6],
                                    uint8_t *addr_type);

// Store a validated identity in RAM and schedule a deferred flash save. The
// delay deliberately keeps flash erase/programming out of the console's
// timing-sensitive USB pairing handshake.
void config_store_wake_identity(const config_wake_identity_t *identity);

#endif
