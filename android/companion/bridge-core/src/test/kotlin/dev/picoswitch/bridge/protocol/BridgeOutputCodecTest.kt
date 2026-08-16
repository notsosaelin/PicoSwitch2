package dev.picoswitch.bridge.protocol

import dev.picoswitch.bridge.core.RumbleRequest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The adapter -> host direction: rumble, the console's player number, and whether
 * motion is actually being consumed.
 *
 * Decoding must be strict enough that an unrelated report can never be applied as
 * rumble, and tolerant enough to survive both framings a HID host stack may use
 * for an output report. Both properties are protocol, not platform, which is why
 * they are pinned here rather than in any backend.
 */
class BridgeOutputCodecTest {
    private val outputId = ControllerReportEncoder.OUTPUT_REPORT_ID

    @Test fun `decodes rumble player indicator and motion flag`() {
        val decoded = BridgeOutputCodec.decode(
            byteArrayOf(200.toByte(), 100, 3, BridgeOutputCodec.FLAG_MOTION_WANTED.toByte()),
            outputId,
        )
        assertNotNull(decoded)
        assertEquals(200, decoded!!.rumble.left)
        assertEquals(100, decoded.rumble.right)
        assertEquals(3, decoded.playerIndicator)
        assertTrue(decoded.motionRequested)
    }

    @Test fun `tolerates an embedded report id`() {
        // Some OEM stacks include the report ID in the delivered payload.
        val decoded = BridgeOutputCodec.decode(
            byteArrayOf(outputId.toByte(), 10, 20, 4, 0), outputId,
        )
        assertNotNull(decoded)
        assertEquals(10, decoded!!.rumble.left)
        assertEquals(20, decoded.rumble.right)
        assertEquals(4, decoded.playerIndicator)
        assertFalse(decoded.motionRequested)
    }

    @Test fun `rejects a foreign report id`() {
        assertNull(BridgeOutputCodec.decode(byteArrayOf(1, 2, 3, 4), reportId = 7))
    }

    @Test fun `rejects null and short payloads instead of half applying them`() {
        assertNull(BridgeOutputCodec.decode(null, outputId))
        assertNull(BridgeOutputCodec.decode(byteArrayOf(1, 2, 3), outputId))
        assertNull(BridgeOutputCodec.decode(byteArrayOf(), outputId))
    }

    @Test fun `decodes without a report id when the stack does not supply one`() {
        val decoded = BridgeOutputCodec.decode(byteArrayOf(5, 6, 1, 1))
        assertNotNull(decoded)
        assertEquals(5, decoded!!.rumble.left)
        assertTrue(decoded.motionRequested)
    }

    @Test fun `unset flag bits do not imply motion`() {
        val decoded = BridgeOutputCodec.decode(byteArrayOf(0, 0, 0, 0xFE.toByte()), outputId)
        assertNotNull(decoded)
        assertFalse(decoded!!.motionRequested)
    }

    /**
     * Per-motor separation survives decode. Collapsing to one value is a
     * single-actuator HOST's decision, so the model must still carry both.
     */
    @Test fun `both motor amplitudes are preserved and only collapsed on request`() {
        val decoded = BridgeOutputCodec.decode(byteArrayOf(200.toByte(), 40, 0, 0), outputId)
        assertEquals(RumbleRequest(200, 40), decoded!!.rumble)
        assertEquals(200, decoded.rumble.strongest)
        assertEquals(90, RumbleRequest(0, 90).strongest)
        assertEquals(0, RumbleRequest.None.strongest)
        assertTrue(RumbleRequest.None.silent)
        assertFalse(RumbleRequest(0, 1).silent)
    }
}
