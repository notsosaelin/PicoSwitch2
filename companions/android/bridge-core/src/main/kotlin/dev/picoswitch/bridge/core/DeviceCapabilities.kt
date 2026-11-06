package dev.picoswitch.bridge.core

/**
 * What the host device can actually do, expressed without naming any platform.
 *
 * Deliberately small. Every field is read by something — the session decides
 * whether to run the time-driven motion loop from [gyroscope]/[accelerometer],
 * the output backend decides per-motor rumble from [rumbleMotors], and the UI
 * explains a missing capability rather than silently doing nothing. Capabilities
 * that no code consumes are not listed, however plausible they look: an unread
 * flag is a claim nobody checks.
 *
 * Two halves with different owners, merged by the session: the input backend
 * fills in what the SELECTED SOURCE has (buttons, sticks, triggers, D-pad), and
 * the session fills in what the HOST has (IMU, actuators, battery) from its own
 * backends. Neither can answer the other's half.
 *
 * There is deliberately no "reports display orientation" flag: whether the motion
 * frame correction actually ran is answered precisely by
 * `MotionDiagnostics.frameRotationMeasured`, and a second, coarser answer would
 * only be a place for the two to disagree.
 */
data class DeviceCapabilities(
    /** The source reports standard gamepad buttons. */
    val gamepadButtons: Boolean = false,
    /** Number of analog sticks the source reports (0, 1 or 2). */
    val analogSticks: Int = 0,
    /** Triggers report a continuous value rather than only a pressed bit. */
    val analogTriggers: Boolean = false,
    /** A hat or D-pad is present, from either axes or keys. */
    val dpad: Boolean = false,
    val gyroscope: Boolean = false,
    val accelerometer: Boolean = false,
    /**
     * Independently drivable rumble actuators. 0 = no output path at all,
     * 1 = amplitudes must be collapsed, 2+ = left/right are preserved.
     */
    val rumbleMotors: Int = 0,
    /** A battery level can be forwarded to the console. */
    val battery: Boolean = false,
) {
    /** Both IMU halves are required: a gyro with no accelerometer is not usable motion. */
    val motion: Boolean get() = gyroscope && accelerometer

    val rumble: Boolean get() = rumbleMotors > 0

    companion object { val None = DeviceCapabilities() }
}
