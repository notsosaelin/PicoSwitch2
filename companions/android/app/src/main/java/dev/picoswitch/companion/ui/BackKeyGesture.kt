package dev.picoswitch.companion.ui

/**
 * The Back key as two gestures (owner, 2026-11-08):
 *
 * - **Tap**: ordinary Back. Whatever is open closes first (a dialog, a pane,
 *   an overlay); otherwise it shows or hides the navigation.
 * - **Hold** ([HOLD_MS]) while Controller Link streams: release Controller
 *   Link. It fires while the key is still down, so the player feels it happen
 *   without guessing how long to hold, and the release that follows does
 *   nothing.
 *
 * Back is the release key because it is the one control Controller Link never
 * sends to the console (it is not a Switch button), so holding it cannot also
 * press something in the game. A hold while not streaming is just a tap.
 *
 * Pure so the timing is unit-tested; MainActivity owns the timer and performs
 * the result.
 */
class BackKeyGesture(private val holdMs: Long = HOLD_MS) {
    enum class Result { None, Tap, Hold }

    private var downAt: Long? = null
    private var held = false

    val pressed: Boolean get() = downAt != null

    /** Key down. Repeats of a key already down change nothing. */
    fun down(nowMs: Long, repeatCount: Int) {
        if (repeatCount > 0 && downAt != null) return
        downAt = nowMs
        held = false
    }

    /**
     * The hold timer (or a key repeat) fired. [holdAllowed]: Controller Link is
     * streaming right now, checked at the moment it fires rather than at key down.
     */
    fun tick(nowMs: Long, holdAllowed: Boolean): Result {
        val start = downAt ?: return Result.None
        if (held || !holdAllowed || nowMs - start < holdMs) return Result.None
        held = true
        return Result.Hold
    }

    /** Key up: a tap unless the hold already fired. */
    fun up(): Result {
        val wasDown = downAt != null
        val wasHeld = held
        downAt = null
        held = false
        return if (wasDown && !wasHeld) Result.Tap else Result.None
    }

    /** The key was cancelled (the window lost focus mid-press): nothing happens. */
    fun cancel() {
        downAt = null
        held = false
    }

    companion object {
        /** Long enough that a tap never becomes a release; short enough to feel deliberate, not slow. */
        const val HOLD_MS = 800L
    }
}
