namespace PicoSwitch.Bridge.Core;

/// <summary>
/// A rumble request in bridge semantics: two motor amplitudes, <c>0..255</c>.
///
/// This says WHAT is asked for, never how a host performs it. There is no
/// duration, no waveform, no effect handle and no platform amplitude constant,
/// because none of those are protocol: the console holds an amplitude until it
/// changes it, and reproducing that with whatever the host's API offers is the
/// output backend's problem.
///
/// Left and right are kept separate all the way from the console. A host with one
/// actuator collapses them — see <see cref="Strongest"/> — but that collapse
/// belongs to the backend that has one actuator, not to the model.
/// </summary>
public readonly record struct RumbleRequest(int Left = 0, int Right = 0)
{
    public static RumbleRequest None => default;

    /// <summary>Convenience for single-actuator hosts. Drive it from the stronger motor.</summary>
    public int Strongest => Math.Max(Left, Right);

    public bool Silent => Left == 0 && Right == 0;
}

/// <summary>
/// Everything the adapter asks of the host, decoded from one output report.
///
/// Three genuinely different requests share one report because the wire contract
/// packs them together, not because they are one concept:
///
/// - <c>Rumble</c> is an output the host should reproduce;
/// - <c>PlayerIndicator</c> is the console's player number (<c>0</c> = none
///   assigned), for a host that can show it — a light bar, an LED, or just the UI;
/// - <c>MotionRequested</c> is a resource gate, not an output: the adapter derives
///   it from the console's real negotiated IMU state, and the host registers its
///   sensors only while it is set. Streaming an IMU nothing reads is pure battery
///   cost.
/// </summary>
public readonly record struct BridgeOutput(
    RumbleRequest Rumble = default,
    int PlayerIndicator = 0,
    bool MotionRequested = false)
{
    public static BridgeOutput None => default;
}

/// <summary>
/// Amplitude shaping for a single-actuator host, kept pure so it can be tested
/// without hardware.
///
/// Shared rather than platform-specific because the physics is: the console sends
/// <c>0..255</c>, and no small actuator usefully reproduces all of it. Below its
/// start threshold an LRA (and an ERM below stiction) makes audible driver noise
/// and no perceptible movement — the "buzzes but does nothing" failure. Tiny
/// changes are also not worth an actuator restart on any platform whose API
/// cannot alter an effect's amplitude in flight.
///
/// A backend whose API *can* ramp amplitude continuously should skip this rather
/// than quantize for no reason.
/// </summary>
public static class RumbleShaping
{
    /// <summary>Below this the actuator is silenced entirely.</summary>
    public const int GateOff = 8;

    /// <summary>Rising edge, above <see cref="GateOff"/> so a value parked on the boundary cannot chatter.</summary>
    public const int GateOn = 14;

    /// <summary>Retrigger granularity; finer differences are imperceptible.</summary>
    public const int Step = 16;

    /// <param name="raw">Newest console amplitude.</param>
    /// <param name="previous">The last value this function returned, for hysteresis.</param>
    public static int Shape(int raw, int previous)
    {
        var clamped = Math.Clamp(raw, 0, 255);
        var gated =
            clamped <= GateOff ? 0
            : clamped >= GateOn ? clamped

            // Between the thresholds, hold whatever we were already doing.
            : previous > 0 ? clamped
            : 0;

        if (gated == 0)
        {
            return 0;
        }

        // Round to nearest rather than down, so quantisation does not
        // systematically under-drive, and clamp so full scale stays full scale --
        // flooring would cap the console's hardest rumble at 240/255.
        return Math.Clamp(((gated + (Step / 2)) / Step) * Step, GateOn, 255);
    }
}
