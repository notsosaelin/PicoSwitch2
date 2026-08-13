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
        val golden = """
            05 01 09 05 A1 01 85 01 09 30 09 31 09 32 09 35 09 33 09 34
            15 00 26 FF 00 75 08 95 06 81 02 05 09 19 01 29 0E 15 00 25 01
            75 01 95 0E 81 02 75 01 95 02 81 03 05 01 09 39 15 00 25 07
            35 00 46 3B 01 65 14 75 04 95 01 81 42 75 04 95 01 81 03 C0
        """.trimIndent().split(Regex("\\s+")).map { it.toInt(16).toByte() }.toByteArray()
        assertArrayEquals(golden, AndroidControllerDescriptor.bytes)
        assertEquals(81, golden.size)
    }

    @Test fun `extreme simultaneous report is exactly nine bounded payload bytes`() {
        val report = ControllerReportEncoder.encode(ControllerState(
            leftX = -100, leftY = 999, rightX = 0, rightY = 255,
            leftTrigger = -1, rightTrigger = 256,
            buttons = ControllerButton.entries.toSet(), dpadUp = true, dpadRight = true,
        ))
        assertEquals(ControllerReportEncoder.PAYLOAD_SIZE, report.size)
        assertArrayEquals(byteArrayOf(0, 255.toByte(), 0, 255.toByte(), 0, 255.toByte(), 255.toByte(), 0x3F, 1), report)
    }

    @Test fun `degenerate axis ranges stay neutral and bounded`() {
        val range = AxisRange(1f, 1f, 10f)
        assertEquals(128, range.stick(1f))
        assertEquals(0, range.trigger(1f))
    }
}
