package dev.picoswitch.companion.controller

import org.junit.Assert.*
import org.junit.Test

class ControllerReportEncoderTest {
    @Test fun `neutral report matches Pico fixture payload`() {
        assertArrayEquals(byteArrayOf(128.toByte(),128.toByte(),128.toByte(),128.toByte(),0,0,0,0,8), ControllerReportEncoder.encode(ControllerState.Neutral))
    }

    @Test fun `all fourteen buttons occupy sequential bits`() {
        ControllerButton.entries.forEachIndexed { index, button ->
            val report = ControllerReportEncoder.encode(ControllerState(buttons = setOf(button)))
            val bits = (report[6].toInt() and 0xFF) or ((report[7].toInt() and 0xFF) shl 8)
            assertEquals(1 shl index, bits)
        }
    }

    @Test fun `hat covers directions diagonals neutral and opposing cancellation`() {
        assertEquals(0, ControllerReportEncoder.hat(ControllerState(dpadUp = true)))
        assertEquals(1, ControllerReportEncoder.hat(ControllerState(dpadUp = true, dpadRight = true)))
        assertEquals(4, ControllerReportEncoder.hat(ControllerState(dpadDown = true)))
        assertEquals(7, ControllerReportEncoder.hat(ControllerState(dpadUp = true, dpadLeft = true)))
        assertEquals(8, ControllerReportEncoder.hat(ControllerState(dpadUp = true, dpadDown = true)))
    }

    @Test fun `axis normalization uses actual range and a safe floor deadzone`() {
        val range = AxisRange(-32767f, 32767f, 15f)
        assertEquals(128, range.stick(0f))
        assertEquals(128, range.stick(500f))
        assertEquals(1, range.stick(-32767f))
        assertEquals(255, range.stick(32767f))
        assertEquals(255, range.stick(-32767f, invert = true))
    }

    @Test fun `Thor gas brake trigger range maps endpoints`() {
        val range = AxisRange(0f, 32767f)
        assertEquals(0, range.trigger(0f))
        assertEquals(255, range.trigger(32767f))
    }

    @Test fun `descriptor stays byte exact`() {
        assertEquals(81, AndroidControllerDescriptor.bytes.size)
        assertEquals(0x05, AndroidControllerDescriptor.bytes.first().toInt())
        assertEquals(0xC0, AndroidControllerDescriptor.bytes.last().toInt() and 0xFF)
    }
}
