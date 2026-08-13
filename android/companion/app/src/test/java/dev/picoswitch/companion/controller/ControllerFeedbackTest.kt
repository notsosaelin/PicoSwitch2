package dev.picoswitch.companion.controller

import org.junit.Assert.*
import org.junit.Test

/**
 * The adapter -> handheld direction: rumble, the console's player number, and
 * whether motion is actually being consumed. Decoding must be strict enough that
 * an unrelated report can never be applied as rumble, and tolerant enough to
 * survive the two framings Android stacks use for output reports.
 */
class ControllerFeedbackTest {
    private val outputId = ControllerReportEncoder.OUTPUT_REPORT_ID

    @Test fun `decodes rumble player led and motion flag`() {
        val decoded = ControllerFeedback.decode(
            byteArrayOf(200.toByte(), 100, 3, ControllerFeedback.FLAG_MOTION_WANTED.toByte()),
            outputId,
        )
        assertNotNull(decoded)
        assertEquals(200, decoded!!.rumbleLeft)
        assertEquals(100, decoded.rumbleRight)
        assertEquals(3, decoded.playerLed)
        assertTrue(decoded.motionWanted)
    }

    @Test fun `tolerates an embedded report id`() {
        // Some OEM stacks include the report ID in the delivered payload.
        val decoded = ControllerFeedback.decode(
            byteArrayOf(outputId.toByte(), 10, 20, 4, 0), outputId,
        )
        assertNotNull(decoded)
        assertEquals(10, decoded!!.rumbleLeft)
        assertEquals(20, decoded.rumbleRight)
        assertEquals(4, decoded.playerLed)
        assertFalse(decoded.motionWanted)
    }

    @Test fun `rejects a foreign report id`() {
        assertNull(ControllerFeedback.decode(byteArrayOf(1, 2, 3, 4), reportId = 7))
    }

    @Test fun `rejects null and short payloads instead of half applying them`() {
        assertNull(ControllerFeedback.decode(null, outputId))
        assertNull(ControllerFeedback.decode(byteArrayOf(1, 2, 3), outputId))
        assertNull(ControllerFeedback.decode(byteArrayOf(), outputId))
    }

    @Test fun `decodes without a report id when the stack does not supply one`() {
        val decoded = ControllerFeedback.decode(byteArrayOf(5, 6, 1, 1))
        assertNotNull(decoded)
        assertEquals(5, decoded!!.rumbleLeft)
        assertTrue(decoded.motionWanted)
    }

    @Test fun `single actuator is driven by the stronger motor`() {
        assertEquals(200, ControllerFeedback(rumbleLeft = 200, rumbleRight = 40).rumbleAmplitude)
        assertEquals(90, ControllerFeedback(rumbleLeft = 0, rumbleRight = 90).rumbleAmplitude)
        assertEquals(0, ControllerFeedback.None.rumbleAmplitude)
    }

    @Test fun `unset flag bits do not imply motion`() {
        val decoded = ControllerFeedback.decode(byteArrayOf(0, 0, 0, 0xFE.toByte()), outputId)
        assertNotNull(decoded)
        assertFalse(decoded!!.motionWanted)
    }
}

/**
 * Sensor frame -> screen frame.
 *
 * Android reports sensor axes in the device's NATURAL orientation. Without this
 * remap, a phone held sideways to play sends pitch as roll and aiming is rotated
 * 90 degrees — the single most likely "gyro is wrong" report from a user. The
 * transform matches the one Dycool's NS-PC-Control applies in its shipped
 * browser/mobile motion path.
 */
class MotionOrientationTest {
    @Test fun `natural orientation is identity`() {
        assertEquals(3, MotionOrientation.remapX(3, 5, 0))
        assertEquals(5, MotionOrientation.remapY(3, 5, 0))
        assertEquals(7, MotionOrientation.remapZ(7))
    }

    @Test fun `landscape rotations swap the horizontal axes with the right signs`() {
        // 90: [x, y] -> [-y, x]
        assertEquals(-5, MotionOrientation.remapX(3, 5, 90))
        assertEquals(3, MotionOrientation.remapY(3, 5, 90))
        // 270: [x, y] -> [y, -x]
        assertEquals(5, MotionOrientation.remapX(3, 5, 270))
        assertEquals(-3, MotionOrientation.remapY(3, 5, 270))
    }

    @Test fun `upside down inverts both horizontal axes`() {
        assertEquals(-3, MotionOrientation.remapX(3, 5, 180))
        assertEquals(-5, MotionOrientation.remapY(3, 5, 180))
    }

    @Test fun `screen normal is never touched by rotation`() {
        listOf(0, 90, 180, 270).forEach { assertEquals(9, MotionOrientation.remapZ(9)) }
    }

    @Test fun `four rotations compose back to identity`() {
        // Rotating a vector through all four screen orientations must be a pure
        // rotation group: applying 90 four times returns the original vector.
        var x = 11; var y = -4
        repeat(4) {
            val nx = MotionOrientation.remapX(x, y, 90)
            val ny = MotionOrientation.remapY(x, y, 90)
            x = nx; y = ny
        }
        assertEquals(11, x)
        assertEquals(-4, y)
    }

    @Test fun `unexpected rotation values fall back to identity instead of scrambling axes`() {
        // A malformed or unusual value must not silently permute axes.
        listOf(45, -1, 359, 1000).forEach { angle ->
            assertEquals(3, MotionOrientation.remapX(3, 5, angle))
            assertEquals(5, MotionOrientation.remapY(3, 5, angle))
        }
    }

    @Test fun `negative rotation values normalize to their positive equivalents`() {
        assertEquals(MotionOrientation.remapX(3, 5, 270), MotionOrientation.remapX(3, 5, -90))
        assertEquals(MotionOrientation.remapY(3, 5, 270), MotionOrientation.remapY(3, 5, -90))
    }

    @Test fun `extreme values do not overflow into the wrong sign`() {
        // Int32 headroom: the wire values are clamped to int16 before this point,
        // so negation can never wrap.
        assertEquals(-32767, MotionOrientation.remapX(0, 32767, 90))
        assertEquals(32768, MotionOrientation.remapX(0, -32768, 90))
    }
}

/**
 * Android SI sensor units -> the adapter's wire counts. These constants must
 * match tools/fixtures/android_controller_hid.h exactly, because the firmware
 * feeds them straight into the hardware-validated DualSense motion translation.
 */
class MotionScaleTest {
    @Test fun `one g of acceleration is 8192 counts`() {
        assertEquals(8192, MotionScale.accelCounts(MotionScale.GRAVITY_MS2.toFloat()))
        assertEquals(-8192, MotionScale.accelCounts(-MotionScale.GRAVITY_MS2.toFloat()))
        assertEquals(0, MotionScale.accelCounts(0f))
    }

    @Test fun `one degree per second is 16 point 384 counts`() {
        val oneDegreeInRadians = (1.0 / MotionScale.DEGREES_PER_RADIAN).toFloat()
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
}
