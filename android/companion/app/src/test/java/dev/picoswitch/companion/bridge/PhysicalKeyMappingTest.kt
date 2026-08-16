package dev.picoswitch.companion.bridge

import android.view.KeyEvent
import dev.picoswitch.bridge.core.ControllerButton
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Which physical key codes this project claims a meaning for.
 *
 * The rule these tests enforce: unknown or additional physical controller buttons
 * are preserved as candidates for future custom mapping rather than silently
 * assigned to unrelated controller actions.
 */
class PhysicalKeyMappingTest {

    /**
     * Regression, 2026-08-15. `BUTTON_C` and `BUTTON_Z` were routed to Capture,
     * which was arbitrary: they are extra physical buttons on some handhelds and
     * pads, not Capture keys. That surprised the user AND consumed two inputs the
     * eventual custom-mapping system should own.
     */
    @Test
    fun `BUTTON_C and BUTTON_Z are unmapped`() {
        assertNull(AndroidInputBackend.positionalButtonForKey(KeyEvent.KEYCODE_BUTTON_C))
        assertNull(AndroidInputBackend.positionalButtonForKey(KeyEvent.KEYCODE_BUTTON_Z))
    }

    /** They must not have been quietly repointed at C / GameChat either. */
    @Test
    fun `no physical key maps to Capture or C`() {
        // Sweep every gamepad key code Android defines, plus the extras.
        for (code in KeyEvent.KEYCODE_BUTTON_A..KeyEvent.KEYCODE_BUTTON_MODE) {
            val mapped = AndroidInputBackend.positionalButtonForKey(code)
            assertEquals(
                "key code $code must not map to Capture",
                false, mapped == ControllerButton.Capture,
            )
            assertEquals(
                "key code $code must not map to C / GameChat",
                false, mapped == ControllerButton.C,
            )
        }
    }

    /** The standard controls this project does have an intentional meaning for. */
    @Test
    fun `standard controls keep their mappings`() {
        val expected = mapOf(
            KeyEvent.KEYCODE_BUTTON_A to ControllerButton.A,
            KeyEvent.KEYCODE_BUTTON_B to ControllerButton.B,
            KeyEvent.KEYCODE_BUTTON_X to ControllerButton.X,
            KeyEvent.KEYCODE_BUTTON_Y to ControllerButton.Y,
            KeyEvent.KEYCODE_BUTTON_L1 to ControllerButton.L1,
            KeyEvent.KEYCODE_BUTTON_R1 to ControllerButton.R1,
            KeyEvent.KEYCODE_BUTTON_L2 to ControllerButton.L2,
            KeyEvent.KEYCODE_BUTTON_R2 to ControllerButton.R2,
            KeyEvent.KEYCODE_BUTTON_SELECT to ControllerButton.Select,
            KeyEvent.KEYCODE_BUTTON_START to ControllerButton.Start,
            KeyEvent.KEYCODE_BUTTON_THUMBL to ControllerButton.LeftStick,
            KeyEvent.KEYCODE_BUTTON_THUMBR to ControllerButton.RightStick,
            KeyEvent.KEYCODE_BUTTON_MODE to ControllerButton.Home,
        )
        expected.forEach { (code, button) ->
            assertEquals(button, AndroidInputBackend.positionalButtonForKey(code))
        }
    }

    /**
     * Capture and C are reachable, just not from a physical key: both go through
     * the virtual-button path, which removing the key mappings must not disturb.
     */
    @Test
    fun `Capture and C remain reachable as virtual buttons`() {
        val backend = AndroidInputBackend()
        backend.setVirtualButton(ControllerButton.Capture, true)
        backend.setVirtualButton(ControllerButton.C, true)
        assertEquals(
            setOf(ControllerButton.Capture, ControllerButton.C),
            backend.state.value.buttons,
        )
    }
}
