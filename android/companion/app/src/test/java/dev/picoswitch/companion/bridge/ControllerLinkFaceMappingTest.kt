package dev.picoswitch.companion.bridge

import android.view.KeyEvent
import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerFaceLayout
import java.nio.file.Files
import java.nio.file.Path
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Controller Link: what a press on the host's BUILT-IN pad sends to the console.
 *
 * Regression, 2026-08-24. The Touch Gamepad face correction moved the firmware's
 * bridge face usages onto their logical A/B/X/Y destinations, which is right for
 * the on-screen pad — but the physical path still forwarded its key codes in the
 * source device's own dialect, so every Controller Link face press came out
 * inverted on console under both layouts. Nothing failed: the two origins shared
 * one mapper and only the on-screen half had a cross-layer golden.
 *
 * This is the head of the physical path. `tools/test_controller_link_face_goldens.c`
 * is the tail, and both read `tools/fixtures/controller_link_face_mapping.csv`
 * so the console-facing expectation exists in exactly one place.
 */
class ControllerLinkFaceMappingTest {

    @Test
    fun `every fixture row maps its platform key to the logical button it claims`() {
        val rows = fixtureRows()
        assertEquals("fixture row count", 8, rows.size)

        rows.forEach { row ->
            val backend = AndroidInputBackend()
            backend.setFaceLayout(row.layout)

            val reported = AndroidInputBackend.reportedButtonForKey(row.keyCode)
            assertEquals("${row.id} is a mapped platform key", true, reported != null)

            backend.controller.pressButton(reported!!, true)
            assertEquals(
                "${row.id} published button",
                setOf(row.logicalButton),
                backend.state.value.buttons,
            )
            assertEquals(
                "${row.id} Android HID usage",
                row.usage,
                row.logicalButton.ordinal + 1,
            )
        }
    }

    /**
     * The property behind the table: on both source families the button you press
     * reaches the console face button in the SAME PLACE. A Nintendo-labelled
     * handheld reports its printed legend and needs no correction; a positional
     * pad names its bottom button `A` while the console's bottom button is B.
     */
    @Test
    fun `both layouts preserve the physical position of the press`() {
        val byLayout = fixtureRows().groupBy { it.layout }
        val nintendo = byLayout.getValue(ControllerFaceLayout.Nintendo).associate { it.keyCode to it.logicalButton }
        val xbox = byLayout.getValue(ControllerFaceLayout.Xbox).associate { it.keyCode to it.logicalButton }

        assertEquals(ControllerButton.A, nintendo.getValue(KeyEvent.KEYCODE_BUTTON_A))
        assertEquals(ControllerButton.B, xbox.getValue(KeyEvent.KEYCODE_BUTTON_A))
        listOf(
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y,
        ).forEach { key ->
            assertTrue(
                "key $key must not resolve alike under both layouts",
                nintendo.getValue(key) != xbox.getValue(key),
            )
        }
    }

    /** Non-face controls mean the same thing on every source. */
    @Test
    fun `shoulders and menu keys are never face swapped`() {
        val unchanged = mapOf(
            KeyEvent.KEYCODE_BUTTON_L1 to ControllerButton.L1,
            KeyEvent.KEYCODE_BUTTON_R1 to ControllerButton.R1,
            KeyEvent.KEYCODE_BUTTON_SELECT to ControllerButton.Select,
            KeyEvent.KEYCODE_BUTTON_START to ControllerButton.Start,
            KeyEvent.KEYCODE_BUTTON_THUMBL to ControllerButton.LeftStick,
            KeyEvent.KEYCODE_BUTTON_MODE to ControllerButton.Home,
        )
        ControllerFaceLayout.entries.forEach { layout ->
            unchanged.forEach { (key, expected) ->
                val backend = AndroidInputBackend()
                backend.setFaceLayout(layout)
                backend.controller.pressButton(
                    AndroidInputBackend.reportedButtonForKey(key)!!, true,
                )
                assertEquals("$layout key $key", setOf(expected), backend.state.value.buttons)
            }
        }
    }

    private data class Row(
        val layout: ControllerFaceLayout,
        val platformKey: String,
        val keyCode: Int,
        val logicalButton: ControllerButton,
        val usage: Int,
    ) {
        val id = "${layout.key}/$platformKey"
    }

    private fun fixtureRows(): List<Row> =
        Files.readAllLines(fixturePath())
            .map(String::trim)
            .filter { it.isNotEmpty() && !it.startsWith("#") }
            .map { line ->
                val fields = line.split(",")
                require(fields.size == 6) { "Malformed fixture row: $line" }
                Row(
                    layout = ControllerFaceLayout.fromKey(fields[0]),
                    platformKey = fields[1],
                    keyCode = keyCodeFor(fields[1]),
                    logicalButton = ControllerButton.valueOf(fields[2]),
                    usage = fields[3].toInt(),
                )
            }

    private fun keyCodeFor(platformKey: String): Int = when (platformKey) {
        "BUTTON_A" -> KeyEvent.KEYCODE_BUTTON_A
        "BUTTON_B" -> KeyEvent.KEYCODE_BUTTON_B
        "BUTTON_X" -> KeyEvent.KEYCODE_BUTTON_X
        "BUTTON_Y" -> KeyEvent.KEYCODE_BUTTON_Y
        else -> error("Fixture names an unknown platform key: $platformKey")
    }

    private fun fixturePath(): Path {
        var cursor: Path? = Path.of("").toAbsolutePath()
        while (cursor != null) {
            val candidate = cursor.resolve("tools/fixtures/controller_link_face_mapping.csv")
            if (Files.isRegularFile(candidate)) return candidate
            cursor = cursor.parent
        }
        error("Cannot find tools/fixtures/controller_link_face_mapping.csv from ${Path.of("").toAbsolutePath()}")
    }
}
