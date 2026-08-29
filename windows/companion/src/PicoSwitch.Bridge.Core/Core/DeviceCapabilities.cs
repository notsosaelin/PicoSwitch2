namespace PicoSwitch.Bridge.Core;

/// <summary>
/// What the host device can actually do, expressed without naming any platform.
///
/// Deliberately small. Every field is read by something — the session decides
/// whether to run the time-driven motion loop from
/// <c>Gyroscope</c>/<c>Accelerometer</c>, the output backend decides per-motor
/// rumble from <c>RumbleMotors</c>, and the UI explains a missing capability
/// rather than silently doing nothing. Capabilities that no code consumes are not
/// listed, however plausible they look: an unread flag is a claim nobody checks.
///
/// Two halves with different owners, merged by the session: the input backend
/// fills in what the SELECTED SOURCE has (buttons, sticks, triggers, D-pad), and
/// the session fills in what the HOST has (IMU, actuators, battery) from its own
/// backends. Neither can answer the other's half.
/// </summary>
public readonly record struct DeviceCapabilities(
    // The source reports standard gamepad buttons.
    bool GamepadButtons = false,

    // Number of analog sticks the source reports (0, 1 or 2).
    int AnalogSticks = 0,

    // Triggers report a continuous value rather than only a pressed bit.
    bool AnalogTriggers = false,

    // A hat or D-pad is present, from either axes or keys.
    bool Dpad = false,
    bool Gyroscope = false,
    bool Accelerometer = false,

    // Independently drivable rumble actuators. 0 = no output path at all,
    // 1 = amplitudes must be collapsed, 2+ = left/right are preserved.
    int RumbleMotors = 0,

    // A battery level can be forwarded to the console.
    bool Battery = false)
{
    /// <summary>Both IMU halves are required: a gyro with no accelerometer is not usable motion.</summary>
    public bool Motion => Gyroscope && Accelerometer;

    public bool Rumble => RumbleMotors > 0;

    public static DeviceCapabilities None => default;
}
