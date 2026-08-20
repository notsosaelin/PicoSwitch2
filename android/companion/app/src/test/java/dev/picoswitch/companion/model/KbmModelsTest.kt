package dev.picoswitch.companion.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Invariants of the Keyboard/Mouse client model.
 *
 * These are the pieces the UI cannot be trusted to get right by inspection: the
 * exact wire spelling of a source, the logarithmic sensitivity mapping, and the
 * fact that every destination the firmware names is representable.
 */
class KbmModelsTest {

    @Test
    fun `key source round-trips through its wire form`() {
        val source = KbmSource(KbmSourceKind.Key, 0x2C)
        assertEquals("key:2C", source.wire)
        assertEquals(source, KbmSource.parse(source.wire))
    }

    @Test
    fun `mouse source round-trips through its wire form`() {
        val source = KbmSource(KbmSourceKind.MouseButton, 3)
        assertEquals("mouse:3", source.wire)
        assertEquals(source, KbmSource.parse(source.wire))
    }

    @Test
    fun `single-digit key usages parse from either width`() {
        // The firmware emits two hex digits but accepts one, so a reply from a
        // future formatter that trims the leading zero must not be rejected.
        assertEquals(KbmSource(KbmSourceKind.Key, 0x04), KbmSource.parse("key:4"))
        assertEquals(KbmSource(KbmSourceKind.Key, 0x04), KbmSource.parse("key:04"))
    }

    @Test
    fun `sources the firmware refuses to bind are rejected here too`() {
        // 0..3 are "no event" and the rollover error codes; mouse buttons are
        // 1..5. Accepting them would let the app send a command that can only
        // come back as an error.
        assertNull(KbmSource.parse("key:00"))
        assertNull(KbmSource.parse("key:03"))
        assertNull(KbmSource.parse("mouse:0"))
        assertNull(KbmSource.parse("mouse:6"))
        assertNull(KbmSource.parse("wheel:1"))
    }

    @Test
    fun `every destination name the firmware emits is representable`() {
        // Verbatim from KBM_DESTINATION_NAMES in src/ns2_kbm.c. An unmapped
        // name would make the whole mapping page fail to parse.
        val firmwareNames = listOf(
            "none", "a", "b", "x", "y", "l", "r", "zl", "zr", "l3", "r3",
            "minus", "plus", "home", "capture", "dup", "ddown", "dleft", "dright",
            "gl", "gr", "c",
            "lstick_up", "lstick_down", "lstick_left", "lstick_right",
            "rstick_up", "rstick_down", "rstick_left", "rstick_right",
        )
        firmwareNames.forEach { name ->
            assertTrue("unmapped destination $name", KbmDestination.fromWire(name) != null)
        }
        assertEquals(firmwareNames.size, KbmDestination.entries.size)
    }

    @Test
    fun `every mode and profile the firmware names is representable`() {
        listOf("auto", "controller", "keyboard", "kbmouse").forEach {
            assertTrue("unmapped mode $it", KbmMode.fromWire(it) != null)
        }
        listOf("kb", "kbm").forEach {
            assertTrue("unmapped profile $it", KbmProfile.fromWire(it) != null)
        }
    }

    @Test
    fun `default keyboard bindings are all named keys`() {
        // The picker only offers named usages, so an unnamed one would be a
        // binding the user can see but never restore after changing it.
        listOf(0x1A, 0x16, 0x04, 0x07, 0x2C, 0x09, 0x08, 0xE1, 0x14, 0x15, 0x1E, 0x20,
            0xE0, 0xE2, 0x28, 0x2A, 0x29, 0x45, 0x4F, 0x50, 0x51, 0x52, 0x0C, 0x0E, 0x0D, 0x0F)
            .forEach { assertTrue("usage 0x%02X unnamed".format(it), KeyboardKeys.isNamed(it)) }
    }

    @Test
    fun `letters and digits get the labels a keyboard is printed with`() {
        assertEquals("A", KeyboardKeys.label(0x04))
        assertEquals("Z", KeyboardKeys.label(0x1D))
        assertEquals("1", KeyboardKeys.label(0x1E))
        assertEquals("0", KeyboardKeys.label(0x27))
        assertEquals("Space", KeyboardKeys.label(0x2C))
        assertEquals("F12", KeyboardKeys.label(0x45))
        assertEquals("Left Shift", KeyboardKeys.label(0xE1))
    }

    @Test
    fun `an unnamed usage falls back to a neutral label rather than a guess`() {
        assertEquals("Key 0x65", KeyboardKeys.label(0x65))
    }

    @Test
    fun `sensitivity position round-trips across the reported range`() {
        val min = 16
        val max = 8192
        listOf(16, 64, 256, 512, 2048, 8192).forEach { value ->
            val position = SensitivityScale.toPosition(value, min, max)
            assertEquals(
                "round trip failed for $value",
                value.toDouble(),
                SensitivityScale.fromPosition(position, min, max).toDouble(),
                1.0,
            )
        }
    }

    @Test
    fun `sensitivity slider spends half its travel below the default`() {
        // The point of the logarithmic mapping: on a linear slider the 512
        // default sits at 6% of travel, so every usable low setting is crammed
        // into the first fraction of an inch.
        val position = SensitivityScale.toPosition(512, 16, 8192)
        assertTrue("default sits at $position of travel", position in 0.45f..0.65f)
    }

    @Test
    fun `sensitivity mapping clamps to the adapter-reported bounds`() {
        assertEquals(16, SensitivityScale.fromPosition(-1f, 16, 8192))
        assertEquals(8192, SensitivityScale.fromPosition(2f, 16, 8192))
        assertEquals(0f, SensitivityScale.toPosition(1, 16, 8192))
        assertEquals(1f, SensitivityScale.toPosition(99999, 16, 8192))
    }

    @Test
    fun `a degenerate range cannot divide by zero`() {
        // A firmware that reported min == max would otherwise take log(1)/0.
        assertEquals(0f, SensitivityScale.toPosition(100, 100, 100))
        assertEquals(100, SensitivityScale.fromPosition(0.5f, 100, 100))
        assertEquals(0, SensitivityScale.steps(100, 100))
    }

    @Test
    fun `linked axes are reported only when the two values agree`() {
        assertTrue(KbmMouseConfig(sensitivityX = 512, sensitivityY = 512).axesLinked)
        assertFalse(KbmMouseConfig(sensitivityX = 512, sensitivityY = 640).axesLinked)
    }

    @Test
    fun `mouse tuning is reported as out of effect on a native pointer path`() {
        // Mouse settings only drive the translated-stick path. Claiming they
        // apply while the adapter emits real pointer movement would send the
        // user tuning a control that does nothing.
        val translated = KbmState(status = KbmStatus(nativeMouseOutput = false))
        val native = KbmState(status = KbmStatus(nativeMouseOutput = true))
        assertTrue(translated.mouseTuningInEffect)
        assertFalse(native.mouseTuningInEffect)
    }

    @Test
    fun `a fresh state is never claimed to have unsaved changes`() {
        // The protocol cannot say whether a runtime value matches flash, so a
        // new connection has to start clean rather than guessing.
        assertFalse(KbmState().dirty)
    }

    @Test
    fun `mapping separates keyboard and mouse sources`() {
        val mapping = KbmMapping(
            profile = KbmProfile.KeyboardMouse,
            bindings = listOf(
                KbmBinding(KbmSource(KbmSourceKind.Key, 0x1A), KbmDestination.LStickUp, false),
                KbmBinding(KbmSource(KbmSourceKind.MouseButton, 1), KbmDestination.Zr, true),
            ),
            loaded = true,
        )
        assertEquals(1, mapping.keyBindings.size)
        assertEquals(1, mapping.mouseBindings.size)
        assertEquals(1, mapping.customCount)
    }
}
