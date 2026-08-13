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
