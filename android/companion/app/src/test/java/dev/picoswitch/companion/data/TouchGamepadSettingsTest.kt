package dev.picoswitch.companion.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Settings decoding, which is where a stored value that has drifted out of range
 * has to become a usable one.
 *
 * The values here come off disk, so they can be anything an older build, a hand
 * edit or a partially written file left behind. A controller with an 80 percent
 * deadzone or invisible controls is a broken controller, and the honest recovery
 * is a working default rather than a faithful reproduction of nonsense.
 */
class TouchGamepadSettingsTest {

    private fun decode(
        opacity: Float = TouchGamepadSettings.DEFAULT_OPACITY,
        dim: Float = TouchGamepadSettings.DEFAULT_DIM,
        haptics: Boolean = true,
        deadzone: Float = TouchGamepadSettings.DEFAULT_DEADZONE,
        background: String? = null,
    ) = TouchGamepadSettingsCodec.decode(opacity, dim, haptics, deadzone, background)

    @Test fun `the defaults are usable without visiting the settings`() {
        val settings = TouchGamepadSettings.Default
        assertEquals(decode(), settings)
        // Visible, unblocked, and with a gate small enough to keep full range.
        org.junit.Assert.assertTrue(settings.controlOpacity >= 0.6f)
        org.junit.Assert.assertTrue(settings.stickDeadzone <= 0.1f)
        org.junit.Assert.assertTrue(settings.hapticsEnabled)
        assertNull(settings.backgroundImage)
    }

    @Test fun `out of range values are clamped rather than trusted`() {
        assertEquals(TouchGamepadSettings.MIN_OPACITY, decode(opacity = -3f).controlOpacity, 1e-4f)
        assertEquals(TouchGamepadSettings.MAX_OPACITY, decode(opacity = 9f).controlOpacity, 1e-4f)
        assertEquals(TouchGamepadSettings.MAX_DIM, decode(dim = 4f).backgroundDim, 1e-4f)
        assertEquals(0f, decode(dim = -1f).backgroundDim, 1e-4f)
        assertEquals(TouchGamepadSettings.MAX_DEADZONE, decode(deadzone = 0.8f).stickDeadzone, 1e-4f)
        assertEquals(0f, decode(deadzone = -0.5f).stickDeadzone, 1e-4f)
    }

    @Test fun `non-finite values fall back to the default`() {
        assertEquals(
            TouchGamepadSettings.DEFAULT_OPACITY,
            decode(opacity = Float.NaN).controlOpacity, 1e-4f,
        )
        assertEquals(
            TouchGamepadSettings.DEFAULT_DEADZONE,
            decode(deadzone = Float.POSITIVE_INFINITY).stickDeadzone, 1e-4f,
        )
        assertEquals(
            TouchGamepadSettings.DEFAULT_DIM,
            decode(dim = Float.NEGATIVE_INFINITY).backgroundDim, 1e-4f,
        )
    }

    @Test fun `a blank background reference is no background`() {
        assertNull(decode(background = "").backgroundImage)
        assertNull(decode(background = "   ").backgroundImage)
        assertEquals("/data/x/touch-background.jpg", decode(background = "/data/x/touch-background.jpg").backgroundImage)
    }

    /**
     * Configuration only. Nothing that describes what is currently HELD may be
     * persisted: a process that came back from the dead and immediately told the
     * console a button was down would be the worst possible restoration.
     */
    @Test fun `the settings model carries no gameplay state`() {
        val fields = TouchGamepadSettings::class.java.declaredFields
            .map { it.name.lowercase() }
        listOf("pointer", "contact", "pressed", "button", "stickx", "sticky", "dpad", "trigger")
            .forEach { forbidden ->
                org.junit.Assert.assertFalse(
                    "settings must not persist gameplay state: $forbidden",
                    fields.any { it.contains(forbidden) },
                )
            }
    }
}
