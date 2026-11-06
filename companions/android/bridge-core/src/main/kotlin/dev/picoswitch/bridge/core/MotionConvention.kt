package dev.picoswitch.bridge.core

import kotlin.math.roundToInt

/**
 * THE canonical motion convention of the PicoSwitch Bridge.
 *
 * Motion is the one part of this contract where a platform convention can leak in
 * unnoticed and stay wrong for months, because a wrong axis still *works* — it
 * just aims sideways. So the convention is defined here, independently of any
 * host operating system, and every platform backend converts into it.
 *
 * ## Frame
 *
 * Right-handed, expressed in the frame the user is HOLDING the device, not the
 * frame the device was manufactured in:
 *
 * ```text
 *   +X  right, along the top edge of the screen
 *   +Y  up, toward the top of the screen
 *   +Z  out of the screen, toward the user
 * ```
 *
 * Gyroscope values are angular rate about those axes, positive counter-clockwise
 * when looking from the positive end of the axis toward the origin (the standard
 * right-hand rule). Accelerometer values are proper acceleration and are
 * GRAVITY-INCLUSIVE: a device lying face up on a table reads about `+1 g` on Z,
 * not zero. The firmware's stillness/bias tracking depends on that.
 *
 * This is deliberately the same frame the Switch 2 carrier expects, which is why
 * the firmware's `SWITCH_MOTION_SOURCE_ANDROID` seam row is the identity. It is
 * not "Android's frame promoted to a standard": Android happens to define its
 * sensor frame the same way, in the device's NATURAL orientation, and the
 * remaining difference — natural orientation versus held orientation — is exactly
 * what [ScreenOrientation] exists to remove.
 *
 * ## Units
 *
 * Fixed-point counts, not SI, because the wire contract is fixed point and a
 * float intermediate would put rounding in two places:
 *
 * - acceleration: `8192` counts per g
 * - angular rate: `16.384` counts per degree/second
 *
 * These match what the adapter already receives from a DualSense, so a host
 * device reuses the hardware-validated motion translation instead of adding a
 * second scaling convention. [MotionScale] converts from SI, which is what most
 * platform sensor APIs report.
 *
 * ## Timestamps
 *
 * [ControllerMotion.timestampTicks] is a free-running counter in **100 us** ticks,
 * truncated to 16 bits on the wire (wraps every 6.5536 s; the firmware takes the
 * delta in the field's own modulus).
 *
 * It MUST come from the sensor sample, never from send time. The report cadence
 * is faster than a typical IMU delivers, so the same physical sample is sent more
 * than once; the adapter de-duplicates on this field and only advances its motion
 * sequence when it genuinely changes. Stamping at send time makes every repeat
 * look like a fresh IMU frame, which the console integrates as real movement.
 *
 * 100 us rather than 1 ms because the adapter integrates angular rate against
 * this clock: at a 125 Hz cadence a 1 ms quantum is 12.5% of the interval, the
 * same order as the Bluetooth arrival jitter the timestamp exists to eliminate.
 *
 * ## Where conversion happens
 *
 * ```text
 * platform sensor frame + platform units
 *          v   platform backend (device transform, orientation, MotionScale)
 * CANONICAL BRIDGE MOTION  (this file)
 *          v   protocol layer
 * PicoSwitch wire report
 * ```
 *
 * Nothing after the platform backend re-interprets axes or units.
 */
object MotionConvention {
    const val ACCEL_COUNTS_PER_G = 8192.0
    const val GYRO_COUNTS_PER_DPS = 16.384
    const val GRAVITY_MS2 = 9.80665
    const val DEGREES_PER_RADIAN = 57.2957795

    /** Wire ticks per second for [ControllerMotion.timestampTicks]. */
    const val TIMESTAMP_TICKS_PER_SECOND = 10_000L

    /** Nanoseconds per tick, for backends whose sensor clock is in nanoseconds. */
    const val TIMESTAMP_NANOS_PER_TICK = 100_000L

    /** The field is 16 bits on the wire; wrap is expected and handled downstream. */
    const val TIMESTAMP_MASK = 0xFFFF
}

/** SI sensor values -> canonical bridge counts. See [MotionConvention]. */
object MotionScale {
    /** Proper acceleration in m/s^2, gravity-inclusive. */
    fun accelCounts(metersPerSecondSquared: Float): Int =
        clamp16(metersPerSecondSquared * (MotionConvention.ACCEL_COUNTS_PER_G / MotionConvention.GRAVITY_MS2))

    /** Angular rate in rad/s. */
    fun gyroCounts(radiansPerSecond: Float): Int =
        clamp16(radiansPerSecond * MotionConvention.DEGREES_PER_RADIAN * MotionConvention.GYRO_COUNTS_PER_DPS)

    /** Truncate a free-running nanosecond sensor clock to the wire tick field. */
    fun timestampTicks(sensorTimestampNanos: Long): Int =
        ((sensorTimestampNanos / MotionConvention.TIMESTAMP_NANOS_PER_TICK) and
            MotionConvention.TIMESTAMP_MASK.toLong()).toInt()

    fun clamp16(value: Double): Int = value.roundToInt().coerceIn(-32768, 32767)
}

/**
 * Device natural frame -> held (screen) frame.
 *
 * Every platform that can rotate its display has this problem: sensors are
 * reported in the device's manufactured orientation, which is not the orientation
 * the user is holding. A phone held sideways to play, or a handheld whose natural
 * orientation is already landscape, would otherwise send pitch as roll and aim
 * would be rotated 90 degrees.
 *
 * Rotation is expressed in DEGREES, which is a physical quantity rather than any
 * platform's rotation constant. A backend converts its own constant
 * (`Surface.ROTATION_90`, a Win32 `DMDO_*`, a Wayland transform) to degrees and
 * calls this. That keeps the shared layer from ever needing to know how a given
 * OS enumerates display rotation.
 *
 * Pure and integer-only so it is exhaustively unit-testable, and applied to
 * counts rather than SI values so it is an exact sign/axis permutation with no
 * second rounding step.
 */
object ScreenOrientation {
    fun remapX(x: Int, y: Int, rotationDegrees: Int): Int = when (normalize(rotationDegrees)) {
        90 -> -y
        180 -> -x
        270 -> y
        else -> x
    }

    fun remapY(x: Int, y: Int, rotationDegrees: Int): Int = when (normalize(rotationDegrees)) {
        90 -> x
        180 -> -y
        270 -> -x
        else -> y
    }

    /** Z is the screen normal and is unchanged by any screen rotation. */
    fun remapZ(z: Int): Int = z

    /** Apply the full remap to one canonical sample. */
    fun apply(sample: ControllerMotion, rotationDegrees: Int): ControllerMotion = sample.copy(
        gyroX = remapX(sample.gyroX, sample.gyroY, rotationDegrees),
        gyroY = remapY(sample.gyroX, sample.gyroY, rotationDegrees),
        gyroZ = remapZ(sample.gyroZ),
        accelX = remapX(sample.accelX, sample.accelY, rotationDegrees),
        accelY = remapY(sample.accelX, sample.accelY, rotationDegrees),
        accelZ = remapZ(sample.accelZ),
    )

    private fun normalize(degrees: Int): Int {
        val wrapped = ((degrees % 360) + 360) % 360
        return when (wrapped) { 90, 180, 270 -> wrapped; else -> 0 }
    }
}
