package dev.picoswitch.bridge.protocol

import dev.picoswitch.bridge.core.ControllerBattery
import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerMotion
import dev.picoswitch.bridge.core.ControllerState
import java.nio.file.Files
import java.nio.file.Path
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Encoder goldens, as DATA shared with every other implementation.
 *
 * The descriptor is already guarded in three languages, and the C and Kotlin
 * sides each have their own encoder tests — but until now the ENCODER OUTPUT was
 * never cross-checked as data. Two implementations can agree on 161 descriptor
 * bytes and still disagree about which bit GR sets, where the hat byte moved to
 * in contract 4, or whether a battery level is clamped before or after the valid
 * flag is decided. None of that is visible until a console misbehaves.
 *
 * `tools/fixtures/bridge_report_goldens.csv` closes that gap. This test is the
 * Kotlin consumer; `BridgeReportGoldenTests` in
 * `windows/companion/tests/PicoSwitch.Bridge.Core.Tests` is the C# one. Both read
 * the same file, so an encoder change made in one language and not the other
 * fails in the other language's suite.
 *
 * The file was generated from THIS encoder (see [regenerate]) and is checked in.
 */
class BridgeReportGoldenTest {

    @Test
    fun `every golden vector encodes to exactly its recorded bytes`() {
        val rows = goldenRows()
        assertTrue("fixture must carry vectors", rows.size >= 30)

        rows.forEach { row ->
            val state = row.state()
            assertEquals("${row.name} v1", row.v1, hex(ControllerReportEncoder.encodeV1(state)))
            assertEquals("${row.name} v2", row.v2, hex(ControllerReportEncoder.encode(state)))
        }
    }

    @Test
    fun `the goldens pin the wire payload lengths of both report versions`() {
        val rows = goldenRows()
        rows.forEach { row ->
            assertEquals("${row.name} v1 length", ControllerReportEncoder.PAYLOAD_SIZE * 2, row.v1.length)
            assertEquals("${row.name} v2 length", ControllerReportEncoder.PAYLOAD_SIZE_V2 * 2, row.v2.length)
        }
    }

    @Test
    fun `the goldens cover every logical button, every hat direction and both flag halves`() {
        // A golden file that happens to omit a button is a golden file that cannot
        // catch that button moving.
        val rows = goldenRows()
        ControllerButton.entries.forEach { button ->
            assertTrue(
                "no golden presses $button alone",
                rows.any { it.buttons == setOf(button) },
            )
        }
        (0..8).forEach { hat ->
            assertTrue(
                "no golden produces hat code $hat",
                rows.any { ControllerReportEncoder.hat(it.state()) == hat },
            )
        }
        assertTrue(rows.any { it.motion?.valid == true })
        assertTrue(rows.any { it.battery?.valid == true && it.battery.charging })
    }

    /**
     * Rewrites the fixture from this encoder.
     *
     * Deliberately not a test. Regenerating is a protocol change, and a protocol
     * change is a deliberate act with a contract bump attached (see
     * [BridgeContract]); a test that silently rewrote the goldens would erase the
     * evidence of exactly the divergence the file exists to catch. Call it by hand
     * when the wire layout is intentionally revised.
     */
    @Suppress("unused")
    fun regenerate() {
        val lines = mutableListOf(
            "# PicoSwitch Bridge input-report goldens: normalized state -> wire bytes.",
            "#",
            "# Generated from dev.picoswitch.bridge.protocol.ControllerReportEncoder and read by",
            "# BOTH encoders (Kotlin BridgeReportGoldenTest, C# BridgeReportGoldenTests). The",
            "# descriptor parity guard proves the two ends describe the same report; this proves",
            "# they FILL it identically, which no descriptor comparison can show.",
            "#",
            "# buttons  '-' or '|'-separated ControllerButton names",
            "# dpad     '-' or any of U R D L",
            "# motion   '-' (no sample) or [!]gx:gy:gz:ax:ay:az:ticks   ('!' = valid flag clear)",
            "# battery  '-' (no reading) or [!]percent:charging          ('!' = valid flag clear)",
            "# v1/v2    uppercase hex of the 9- and 26-byte payloads (no report id)",
            "#",
            "# name,leftX,leftY,rightX,rightY,leftTrigger,rightTrigger,buttons,dpad,motion,battery,v1,v2",
        )
        goldenRows().forEach { row ->
            val state = row.state()
            lines += listOf(
                row.name,
                row.leftX, row.leftY, row.rightX, row.rightY, row.leftTrigger, row.rightTrigger,
                row.buttonsField, row.dpadField, row.motionField, row.batteryField,
                hex(ControllerReportEncoder.encodeV1(state)),
                hex(ControllerReportEncoder.encode(state)),
            ).joinToString(",")
        }
        Files.write(fixturePath(), lines)
    }

    private data class Row(
        val name: String,
        val leftX: Int,
        val leftY: Int,
        val rightX: Int,
        val rightY: Int,
        val leftTrigger: Int,
        val rightTrigger: Int,
        val buttonsField: String,
        val dpadField: String,
        val motionField: String,
        val batteryField: String,
        val v1: String,
        val v2: String,
    ) {
        val buttons: Set<ControllerButton> = if (buttonsField == "-") emptySet() else
            buttonsField.split('|').map(ControllerButton::valueOf).toSet()

        val motion: ControllerMotion? = parseMotion(motionField)
        val battery: ControllerBattery? = parseBattery(batteryField)

        fun state(): ControllerState = ControllerState(
            leftX = leftX, leftY = leftY, rightX = rightX, rightY = rightY,
            leftTrigger = leftTrigger, rightTrigger = rightTrigger,
            buttons = buttons,
            dpadUp = dpadField.contains('U'),
            dpadRight = dpadField.contains('R'),
            dpadDown = dpadField.contains('D'),
            dpadLeft = dpadField.contains('L'),
            motion = motion ?: ControllerMotion.None,
            battery = battery ?: ControllerBattery.Unknown,
        )

        private fun parseMotion(field: String): ControllerMotion? {
            if (field == "-") return null
            val valid = !field.startsWith('!')
            val parts = field.removePrefix("!").split(':').map(String::toInt)
            require(parts.size == 7) { "malformed motion field: $field" }
            return ControllerMotion(
                gyroX = parts[0], gyroY = parts[1], gyroZ = parts[2],
                accelX = parts[3], accelY = parts[4], accelZ = parts[5],
                timestampTicks = parts[6], valid = valid,
            )
        }

        private fun parseBattery(field: String): ControllerBattery? {
            if (field == "-") return null
            val valid = !field.startsWith('!')
            val parts = field.removePrefix("!").split(':').map(String::toInt)
            require(parts.size == 2) { "malformed battery field: $field" }
            return ControllerBattery(levelPercent = parts[0], charging = parts[1] != 0, valid = valid)
        }
    }

    private fun goldenRows(): List<Row> =
        Files.readAllLines(fixturePath())
            .map(String::trim)
            .filter { it.isNotEmpty() && !it.startsWith("#") }
            .map { line ->
                val f = line.split(",")
                require(f.size == 13) { "Malformed golden row: $line" }
                Row(
                    name = f[0],
                    leftX = f[1].toInt(), leftY = f[2].toInt(),
                    rightX = f[3].toInt(), rightY = f[4].toInt(),
                    leftTrigger = f[5].toInt(), rightTrigger = f[6].toInt(),
                    buttonsField = f[7], dpadField = f[8],
                    motionField = f[9], batteryField = f[10],
                    v1 = f[11], v2 = f[12],
                )
            }

    private fun hex(bytes: ByteArray) = bytes.joinToString("") { "%02X".format(it.toInt() and 0xFF) }

    private fun fixturePath(): Path {
        var cursor: Path? = Path.of("").toAbsolutePath()
        while (cursor != null) {
            val candidate = cursor.resolve("tools/fixtures/bridge_report_goldens.csv")
            if (Files.isRegularFile(candidate)) return candidate
            cursor = cursor.parent
        }
        error("Cannot find tools/fixtures/bridge_report_goldens.csv from ${Path.of("").toAbsolutePath()}")
    }
}
