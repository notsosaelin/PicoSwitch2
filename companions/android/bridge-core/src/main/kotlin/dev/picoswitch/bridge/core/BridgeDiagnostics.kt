package dev.picoswitch.bridge.core

/**
 * Where the bridge records what it observed.
 *
 * An interface rather than a concrete log because every platform already has a
 * place these belong (logcat, the Windows event log, journald, a file), and
 * because the shared layer must not depend on any of them. The application
 * supplies one implementation and gets bridge, transport and backend events in
 * the same stream as its own.
 *
 * Structured and throttled by the callers, never always-on: entries mark
 * transitions and edges, not every report.
 */
interface BridgeDiagnostics {
    fun event(area: String, event: String, detail: String = "")
    fun error(area: String, operation: String, error: Throwable)

    /** Discards everything; for hosts with no log and for tests. */
    object None : BridgeDiagnostics {
        override fun event(area: String, event: String, detail: String) = Unit
        override fun error(area: String, operation: String, error: Throwable) = Unit
    }
}

/** Human-readable renderings of the shared model, for diagnostics surfaces. */
object BridgeFormat {
    /** The normalized state, in bridge units, before any wire encoding. */
    fun describeNormalized(state: ControllerState): String = buildString {
        append("L(").append(state.leftX).append(',').append(state.leftY).append(") ")
        append("R(").append(state.rightX).append(',').append(state.rightY).append(") ")
        append("LT=").append(state.leftTrigger).append(" RT=").append(state.rightTrigger)
        append(" dpad=")
        append(if (state.dpadUp) "U" else "-")
        append(if (state.dpadRight) "R" else "-")
        append(if (state.dpadDown) "D" else "-")
        append(if (state.dpadLeft) "L" else "-")
        append(" buttons=")
        if (state.buttons.isEmpty()) append("none")
        else append(ControllerButton.entries.filter { it in state.buttons }.joinToString("+") { it.name })
    }

    fun hex(bytes: ByteArray): String = bytes.joinToString(" ") { "%02X".format(it) }

    fun describeMotion(motion: ControllerMotion): String =
        if (!motion.valid) "invalid"
        else "gyro(${motion.gyroX},${motion.gyroY},${motion.gyroZ}) " +
            "accel(${motion.accelX},${motion.accelY},${motion.accelZ}) t=${motion.timestampTicks}"
}
