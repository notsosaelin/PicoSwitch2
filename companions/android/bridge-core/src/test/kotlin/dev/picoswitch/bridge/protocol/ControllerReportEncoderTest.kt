package dev.picoswitch.bridge.protocol

import dev.picoswitch.bridge.core.AxisRange
import dev.picoswitch.bridge.core.ControllerBattery
import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerMotion
import dev.picoswitch.bridge.core.ControllerState
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
        // Ten bytes now, not nine: contract 4 gave the button field a third byte
        // for GL/GR, which moved the hat from 8 to 9 and the whole vendor
        // extension with it. Byte-for-byte against the C fixture's
        // ANDROID_CONTROLLER_V2_NEUTRAL_REPORT, minus its leading report ID.
        assertArrayEquals(
            byteArrayOf(128.toByte(), 128.toByte(), 128.toByte(), 128.toByte(), 0, 0, 0, 0, 0, 8),
            report.copyOfRange(0, 10),
        )
        // Nothing else asserted: no motion, no battery.
        assertEquals(0, report.u(23))
    }

    /**
     * The v1 report is no longer a prefix of the v2 one — through contract 3 it
     * was, and contract 4's third button byte ended that. A v1 peer reads the v1
     * descriptor, which still describes exactly these nine bytes, so this has to
     * keep producing them unchanged.
     */
    @Test fun `v1 encoder still produces the original nine byte report`() {
        val report = ControllerReportEncoder.encodeV1(ControllerState.Neutral)
        assertEquals(ControllerReportEncoder.PAYLOAD_SIZE, report.size)
        assertArrayEquals(
            byteArrayOf(128.toByte(), 128.toByte(), 128.toByte(), 128.toByte(), 0, 0, 0, 0, 8),
            report,
        )
        // The hat is still at 8 here, where the v2 report now has a button byte.
        assertEquals(8, report.u(8))
    }

    @Test fun `every button occupies its own sequential bit`() {
        assertEquals("contract 4 carries seventeen buttons", 17, ControllerButton.entries.size)
        ControllerButton.entries.forEachIndexed { index, button ->
            val report = ControllerReportEncoder.encode(ControllerState(buttons = setOf(button)))
            val bits = report.u(6) or (report.u(7) shl 8) or (report.u(8) shl 16)
            assertEquals("$button", 1 shl index, bits)
        }
    }

    /**
     * The grip buttons specifically, on the byte that only exists because of
     * them. GL is bit 15 -- the pad bit contract 3 had left -- and GR is bit 16,
     * the first bit of the new third byte.
     */
    @Test fun `GL and GR land in the new button byte`() {
        val gl = ControllerReportEncoder.encode(ControllerState(buttons = setOf(ControllerButton.GL)))
        assertEquals(0x80, gl.u(7))
        assertEquals(0, gl.u(8))

        val gr = ControllerReportEncoder.encode(ControllerState(buttons = setOf(ControllerButton.GR)))
        assertEquals(0, gr.u(7))
        assertEquals(0x01, gr.u(8))

        val both = ControllerReportEncoder.encode(
            ControllerState(buttons = setOf(ControllerButton.GL, ControllerButton.GR)),
        )
        assertEquals(0x80, both.u(7))
        assertEquals(0x01, both.u(8))
        assertEquals("and the hat still follows them", 8, both.u(9))
    }

    /** The seven pad bits of the third button byte must stay clear. */
    @Test fun `no button can set a padding bit`() {
        val all = ControllerReportEncoder.encode(
            ControllerState(buttons = ControllerButton.entries.toSet()),
        )
        assertEquals("only bit 16 of the third byte is a button", 0x01, all.u(8))
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
            15 00 26 FF 00 75 08 95 06 81 02 05 09 19 01 29 11 15 00 25
            01 75 01 95 11 81 02 75 01 95 07 81 03 05 01 09 39 15 00 25
            07 35 00 46 3B 01 65 14 75 04 95 01 81 42 75 04 95 01 81 03
            06 00 FF 65 00 09 20 09 21 09 22 09 23 09 24 09 25 16 00 80
            26 FF 7F 75 10 95 06 81 02 09 30 09 31 15 00 26 FF 00 75 08
            95 02 81 02 09 32 15 00 27 FF FF 00 00 75 10 95 01 81 02 85
            02 09 40 09 41 09 42 09 43 15 00 26 FF 00 75 08 95 04 91 02
            C0
        """.trimIndent().split(Regex("\\s+")).map { it.toInt(16).toByte() }.toByteArray()
        assertArrayEquals(golden, BridgeHidDescriptor.bytes)
        assertEquals(161, golden.size)
        // v2 contains the hardware-validated v1 descriptor verbatim except for its
        // trailing End Collection (0xC0), which is deferred until after the
        // extension -- so the vendor block starts at 80, not 81, and the whole
        // descriptor still closes with exactly one 0xC0.
        assertEquals(0x06.toByte(), golden[80])
        assertEquals(0xC0.toByte(), golden[golden.size - 1])
        assertEquals(1, golden.count { it == 0xC0.toByte() })
    }

    @Test fun `extreme simultaneous report keeps every field bounded`() {
        val report = ControllerReportEncoder.encode(ControllerState(
            leftX = -100, leftY = 999, rightX = 0, rightY = 255,
            leftTrigger = -1, rightTrigger = 256,
            buttons = ControllerButton.entries.toSet(), dpadUp = true, dpadRight = true,
        ))
        assertEquals(ControllerReportEncoder.PAYLOAD_SIZE_V2, report.size)
        assertArrayEquals(
            // Seventeen buttons: both full bytes, then bit 16 (GR) alone in the
            // third, whose remaining seven bits are descriptor padding.
            byteArrayOf(
                0, 255.toByte(), 0, 255.toByte(), 0, 255.toByte(),
                255.toByte(), 255.toByte(), 0x01, 1,
            ),
            report.copyOfRange(0, 10),
        )
    }

    @Test fun `motion is encoded little endian only when valid`() {
        val motion = ControllerMotion(
            gyroX = 1000, gyroY = -2000, gyroZ = 32767,
            accelX = -8192, accelY = 8192, accelZ = -32768,
            timestampTicks = 0x1234, valid = true,
        )
        // Every offset here moved one later at contract 4; see the neutral test.
        val report = ControllerReportEncoder.encode(ControllerState(motion = motion))
        assertEquals(1000, report.le16(10))
        assertEquals(-2000, report.le16(12))
        assertEquals(32767, report.le16(14))
        assertEquals(-8192, report.le16(16))
        assertEquals(8192, report.le16(18))
        assertEquals(-32768, report.le16(20))
        assertEquals(0x1234, report.u(24) or (report.u(25) shl 8))
        assertEquals(ControllerReportEncoder.FLAG_MOTION_VALID, report.u(23))

        // Invalid motion must not leak stale values or set the flag.
        val idle = ControllerReportEncoder.encode(
            ControllerState(motion = motion.copy(valid = false)))
        assertEquals(0, idle.le16(10))
        assertEquals(0, idle.u(23) and ControllerReportEncoder.FLAG_MOTION_VALID)
    }

    @Test fun `motion timestamp wraps into sixteen bits`() {
        val report = ControllerReportEncoder.encode(ControllerState(
            motion = ControllerMotion(timestampTicks = 0x1_0001, valid = true)))
        assertEquals(1, report.u(24) or (report.u(25) shl 8))
    }

    @Test fun `battery level and charging are encoded and clamped`() {
        val report = ControllerReportEncoder.encode(ControllerState(
            battery = ControllerBattery(levelPercent = 77, charging = true, valid = true)))
        assertEquals(77, report.u(22))
        assertEquals(
            ControllerReportEncoder.FLAG_BATTERY_VALID or ControllerReportEncoder.FLAG_CHARGING,
            report.u(23),
        )

        val clamped = ControllerReportEncoder.encode(ControllerState(
            battery = ControllerBattery(levelPercent = 250, valid = true)))
        assertEquals(100, clamped.u(22))
        assertEquals(ControllerReportEncoder.FLAG_BATTERY_VALID, clamped.u(23))

        // An unknown battery must not claim validity.
        val unknown = ControllerReportEncoder.encode(ControllerState.Neutral)
        assertEquals(0, unknown.u(23) and ControllerReportEncoder.FLAG_BATTERY_VALID)
    }

    @Test fun `degenerate axis ranges stay neutral and bounded`() {
        val range = AxisRange(1f, 1f, 10f)
        assertEquals(128, range.stick(1f))
        assertEquals(0, range.trigger(1f))
    }
}
