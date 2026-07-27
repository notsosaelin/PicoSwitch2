#include "ns2_motion_seam.h"

#include "switch_pro.h"  // SWITCH_MOTION_SOURCE_*

// The DualSense row is the hardware-validated reference (paired native-Pro2/DS5
// capture, 2026-07-22); its derivation is documented at the call site in
// ns2_seam.c. Every other row is stated relative to its own sensor frame.
//
// EVERY ROW MUST HAVE DETERMINANT +1 (parity of the src permutation times the
// product of the signs). A row describes a physical sensor remount, which is a
// rotation; a determinant of -1 is a reflection and cannot describe any real
// mounting. Gravity cannot reveal the error -- a single vector looks correct
// reflected -- so an improper row passes every static check and then behaves
// wrongly only under rotation. That is exactly what happened to the SWITCH1 row
// (det -1 until 2026-07-27): its accelerometer matched a genuine Pro Controller
// 2 to within 1% while its gyro produced no horizontal aim at all.
// tools/test_ns2_motion_seam.c enforces this.
static const ns2_motion_seam_t NS2_MOTION_SEAMS[] = {
    // GENERIC: frame unknown; pass through in the DualSense arrangement.
    [SWITCH_MOTION_SOURCE_GENERIC] = {
        {0, 2, 1}, {1, -1, 1}, {0, 2, 1}, {1, -1, 1} },

    [SWITCH_MOTION_SOURCE_DUALSENSE] = {
        {0, 2, 1}, {1, -1, 1}, {0, 2, 1}, {1, -1, 1} },

    // WII: wiimote_bt.c already publishes in the DualSense arrangement, so this
    // row matches the DualSense one. See docs/bluetooth/wii-motion.md.
    [SWITCH_MOTION_SOURCE_WII] = {
        {0, 2, 1}, {1, -1, 1}, {0, 2, 1}, {1, -1, 1} },

    // SWITCH1: raw LSM6DS3 axes (X longitudinal, Y +left, Z +face normal), per
    // Linux hid-nintendo, which applies no transform to the Pro Controller.
    //
    // Slot 2 is measured, not assumed: a resting Pro Controller puts gravity on
    // Pro2 accel[2] at +4245 (4096 counts/g), matching the genuine Pro
    // Controller 2 capture's +4279/+4309. Slots 0 and 1 then follow from the
    // frame plus the determinant rule below.
    //
    // Slots 0/1 resolved on hardware 2026-07-27. Only two rows keep slot 2 and
    // stay proper: {-1,1,1} = (+R,+F,+U) and {1,-1,1} = (-R,-F,+U). They differ
    // by a 180 degree yaw, so they share yaw and invert pitch and roll relative
    // to each other. {1,-1,1} restored horizontal aim but left pitch inverted,
    // which selects the other one.
    [SWITCH_MOTION_SOURCE_SWITCH1] = {
        {1, 0, 2}, {-1, 1, 1}, {1, 0, 2}, {-1, 1, 1} },
};

#define NS2_MOTION_SEAM_COUNT \
    (sizeof(NS2_MOTION_SEAMS) / sizeof(NS2_MOTION_SEAMS[0]))

const ns2_motion_seam_t *ns2_motion_seam_for(uint8_t motion_source)
{
    if (motion_source >= NS2_MOTION_SEAM_COUNT)
        motion_source = SWITCH_MOTION_SOURCE_GENERIC;
    return &NS2_MOTION_SEAMS[motion_source];
}

static int16_t seam_clamp16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

void ns2_motion_seam_apply(uint8_t motion_source,
                           const int16_t accel_in[3],
                           const int16_t gyro_in[3],
                           int16_t accel_out[3],
                           int16_t gyro_out[3])
{
    const ns2_motion_seam_t *s = ns2_motion_seam_for(motion_source);
    for (unsigned i = 0; i < 3; i++) {
        accel_out[i] = seam_clamp16(
            (int32_t)accel_in[s->accel_src[i]] * s->accel_sign[i] / 2);
        gyro_out[i] = seam_clamp16(
            (int32_t)gyro_in[s->gyro_src[i]] * s->gyro_sign[i]);
    }
}
