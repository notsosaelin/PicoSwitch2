#ifndef _NS2_MOTION_SEAM_H_
#define _NS2_MOTION_SEAM_H_

#include <stdint.h>

// Per-controller-family IMU axis transform into the Pro2 carrier frame.
//
// Each motion source owns one row, so changing one family's axes cannot affect
// another. Add a row when adding a motion source; never edit an existing row to
// fix a different controller.
//
// src[i] = which source axis feeds output slot i; sign[i] = its polarity.
// Accel and gyro are separate because a source's gyro polarity need not follow
// the right-hand rule about its own accel axes (the DualSense's does not).
typedef struct {
    int8_t accel_src[3], accel_sign[3];
    int8_t gyro_src[3],  gyro_sign[3];
} ns2_motion_seam_t;

const ns2_motion_seam_t *ns2_motion_seam_for(uint8_t motion_source);

// Event-frame accel/gyro -> Pro2 carrier frame. Accel is additionally halved:
// sources publish 8192 counts/g, the genuine PC2 carrier is 4096 counts/g.
// Gyro passes 1:1 (16.384 counts/dps end to end).
void ns2_motion_seam_apply(uint8_t motion_source,
                           const int16_t accel_in[3],
                           const int16_t gyro_in[3],
                           int16_t accel_out[3],
                           int16_t gyro_out[3]);

#endif  // _NS2_MOTION_SEAM_H_
