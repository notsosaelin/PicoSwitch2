package dev.picoswitch.companion.ui.touch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchGamepadNavigationTest {
    @Test
    fun `back opens a closed menu without exiting`() {
        val result = resolveTouchGamepadMenuEvent(
            menuOpen = false,
            event = TouchGamepadMenuEvent.Back,
        )

        assertTrue(result.menuOpen)
        assertFalse(result.exitTouchGamepad)
    }

    @Test
    fun `back closes an open menu without exiting`() {
        val result = resolveTouchGamepadMenuEvent(
            menuOpen = true,
            event = TouchGamepadMenuEvent.Back,
        )

        assertFalse(result.menuOpen)
        assertFalse(result.exitTouchGamepad)
    }

    @Test
    fun `only the explicit exit action requests mode exit`() {
        val result = resolveTouchGamepadMenuEvent(
            menuOpen = true,
            event = TouchGamepadMenuEvent.Exit,
        )

        assertEquals(TouchGamepadMenuResult(menuOpen = false, exitTouchGamepad = true), result)
    }
}
