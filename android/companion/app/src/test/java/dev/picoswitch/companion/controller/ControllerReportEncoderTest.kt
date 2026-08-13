package dev.picoswitch.companion.controller

import org.junit.Assert.*
import org.junit.Test

class ControllerReportEncoderTest {
    private fun ByteArray.u(index: Int) = this[index].toInt() and 0xFF
    private fun ByteArray.le16(index: Int): Int {
        val raw = u(index) or (u(index + 1) shl 8)
        return if (raw >= 0x8000) raw - 0x10000 else raw
    }

    @Test fun `neutral report matches Pico fixture payload`() {
        val report = ControllerReportEncoder.encode(ControllerState.Neutral)
        assertEquals(ControllerReportEncoder.PAYLOAD_SIZE_V2, report.size)
        // The v1 prefix is the contract the adapter's validated path parses; it
        // must stay byte-identical now that the extension is appended.
        assertArrayEquals(
            byteArrayOf(128.toByte(), 128.toByte(), 128.toByte(), 128.toByte(), 0, 0, 0, 0, 8),
            report.copyOfRange(0, ControllerReportEncoder.PAYLOAD_SIZE),
        )
        // Nothing else asserted: no motion, no battery.
        assertEquals(0, report.u(22))
    }

    @Test fun `v1 encoder still produces the original nine byte report`() {
        val report = ControllerReportEncoder.encodeV1(ControllerState.Neutral)
        assertEquals(ControllerReportEncoder.PAYLOAD_SIZE, report.size)
        assertArrayEquals(
            byteArrayOf(128.toByte(), 128.toByte(), 128.toByte(), 128.toByte(), 0, 0, 0, 0, 8),
            report,
        )
    }

    @Test fun `all fourteen buttons occupy sequential bits`() {
        ControllerButton.entries.forEachIndexed { index, button ->
            val report = ControllerReportEncoder.encode(ControllerState(buttons = setOf(button)))
            val bits = report.u(6) or (report.u(7) shl 8)
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
        // Must stay byte-identical to ANDROID_CONTROLLER_V2_HID_DESCRIPTOR: the
        // firmware identifies this bridge by an exact descriptor match, so a
        // one-sided edit silently disables motion/battery/rumble/LED.
        // tools/check_android_descriptor_parity.py enforces the same across languages.
        val golden = """
            05 01 09 05 A1 01 85 01 09 30 09 31 09 32 09 35 09 33 09 34
            15 00 26 FF 00 75 08 95 06 81 02 05 09 19 01 29 0E 15 00 25
            01 75 01 95 0E 81 02 75 01 95 02 81 03 05 01 09 39 15 00 25
            07 35 00 46 3B 01 65 14 75 04 95 01 81 42 75 04 95 01 81 03
            06 00 FF 65 00 09 20 09 21 09 22 09 23 09 24 09 25 16 00 80
            26 FF 7F 75 10 95 06 81 02 09 30 09 31 15 00 26 FF 00 75 08
            95 02 81 02 09 32 15 00 27 FF FF 00 00 75 10 95 01 81 02 85
            02 09 40 09 41 09 42 09 43 15 00 26 FF 00 75 08 95 04 91 02
            C0
        """.trimIndent().split(Regex("\\s+")).map { it.toInt(16).toByte() }.toByteArray()
        assertArrayEquals(golden, AndroidControllerDescriptor.bytes)
        assertEquals(161, golden.size)
        // The first 81 bytes are the hardware-validated v1 descriptor verbatim.
        assertEquals(0x06.toByte(), golden[81])
    }

    @Test fun `extreme simultaneous report keeps every field bounded`() {
        val report = ControllerReportEncoder.encode(ControllerState(
            leftX = -100, leftY = 999, rightX = 0, rightY = 255,
            leftTrigger = -1, rightTrigger = 256,
            buttons = ControllerButton.entries.toSet(), dpadUp = true, dpadRight = true,
        ))
        assertEquals(ControllerReportEncoder.PAYLOAD_SIZE_V2, report.size)
        assertArrayEquals(
            byteArrayOf(0, 255.toByte(), 0, 255.toByte(), 0, 255.toByte(), 255.toByte(), 0x3F, 1),
            report.copyOfRange(0, ControllerReportEncoder.PAYLOAD_SIZE),
        )
    }

    @Test fun `motion is encoded little endian only when valid`() {
        val motion = ControllerMotion(
            gyroX = 1000, gyroY = -2000, gyroZ = 32767,
            accelX = -8192, accelY = 8192, accelZ = -32768,
            timestampMs = 0x1234, valid = true,
        )
        val report = ControllerReportEncoder.encode(ControllerState(motion = motion))
        assertEquals(1000, report.le16(9))
        assertEquals(-2000, report.le16(11))
        assertEquals(32767, report.le16(13))
        assertEquals(-8192, report.le16(15))
        assertEquals(8192, report.le16(17))
        assertEquals(-32768, report.le16(19))
        assertEquals(0x1234, report.u(23) or (report.u(24) shl 8))
        assertEquals(ControllerReportEncoder.FLAG_MOTION_VALID, report.u(22))

        // Invalid motion must not leak stale values or set the flag.
        val idle = ControllerReportEncoder.encode(
            ControllerState(motion = motion.copy(valid = false)))
        assertEquals(0, idle.le16(9))
        assertEquals(0, idle.u(22) and ControllerReportEncoder.FLAG_MOTION_VALID)
    }

    @Test fun `motion timestamp wraps into sixteen bits`() {
        val report = ControllerReportEncoder.encode(ControllerState(
            motion = ControllerMotion(timestampMs = 0x1_0001, valid = true)))
        assertEquals(1, report.u(23) or (report.u(24) shl 8))
    }

    @Test fun `battery level and charging are encoded and clamped`() {
        val report = ControllerReportEncoder.encode(ControllerState(
            battery = ControllerBattery(levelPercent = 77, charging = true, valid = true)))
        assertEquals(77, report.u(21))
        assertEquals(
            ControllerReportEncoder.FLAG_BATTERY_VALID or ControllerReportEncoder.FLAG_CHARGING,
            report.u(22),
        )

        val clamped = ControllerReportEncoder.encode(ControllerState(
            battery = ControllerBattery(levelPercent = 250, valid = true)))
        assertEquals(100, clamped.u(21))
        assertEquals(ControllerReportEncoder.FLAG_BATTERY_VALID, clamped.u(22))

        // An unknown battery must not claim validity.
        val unknown = ControllerReportEncoder.encode(ControllerState.Neutral)
        assertEquals(0, unknown.u(22) and ControllerReportEncoder.FLAG_BATTERY_VALID)
    }

    @Test fun `degenerate axis ranges stay neutral and bounded`() {
        val range = AxisRange(1f, 1f, 10f)
        assertEquals(128, range.stick(1f))
        assertEquals(0, range.trigger(1f))
    }
}
