package dev.picoswitch.bridge.session

import dev.picoswitch.bridge.core.ControllerBattery
import dev.picoswitch.bridge.core.ControllerMotion
import dev.picoswitch.bridge.core.ControllerSourceIdentity
import dev.picoswitch.bridge.core.RumbleRequest

/**
 * Host motion, already converted into the bridge's canonical convention.
 *
 * See `dev.picoswitch.bridge.core.MotionConvention`: axes, signs, units, the
 * held-orientation frame and the timestamp base are all defined there, and it is
 * this backend's entire job to satisfy them. Nothing downstream re-interprets
 * what it returns.
 *
 * [start]/[stop] exist because motion is gated on the console actually consuming
 * it. A backend must tolerate being started and stopped repeatedly.
 */
interface MotionBackend {
    /** False when the host has no usable IMU; the bridge then never sends motion. */
    val available: Boolean

    fun start()
    fun stop()

    /**
     * Newest sample, or [ControllerMotion.None] until every required sensor has
     * reported at least once. A half-populated first frame must never be
     * published as motion.
     */
    fun sample(): ControllerMotion

    /** Layered view for diagnostics; see [MotionDiagnostics]. */
    fun diagnostics(): MotionDiagnostics

    object None : MotionBackend {
        override val available = false
        override fun start() = Unit
        override fun stop() = Unit
        override fun sample() = ControllerMotion.None
        override fun diagnostics() = MotionDiagnostics()
    }
}

/**
 * What a motion backend saw, at each layer, so a "my aim is rotated" report is
 * diagnosable without a rebuild.
 *
 * The three layers are deliberately separate: an axis complaint is only
 * interpretable next to the frame correction that was actually applied, and a
 * correction that could not be *measured* is itself the defect rather than a
 * value worth trusting.
 */
data class MotionDiagnostics(
    /** The host's own reading, in the host's own units and frame. */
    val platformRaw: String = "unavailable",
    /** The same sample after the backend's conversion, in canonical counts. */
    val canonical: String = "unavailable",
    /** Display rotation the backend corrected for, in degrees. */
    val frameRotationDegrees: Int = 0,
    /** False when the rotation could not be read and 0 is a default, not a measurement. */
    val frameRotationMeasured: Boolean = false,
)

/** Host battery, forwarded so the console shows a real controller battery. */
interface BatteryBackend {
    /** False on a host with no battery at all (a desktop); the field is then never sent. */
    val available: Boolean

    fun read(): ControllerBattery

    object None : BatteryBackend {
        override val available = false
        override fun read() = ControllerBattery.Unknown
    }
}

/**
 * Normalized output requests -> whatever the host can actually drive.
 *
 * The bridge hands over a [RumbleRequest] and nothing else. Effect construction,
 * amplitude control, retrigger rate, watchdogs, usage classification and every
 * other host-API concern belong here, because they differ completely between an
 * Android vibrator, a Windows force-feedback device and a Linux evdev FF slot,
 * while the request does not.
 */
interface OutputBackend {
    /**
     * Bind to the actuators belonging to a specific input source.
     *
     * MUST be honoured on every source change: on at least Android the correct
     * actuator is a property of the selected device, not of the application, and
     * driving the system-wide one instead is silent, total rumble loss.
     */
    fun bindToSource(source: ControllerSourceIdentity?)

    fun apply(request: RumbleRequest)

    /**
     * Re-assert or expire the current effect. Called on a low-rate tick while the
     * link is live, so a backend whose effect repeats until cancelled cannot leave
     * an actuator running after the bridge goes quiet.
     */
    fun keepAlive()

    fun stop()

    /** Layered view for diagnostics; see [OutputDiagnostics]. */
    fun diagnostics(): OutputDiagnostics

    object None : OutputBackend {
        override fun bindToSource(source: ControllerSourceIdentity?) = Unit
        override fun apply(request: RumbleRequest) = Unit
        override fun keepAlive() = Unit
        override fun stop() = Unit
        override fun diagnostics() = OutputDiagnostics()
    }
}

/**
 * Why output is or is not reaching hardware.
 *
 * [route] is free text on purpose: the useful answer is platform-specific
 * ("which of the four candidate actuators did we bind, and what did the others
 * report"), and forcing it into a shared enum would either lose the information
 * or drag platform vocabulary into the shared model.
 *
 * [warning] is separate because it answers a different question — not "where did
 * we route it" but "is something outside this app going to discard it anyway".
 * That distinction was worth days of investigation once already.
 */
data class OutputDiagnostics(
    val route: String = "not bound",
    /**
     * Independently drivable actuators the bind resolved. `0` means nothing on
     * this host can be driven, which is also how the session learns that output
     * is unavailable — there is no separate `available` flag to disagree with it.
     */
    val motors: Int = 0,
    val warning: String? = null,
)
