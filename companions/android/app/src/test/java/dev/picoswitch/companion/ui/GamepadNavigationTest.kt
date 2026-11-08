package dev.picoswitch.companion.ui

import android.view.KeyEvent
import dev.picoswitch.companion.ui.GamepadNavigation.Action
import org.junit.Assert.assertEquals
import org.junit.Test

class GamepadNavigationTest {
    @Test
    fun aConfirmsOnBothEdgesSoTheClickCompletes() {
        assertEquals(Action.Confirm, GamepadNavigation.of(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.ACTION_DOWN, 0))
        assertEquals(Action.Confirm, GamepadNavigation.of(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.ACTION_UP, 0))
    }

    @Test
    fun bGoesBackOnReleaseOnly() {
        assertEquals(Action.Consume, GamepadNavigation.of(KeyEvent.KEYCODE_BUTTON_B, KeyEvent.ACTION_DOWN, 0))
        assertEquals(Action.Back, GamepadNavigation.of(KeyEvent.KEYCODE_BUTTON_B, KeyEvent.ACTION_UP, 0))
    }

    @Test
    fun shouldersStepOncePerPressNotPerRepeat() {
        assertEquals(Action.NextPage, GamepadNavigation.of(KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.ACTION_DOWN, 0))
        assertEquals(Action.Consume, GamepadNavigation.of(KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.ACTION_DOWN, 1))
        assertEquals(Action.Consume, GamepadNavigation.of(KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.ACTION_UP, 0))
        assertEquals(Action.PreviousPage, GamepadNavigation.of(KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.ACTION_DOWN, 0))
    }

    @Test
    fun dpadIsLeftToFocusSearch() {
        for (key in listOf(KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT)) {
            assertEquals(Action.PassThrough, GamepadNavigation.of(key, KeyEvent.ACTION_DOWN, 0))
        }
    }

    @Test
    fun theConsoleOwnsThePadOnlyOnTheBareGameScreen() {
        val none = AppOverlay.None
        assertEquals(true, GamepadNavigation.consoleOwnsPad(true, AppSection.Play, none, navigationShown = false))
        // Not streaming: always the app.
        assertEquals(false, GamepadNavigation.consoleOwnsPad(false, AppSection.Play, none, navigationShown = false))
        // The regression: a game asks for an amiibo, the library opens while
        // streaming, and the D-pad must drive the library, not the console.
        assertEquals(false, GamepadNavigation.consoleOwnsPad(true, AppSection.Amiibo, none, navigationShown = false))
        // The navigation slid out over the game screen.
        assertEquals(false, GamepadNavigation.consoleOwnsPad(true, AppSection.Play, none, navigationShown = true))
        for (overlay in AppOverlay.entries.filter { it != none }) {
            assertEquals(false, GamepadNavigation.consoleOwnsPad(true, AppSection.Play, overlay, navigationShown = false))
        }
    }

    @Test
    fun pagesWrapThroughTheDockAndSettingsStepsFromPlay() {
        val docked = AppSection.entries.filter { it.inDock }
        assertEquals(docked[1], GamepadNavigation.stepPage(AppSection.Play, 1))
        assertEquals(docked.last(), GamepadNavigation.stepPage(AppSection.Play, -1))
        assertEquals(AppSection.Play, GamepadNavigation.stepPage(docked.last(), 1))
        assertEquals(docked[1], GamepadNavigation.stepPage(AppSection.Settings, 1))
    }
}
