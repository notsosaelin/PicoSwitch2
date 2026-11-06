#ifndef XBOX_RUMBLE_H
#define XBOX_RUMBLE_H

#include <stdint.h>

#define XBOX_RUMBLE_REPORT_ID 0x03
#define XBOX_RUMBLE_DATA_LEN  8

// Build the common Xbox Bluetooth rumble payload used by the dedicated BLE/Classic
// drivers and by the generic-driver fallback (notably Elite Series 2).
//
// Byte 0 is a motor-update mask, so STOP selects both main motors and writes zero
// magnitudes; an all-zero packet updates nothing and cannot stop a latched motor.
// Active pulses use the short envelope established by the GameCube-mode hardware
// investigation; the previous 2.55-second pulse smeared rapid ON/OFF modulation.
void xbox_rumble_build_payload(uint8_t left, uint8_t right,
                               uint8_t out[XBOX_RUMBLE_DATA_LEN]);

#endif  // XBOX_RUMBLE_H
