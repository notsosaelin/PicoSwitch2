package dev.picoswitch.bridge.core

/**
 * A rumble request in bridge semantics: two motor amplitudes, `0..255`.
 *
 * This says WHAT is asked for, never how a host performs it. There is no
 * duration, no waveform, no effect handle and no platform amplitude constant,
 * because none of those are protocol: the console holds an amplitude until it
 * changes it, and reproducing that with whatever the host's API offers is the
 * output backend's problem.
 *
 * Left and right are kept separate all the way from the console. A host with one
 * actuator collapses them — see [strongest] — but that collapse belongs to the
 * backend that has one actuator, not to the model.
 */
data class RumbleRequest(val left: Int = 0, val right: Int = 0) {
    /** Convenience for single-actuator hosts. Drive it from the stronger motor. */
    val strongest: Int get() = maxOf(left, right)

    val silent: Boolean get() = left == 0 && right == 0

    companion object { val None = RumbleRequest() }
}

/**
 * Everything the adapter asks of the host, decoded from one output report.
 *
 * Three genuinely different requests share one report because the wire contract
 * packs them together, not because they are one concept:
 *
 * - [rumble] is an output the host should reproduce;
 * - [playerIndicator] is the console's player number (`0` = none assigned), for a
 *   host that can show it — a light bar, an LED, or just the UI;
 * - [motionRequested] is a resource gate, not an output: the adapter derives it
 *   from the console's real negotiated IMU state, and the host registers its
 *   sensors only while it is set. Streaming an IMU nothing reads is pure battery
 *   cost.
 */
data class BridgeOutput(
    val rumble: RumbleRequest = RumbleRequest.None,
    val playerIndicator: Int = 0,
    val motionRequested: Boolean = false,
) {
    companion object { val None = BridgeOutput() }
}

/**
 * Amplitude shaping for a single-actuator host, kept pure so it can be tested
 * without hardware.
 *
 * Shared rather than platform-specific because the physics is: the console sends
 * `0..255`, and no small actuator usefully reproduces all of it. Below its start
 * threshold an LRA (and an ERM below stiction) makes audible driver noise and no
 * perceptible movement — the "buzzes but does nothing" failure. Tiny changes are
 * also not worth an actuator restart on any platform whose API cannot alter an
 * effect's amplitude in flight.
 *
 * A backend whose API *can* ramp amplitude continuously should skip this rather
 * than quantize for no reason.
 */
object RumbleShaping {
    /** Below this the actuator is silenced entirely. */
    const val GATE_OFF = 8

    /** Rising edge, above GATE_OFF so a value parked on the boundary cannot chatter. */
    const val GATE_ON = 14

    /** Retrigger granularity; finer differences are imperceptible. */
    const val STEP = 16

    /**
     * @param raw newest console amplitude
     * @param previous the last value this function returned, for hysteresis
     */
    fun shape(raw: Int, previous: Int): Int {
        val clamped = raw.coerceIn(0, 255)
        val gated = when {
            clamped <= GATE_OFF -> 0
            clamped >= GATE_ON -> clamped
            // Between the thresholds, hold whatever we were already doing.
            else -> if (previous > 0) clamped else 0
        }
        if (gated == 0) return 0
        // Round to nearest rather than down, so quantisation does not
        // systematically under-drive, and clamp so full scale stays full scale --
        // flooring would cap the console's hardest rumble at 240/255.
        return (((gated + STEP / 2) / STEP) * STEP)
            .coerceAtLeast(GATE_ON)
            .coerceAtMost(255)
    }
}
