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
#define CONFIG_PERSIST_VERSION 13u

// How many management companions an adapter remembers.
//
// Four covers every realistic setup -- a phone, a tablet and two PCs -- and the
// table is the ONLY durable role evidence this firmware has, so it is
// deliberately small and bounded rather than growing with the bond database.
#define CONFIG_MGMT_COMPANIONS_MAX 4u

// One remembered management companion.
//
// Stored as the durable IDENTITY address, never a connection address: under LE
// privacy the connection address is a rotating RPA that is never equal to
// anything durable, so persisting one would store a fact that is false within
// fifteen minutes.
typedef struct {
    uint8_t valid;
    uint8_t addr_type;
    uint8_t addr[6];
} config_mgmt_companion_t;

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

// Schema 11's mouse-translation settings. Frozen: schema 12 appends
// `anti_deadzone` to the live ns2_kbm_mouse_config_t, which RESIZES it, so the
// v11 bytes on existing adapters can only be read through their own layout.
// Never edit it.
typedef struct {
    uint16_t sensitivity_x;
    uint16_t sensitivity_y;
    uint16_t recenter_ms;
    uint8_t invert_x;
    uint8_t invert_y;
} ns2_kbm_mouse_config_v11_t;

// Schema 11's keyboard/mouse block. The profile table is shared with the live
// type on purpose -- it did not change -- and config_persist.c static-asserts
// that, so a future edit to the mapping tables cannot silently redefine what
// v11 bytes mean. Never edit it.
typedef struct {
    uint8_t mode;
    uint8_t reserved[3];
    ns2_kbm_profile_overrides_t profiles[NS2_KBM_PROFILE_COUNT];
    ns2_kbm_mouse_config_v11_t mouse;
} ns2_kbm_config_v11_t;

// Schema 11 (PicoSwitch2 v2.1.x). Added the Bluetooth Keyboard / Keyboard +
// Mouse configuration. Frozen: it describes bytes that already exist on real
// adapters. Never edit it.
typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t body_color[3];
    uint8_t joycon2_left_accent[3];
    uint8_t joycon2_right_accent[3];
    uint8_t wake_valid;
    config_wake_identity_t wake_identity;
    ns2_kbm_config_v11_t kbm;
} config_record_v11_t;

// Schema 12 adds the mouse-to-stick anti-deadzone (inside the KB/M block).
// Schema 12 (the KB/M mouse block's final 10-byte form). Frozen: it describes
// bytes that already exist on real adapters. Never edit it.
typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t body_color[3];
    uint8_t joycon2_left_accent[3];
    uint8_t joycon2_right_accent[3];
    uint8_t wake_valid;
    config_wake_identity_t wake_identity;
    ns2_kbm_config_t kbm;
} config_record_v12_t;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t body_color[3];           // Pro2 body/lightbar R,G,B
    uint8_t joycon2_left_accent[3];  // Joy-Con 2 L highlight/lightbar
    uint8_t joycon2_right_accent[3]; // Joy-Con 2 R highlight/lightbar
    uint8_t wake_valid;
    config_wake_identity_t wake_identity;
    ns2_kbm_config_t kbm;            // v11; mouse block extended in v12

    // v13: which bonds belong to a management companion rather than a
    // controller.
    //
    // Before this, role was live evidence only -- mgmt_peers.h said so -- so a
    // management bond that was not connected at that moment was reported with
    // role `unknown`. A companion reads a bonded, non-connected, roleless peer
    // as a PAIRED CONTROLLER, which is correct for a controller and wrong here:
    // a second management client showed up in the controller list as a device
    // the user never paired, offering to forget a management relationship.
    //
    // This is remembered fact, not inference: an entry is written only after a
    // management session has actually authenticated under a resolved identity.
    config_mgmt_companion_t mgmt_companions[CONFIG_MGMT_COMPANIONS_MAX];
} config_record_t;

typedef enum {
    CONFIG_PERSIST_CURRENT = 0,    // the stored record is already this schema
    CONFIG_PERSIST_MIGRATED = 1,   // an older schema was upgraded in place
    CONFIG_PERSIST_DEFAULTED = 2,  // nothing usable was stored
    CONFIG_PERSIST_REPAIRED = 3,   // loaded, but some field failed validation
} config_persist_load_t;

// Canonical factory defaults for every field.
void config_persist_defaults(config_record_t *out);

// ---------------------------------------------------------------------------
// Management-companion membership
// ---------------------------------------------------------------------------
// Kept here, beside the record they live in, and written as pure functions so
// the rules are host-testable without a Bluetooth stack. The firmware calls
// them; it does not reimplement them.

// Remember `addr` as a management companion. Returns true when the table
// CHANGED, which is the caller's signal to persist -- a companion reconnecting
// must not cost a flash write on every session.
//
// Oldest-first eviction: the table is small on purpose, and a companion that
// has not been seen for longer than three others is the one whose absence costs
// least. Re-registering an existing entry is idempotent and reports no change.
bool config_mgmt_companion_remember(config_mgmt_companion_t *table,
                                    uint8_t capacity, const uint8_t addr[6],
                                    uint8_t addr_type);

// Is `addr` a remembered management companion?
bool config_mgmt_companion_known(const config_mgmt_companion_t *table,
                                 uint8_t capacity, const uint8_t addr[6]);

// Drop `addr` from the table. Returns true when it CHANGED.
//
// Called when a bond is deleted: the remembered role must not outlive the
// credential it describes, or a forgotten companion would keep a row the user
// can neither see nor remove.
bool config_mgmt_companion_forget(config_mgmt_companion_t *table,
                                  uint8_t capacity, const uint8_t addr[6]);

// Interpret a stored record. `stored` may be any bytes at all — including an
// erased sector or a future schema — and this never reads past `stored_len`.
// The result is always a fully valid record.
config_persist_load_t config_persist_load(const void *stored, uint32_t stored_len,
                                          config_record_t *out);

#endif  // _CONFIG_PERSIST_H_
