package dev.picoswitch.bridge.core

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Device natural frame -> held (screen) frame.
 *
 * Platforms report sensor axes in the device's NATURAL orientation. Without this
 * remap, a phone held sideways to play sends pitch as roll and aiming is rotated
 * 90 degrees — the single most likely "gyro is wrong" report from a user. The
 * transform matches the one Dycool's NS-PC-Control applies in its shipped
 * browser/mobile motion path.
 */
class ScreenOrientationTest {
    @Test fun `natural orientation is identity`() {
        assertEquals(3, ScreenOrientation.remapX(3, 5, 0))
        assertEquals(5, ScreenOrientation.remapY(3, 5, 0))
        assertEquals(7, ScreenOrientation.remapZ(7))
    }

    @Test fun `landscape rotations swap the horizontal axes with the right signs`() {
        // 90: [x, y] -> [-y, x]
        assertEquals(-5, ScreenOrientation.remapX(3, 5, 90))
        assertEquals(3, ScreenOrientation.remapY(3, 5, 90))
        // 270: [x, y] -> [y, -x]
        assertEquals(5, ScreenOrientation.remapX(3, 5, 270))
        assertEquals(-3, ScreenOrientation.remapY(3, 5, 270))
    }

    @Test fun `upside down inverts both horizontal axes`() {
        assertEquals(-3, ScreenOrientation.remapX(3, 5, 180))
        assertEquals(-5, ScreenOrientation.remapY(3, 5, 180))
    }

    @Test fun `screen normal is never touched by rotation`() {
        listOf(0, 90, 180, 270).forEach { assertEquals(9, ScreenOrientation.remapZ(9)) }
    }

    @Test fun `four rotations compose back to identity`() {
        // Rotating a vector through all four screen orientations must be a pure
        // rotation group: applying 90 four times returns the original vector.
        var x = 11; var y = -4
        repeat(4) {
            val nx = ScreenOrientation.remapX(x, y, 90)
            val ny = ScreenOrientation.remapY(x, y, 90)
            x = nx; y = ny
        }
        assertEquals(11, x)
        assertEquals(-4, y)
    }

    @Test fun `unexpected rotation values fall back to identity instead of scrambling axes`() {
        // A malformed or unusual value must not silently permute axes.
        listOf(45, -1, 359, 1000).forEach { angle ->
            assertEquals(3, ScreenOrientation.remapX(3, 5, angle))
            assertEquals(5, ScreenOrientation.remapY(3, 5, angle))
        }
    }

    @Test fun `negative rotation values normalize to their positive equivalents`() {
        assertEquals(ScreenOrientation.remapX(3, 5, 270), ScreenOrientation.remapX(3, 5, -90))
        assertEquals(ScreenOrientation.remapY(3, 5, 270), ScreenOrientation.remapY(3, 5, -90))
    }

    @Test fun `extreme values do not overflow into the wrong sign`() {
        // Int32 headroom: the wire values are clamped to int16 before this point,
        // so negation can never wrap.
        assertEquals(-32767, ScreenOrientation.remapX(0, 32767, 90))
        assertEquals(32768, ScreenOrientation.remapX(0, -32768, 90))
    }

    /**
     * The whole-sample helper must agree with the per-axis functions, and must
     * read every source axis from the ORIGINAL sample. Computing X from the
     * original and Y from an already-rewritten X is the classic way to turn a
     * rotation into a shear, and it would be invisible at 0 degrees.
     */
    @Test fun `whole sample remap matches the per axis functions`() {
        val sample = ControllerMotion(
            gyroX = 100, gyroY = 200, gyroZ = 300,
            accelX = -400, accelY = 500, accelZ = -600,
            timestampTicks = 1234, valid = true,
        )
        listOf(0, 90, 180, 270).forEach { rotation ->
            val remapped = ScreenOrientation.apply(sample, rotation)
            assertEquals(ScreenOrientation.remapX(100, 200, rotation), remapped.gyroX)
            assertEquals(ScreenOrientation.remapY(100, 200, rotation), remapped.gyroY)
            assertEquals(300, remapped.gyroZ)
            assertEquals(ScreenOrientation.remapX(-400, 500, rotation), remapped.accelX)
            assertEquals(ScreenOrientation.remapY(-400, 500, rotation), remapped.accelY)
            assertEquals(-600, remapped.accelZ)
            // Everything that is not an axis survives untouched.
            assertEquals(1234, remapped.timestampTicks)
            assertEquals(true, remapped.valid)
        }
    }
}

/**
 * SI sensor units -> canonical bridge counts. These constants must match
 * tools/fixtures/android_controller_hid.h exactly, because the firmware feeds
 * them straight into the hardware-validated DualSense motion translation.
 */
class MotionScaleTest {
    @Test fun `one g of acceleration is 8192 counts`() {
        assertEquals(8192, MotionScale.accelCounts(MotionConvention.GRAVITY_MS2.toFloat()))
        assertEquals(-8192, MotionScale.accelCounts(-MotionConvention.GRAVITY_MS2.toFloat()))
        assertEquals(0, MotionScale.accelCounts(0f))
    }

    @Test fun `one degree per second is 16 point 384 counts`() {
        val oneDegreeInRadians = (1.0 / MotionConvention.DEGREES_PER_RADIAN).toFloat()
        assertEquals(16, MotionScale.gyroCounts(oneDegreeInRadians))
        // 100 dps should be ~1638 counts.
        assertEquals(1638, MotionScale.gyroCounts(oneDegreeInRadians * 100f))
    }

    @Test fun `conversions saturate instead of wrapping`() {
        // A hard shake or a spin far beyond the modelled range must clamp, never
        // wrap into the opposite direction.
        assertEquals(32767, MotionScale.accelCounts(1000f))
        assertEquals(-32768, MotionScale.accelCounts(-1000f))
        assertEquals(32767, MotionScale.gyroCounts(1000f))
        assertEquals(-32768, MotionScale.gyroCounts(-1000f))
    }

    /**
     * 100 us ticks, truncated to the 16-bit wire field. Wrap is expected: the
     * firmware takes the delta in the field's own modulus, so a wrapping stamp is
     * correct and a millisecond stamp would not be.
     */
    @Test fun `sensor nanoseconds become 100 microsecond ticks and wrap at 16 bits`() {
        assertEquals(0, MotionScale.timestampTicks(0L))
        assertEquals(1, MotionScale.timestampTicks(100_000L))
        assertEquals(10_000, MotionScale.timestampTicks(1_000_000_000L)) // one second
        // 0x10000 ticks is exactly one wrap of the wire field.
        assertEquals(0, MotionScale.timestampTicks(0x10000L * 100_000L))
        assertEquals(1, MotionScale.timestampTicks((0x10000L + 1) * 100_000L))
        assertEquals(
            MotionConvention.TIMESTAMP_TICKS_PER_SECOND,
            1_000_000_000L / MotionConvention.TIMESTAMP_NANOS_PER_TICK,
        )
    }
}
