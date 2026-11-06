package dev.picoswitch.companion.bridge

import android.bluetooth.BluetoothProfile

/**
 * Which HID Device connection states actually END a connect attempt.
 *
 * Pure, and its own object, because the distinction is the whole content of a
 * real defect and deserves a test rather than a comment.
 *
 * ## The bug this exists to prevent (measured 2026-08-21)
 *
 * `AndroidHidTransport.onConnectionStateChanged` used to cancel the connect
 * watchdog unconditionally, at the top, BEFORE deciding whether the state was
 * one it handles. `STATE_DISCONNECTING` has no branch in that handler.
 *
 * On an Android 16 tablet the stack reported, in order:
 *
 * ```text
 * connecting                       -> phase Connecting, watchdog armed
 * (BR/EDR ACL comes up)
 * disconnecting                    -> watchdog CANCELLED, nothing handled it
 * (BR/EDR ACL goes down)
 * ...and never a final disconnected
 * ```
 *
 * so the phase stayed on "Connecting" forever with nothing left that could ever
 * resolve it. The interface lied indefinitely instead of failing in eight
 * seconds, and there was no way out of the screen except leaving it.
 *
 * A non-terminal state is not a promise that another callback is coming. Only a
 * state that genuinely concludes the attempt may disarm the watchdog.
 */
object HidConnectionState {

    /** True only for states that conclude a connect attempt. */
    fun isTerminal(state: Int): Boolean =
        state == BluetoothProfile.STATE_CONNECTED || state == BluetoothProfile.STATE_DISCONNECTED

    /**
     * The state in words.
     *
     * Named rather than logged as an integer because the point of the log line
     * is to make an unhandled state obvious, and "3" does not read as
     * "disconnecting" to someone scanning a bug report.
     */
    fun name(state: Int): String = when (state) {
        BluetoothProfile.STATE_DISCONNECTED -> "disconnected"
        BluetoothProfile.STATE_CONNECTING -> "connecting"
        BluetoothProfile.STATE_CONNECTED -> "connected"
        BluetoothProfile.STATE_DISCONNECTING -> "disconnecting"
        else -> "unknown($state)"
    }
}
