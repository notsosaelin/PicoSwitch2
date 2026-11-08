namespace PicoSwitch.Bridge.Core;

/// <summary>
/// Host motion, already converted into the bridge's canonical convention.
///
/// The C# counterpart of the Kotlin <c>MotionBackend</c>
/// (<c>dev.picoswitch.bridge.session.PlatformBackends</c>). Axes, signs, units
/// and the timestamp base are defined in <see cref="MotionConvention"/>, and it is
/// the implementation's entire job to satisfy them; nothing downstream
/// re-interprets what <see cref="Sample"/> returns.
///
/// <see cref="Start"/> and <see cref="Stop"/> exist because motion is gated on
/// the console actually consuming it (the adapter's motion-wanted feedback flag).
/// An implementation must tolerate being started and stopped repeatedly.
///
/// The Windows companion has no implementation: its gamepad API exposes no
/// controller IMU. The Linux companion reads SDL3 gamepad sensors.
/// </summary>
public interface IMotionBackend
{
    /// <summary>False when the host has no usable IMU; the bridge then never sends motion.</summary>
    bool Available { get; }

    void Start();

    void Stop();

    /// <summary>
    /// Newest sample, or <see cref="ControllerMotion.None"/> until every required
    /// sensor has reported at least once. A half-populated first frame must never
    /// be published as motion.
    /// </summary>
    ControllerMotion Sample();
}

/// <summary>A host with no motion. The bridge sends no motion block at all.</summary>
public sealed class NoMotionBackend : IMotionBackend
{
    public static readonly NoMotionBackend Instance = new();

    private NoMotionBackend()
    {
    }

    public bool Available => false;

    public void Start()
    {
    }

    public void Stop()
    {
    }

    public ControllerMotion Sample() => ControllerMotion.None;
}
