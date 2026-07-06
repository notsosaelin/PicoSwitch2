#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdint.h>

// Persistent settings + the configuration-mode command protocol.

// Load settings from flash into RAM (or defaults if absent/invalid).
// Call once from core0 before launching core1.
void config_load(void);

// Service the configuration-mode CDC serial link (read command lines, reply
// JSON). Called from the USB core (core0) while in config mode.
void config_cdc_task(void);

// Perform a pending flash save, if requested. MUST be called from core1 (the
// Bluetooth core), which holds the multicore-lockout requester role used to park
// core0 during the flash write. Safe to call every control tick.
void config_service_save(void);

// Read a player's configured lightbar colour (core1, when a controller connects).
void config_get_lightbar(uint8_t player, uint8_t rgb[3]);

// Copy a platform family's button remap (SRC_COUNT entries, each a DST_* value)
// for the input mapping (core1, per controller report).
void config_get_button_map(uint8_t family, uint8_t map_out[]);

// Joypad-os stack per-family remap (NS2_SRC_COUNT entries, each an NS2_DST_* value).
// Read by the seam per controller report (core1).
void config_get_ns2_map(uint8_t family, uint8_t map_out[]);

#endif
