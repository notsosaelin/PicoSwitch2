package dev.picoswitch.companion.ui

import android.view.KeyEvent

/**
 * Drives this app with a controller whenever the console does not own the pad
 * ([consoleOwnsPad]).
 *
 * - D-pad and left stick move focus. Android's own focus search handles both:
 *   unhandled stick motion is turned into D-pad steps by the view system.
 * - A activates the focused control (as D-pad centre).
 * - B goes back: an overlay, dialog or open pane first, then it shows or
 *   hides the navigation (never to Play).
 * - L1 / R1 step through the navigation's pages.
 *
 * Pure so the mapping is unit-tested; MainActivity performs the result.
 */
object GamepadNavigation {
    enum class Action { PassThrough, Confirm, Back, PreviousPage, NextPage, Consume }

    fun of(keyCode: Int, action: Int, repeatCount: Int): Action = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> Action.Confirm
        // On release, so the press that opened something cannot also close it.
        KeyEvent.KEYCODE_BUTTON_B ->
            if (action == KeyEvent.ACTION_UP) Action.Back else Action.Consume
        KeyEvent.KEYCODE_BUTTON_L1 ->
            if (action == KeyEvent.ACTION_DOWN && repeatCount == 0) Action.PreviousPage else Action.Consume
        KeyEvent.KEYCODE_BUTTON_R1 ->
            if (action == KeyEvent.ACTION_DOWN && repeatCount == 0) Action.NextPage else Action.Consume
        else -> Action.PassThrough
    }

    /**
     * Whether the handheld's controller drives the console (true) or this app.
     *
     * The console gets it only while Controller Link streams AND Play is in
     * front with the navigation and every overlay closed — the game screen.
     * Anything of the app's own on top (the navigation, another page, a dialog)
     * takes the pad, like Steam's overlay. Before 2026-11-08 streaming alone
     * decided, so the moment a game asked for an amiibo the library could not
     * be driven: every D-pad press went to the console. The Back key is outside
     * this rule (MainActivity reads it first), so the way back out always works.
     */
    fun consoleOwnsPad(
        streaming: Boolean,
        section: AppSection,
        overlay: AppOverlay,
        navigationShown: Boolean,
    ): Boolean = streaming && section == AppSection.Play && overlay == AppOverlay.None && !navigationShown

    /** The dock page [step] away from [current], wrapping; Settings (not in the dock) steps from Play. */
    fun stepPage(current: AppSection, step: Int): AppSection {
        val docked = AppSection.entries.filter { it.inDock }
        val index = docked.indexOf(current).let { if (it < 0) 0 else it }
        return docked[((index + step) % docked.size + docked.size) % docked.size]
    }
}
