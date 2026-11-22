package dev.picoswitch.bridge.protocol

import dev.picoswitch.bridge.core.ControllerState
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Path C framing, held to the same numbers as the firmware contract
 * (ns2_companion_link.h) and the Windows ControllerLinkDataPlaneTests, so the
 * three implementations cannot drift apart.
 */
class ControllerLinkDataPlaneTest {
    @Test fun `frame geometry matches the firmware contract`() {
        assertEquals(ControllerReportEncoder.PAYLOAD_SIZE_V2, ControllerLinkDataPlane.PAYLOAD_BYTES)
        assertEquals(26, ControllerLinkDataPlane.PAYLOAD_BYTES)
        assertEquals(4, ControllerLinkDataPlane.HEADER_BYTES)
        assertEquals(30, ControllerLinkDataPlane.FRAME_BYTES)
        // 30-byte value + 3 bytes of ATT Write Command overhead.
        assertEquals(33, ControllerLinkDataPlane.MINIMUM_ATT_MTU)
        assertFalse(ControllerLinkDataPlane.mtuSufficient(23))  // default ATT MTU
        assertFalse(ControllerLinkDataPlane.mtuSufficient(32))
        assertTrue(ControllerLinkDataPlane.mtuSufficient(33))
        assertTrue(ControllerLinkDataPlane.mtuSufficient(517))
        assertEquals(BridgeOutputCodec.BODY_SIZE, ControllerLinkDataPlane.OUTPUT_MAX_PAYLOAD)
        assertEquals(4, ControllerLinkDataPlane.OUTPUT_MAX_PAYLOAD)
    }

    @Test fun `encodes an encoder payload into a frame`() {
        val payload = ControllerReportEncoder.encode(ControllerState.Neutral)
        val frame = ControllerLinkDataPlane.encodeInput(payload, 0xBEEF)
        assertEquals(ControllerLinkDataPlane.FRAME_BYTES, frame.size)
        assertEquals(ControllerLinkDataPlane.VERSION, frame[0].toInt() and 0xFF)
        assertEquals(ControllerLinkDataPlane.OPCODE_STATE, frame[1].toInt() and 0xFF)
        assertEquals(0xEF, frame[2].toInt() and 0xFF)  // little-endian sequence
        assertEquals(0xBE, frame[3].toInt() and 0xFF)
        assertArrayEquals(payload, frame.copyOfRange(ControllerLinkDataPlane.HEADER_BYTES, frame.size))
    }

    @Test(expected = IllegalArgumentException::class)
    fun `refuses a payload of the wrong size`() {
        ControllerLinkDataPlane.encodeInput(ByteArray(ControllerReportEncoder.PAYLOAD_SIZE), 0)
    }

    @Test fun `sequence wraps within sixteen bits`() {
        val payload = ByteArray(ControllerLinkDataPlane.PAYLOAD_BYTES)
        val top = ControllerLinkDataPlane.encodeInput(payload, 0xFFFF)
        assertEquals(0xFF, top[2].toInt() and 0xFF)
        assertEquals(0xFF, top[3].toInt() and 0xFF)
        val wrapped = ControllerLinkDataPlane.encodeInput(payload, 0x10000)
        assertEquals(0x00, wrapped[2].toInt() and 0xFF)
        assertEquals(0x00, wrapped[3].toInt() and 0xFF)
    }

    @Test fun `decodes a feedback frame and feeds the shared codec`() {
        val frame = byteArrayOf(
            ControllerLinkDataPlane.VERSION.toByte(),
            ControllerLinkDataPlane.OPCODE_OUTPUT.toByte(),
            ControllerReportEncoder.OUTPUT_REPORT_ID.toByte(),
            0x40, 0x80.toByte(), 0x02, BridgeOutputCodec.FLAG_MOTION_WANTED.toByte(),
        )
        val decoded = assertNotNullAndGet(ControllerLinkDataPlane.decodeOutput(frame))
        assertEquals(ControllerReportEncoder.OUTPUT_REPORT_ID, decoded.reportId)
        val output = assertNotNullAndGet(BridgeOutputCodec.decode(decoded.payload, decoded.reportId))
        assertEquals(0x40, output.rumble.left)
        assertEquals(0x80, output.rumble.right)
        assertEquals(2, output.playerIndicator)
        assertTrue(output.motionRequested)
    }

    @Test fun `rejects feedback frames that cannot be ours`() {
        assertNull(ControllerLinkDataPlane.decodeOutput(null))
        assertNull(ControllerLinkDataPlane.decodeOutput(ByteArray(0)))
        assertNull(ControllerLinkDataPlane.decodeOutput(byteArrayOf(1, 2)))
        // Wrong version, wrong opcode (the input opcode echoed back), oversized body.
        assertNull(ControllerLinkDataPlane.decodeOutput(byteArrayOf(2, 0x81.toByte(), 2, 0, 0, 0, 0)))
        assertNull(ControllerLinkDataPlane.decodeOutput(byteArrayOf(1, 0x01, 2, 0, 0, 0, 0)))
        assertNull(ControllerLinkDataPlane.decodeOutput(byteArrayOf(1, 0x81.toByte(), 2, 0, 0, 0, 0, 0)))
    }

    private fun <T> assertNotNullAndGet(value: T?): T {
        assertNotNull(value)
        return value!!
    }
}
