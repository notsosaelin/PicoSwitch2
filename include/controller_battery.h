#ifndef CONTROLLER_BATTERY_H
#define CONTROLLER_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

// Normalized result from a controller-native battery field.
typedef struct {
    uint8_t level;       // 0..100 percent
    bool charging;
} controller_battery_t;

// Controller-native HID battery decoders. False means the raw status does not
// contain a trustworthy level and the caller should retain its previous value.
bool controller_battery_decode_ds3(uint8_t raw, controller_battery_t *out);
bool controller_battery_decode_ds4(uint8_t raw, controller_battery_t *out);
bool controller_battery_decode_ds5(uint8_t raw, controller_battery_t *out);
bool controller_battery_decode_switch_pro(uint8_t battery_conn,
                                          controller_battery_t *out);
bool controller_battery_decode_wii_u_pro(uint8_t status,
                                         controller_battery_t *out);
bool controller_battery_decode_wiimote(uint8_t raw,
                                       controller_battery_t *out);

// Nintendo USB output encoders. Unknown input deliberately preserves the
// project's previous safe default: full battery on a wired/external source.
uint8_t controller_battery_switch1_connection_info(bool valid,
                                                    uint8_t level);
uint8_t controller_battery_switch2_power_info(bool valid, uint8_t level,
                                              bool charging);

#endif  // CONTROLLER_BATTERY_H
