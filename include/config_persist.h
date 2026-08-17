#ifndef _CONFIG_PERSIST_H_
#define _CONFIG_PERSIST_H_

// Persistent settings record layout, defaults, and schema migration.
//
// Split out of config.c so the schema and its migration are host testable: the
// one failure this file exists to prevent is a new version silently
// reinterpreting an existing installation's bytes as different settings. That
// cannot be caught by a firmware build, only by a test that owns both layouts.
//
// Rules for adding a field:
//   * append it to config_record_t, never reorder or resize an existing field
//   * bump CONFIG_PERSIST_VERSION
//   * freeze the previous layout as a config_record_vN_t and extend
//     config_persist_load() to migrate it
//   * give the new field a deterministic default in config_persist_defaults()

#include <stdbool.h>
#include <stdint.h>

#include "config.h"   // config_wake_identity_t
#include "ns2_kbm.h"  // ns2_kbm_config_t

#define CONFIG_PERSIST_MAGIC 0x50535731u  // 'PSW1'
#define CONFIG_PERSIST_VERSION 11u

// Schema 10 (PicoSwitch2 v2.0.0 and earlier). Frozen: it describes bytes that
// already exist on real adapters. Never edit it.
typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t body_color[3];
    uint8_t joycon2_left_accent[3];
    uint8_t joycon2_right_accent[3];
    uint8_t wake_valid;
    config_wake_identity_t wake_identity;
} config_record_v10_t;

// Schema 11 adds the Bluetooth Keyboard / Keyboard + Mouse configuration.
typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t body_color[3];           // Pro2 body/lightbar R,G,B
    uint8_t joycon2_left_accent[3];  // Joy-Con 2 L highlight/lightbar
    uint8_t joycon2_right_accent[3]; // Joy-Con 2 R highlight/lightbar
    uint8_t wake_valid;
    config_wake_identity_t wake_identity;
    ns2_kbm_config_t kbm;            // v11
} config_record_t;

typedef enum {
    CONFIG_PERSIST_CURRENT = 0,    // the stored record is already this schema
    CONFIG_PERSIST_MIGRATED = 1,   // an older schema was upgraded in place
    CONFIG_PERSIST_DEFAULTED = 2,  // nothing usable was stored
    CONFIG_PERSIST_REPAIRED = 3,   // loaded, but some field failed validation
} config_persist_load_t;

// Canonical factory defaults for every field.
void config_persist_defaults(config_record_t *out);

// Interpret a stored record. `stored` may be any bytes at all — including an
// erased sector or a future schema — and this never reads past `stored_len`.
// The result is always a fully valid record.
config_persist_load_t config_persist_load(const void *stored, uint32_t stored_len,
                                          config_record_t *out);

#endif  // _CONFIG_PERSIST_H_
