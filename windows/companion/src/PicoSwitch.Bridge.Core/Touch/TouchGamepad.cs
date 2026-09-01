using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// The on-screen controller, wired to the shared state machine.
///
/// Three things that every touchscreen host client would otherwise repeat
/// identically, done once:
///
/// <code>
/// platform contacts -&gt; TouchContactTracker -&gt; TouchControlEngine
///                                                   |
///                                          TouchContribution
///                                                   v
///                                          ControllerInputState
/// </code>
///
/// plus taking and returning <see cref="InputAuthority"/>, in the order that cannot
/// leave a held control behind: RELEASE the engine first, THEN move authority. Doing
/// it the other way round drops the engine's contribution on the floor while the
/// engine still believes the control is down, so the next contact event would
/// republish it.
///
/// The session-level work — pushing a neutral report while the link is still up,
/// rebinding output actuators — belongs to the host client, because only it knows what
/// the host's actuators are.
/// </summary>
public sealed class TouchGamepad
{
    private readonly ControllerInputState input;

    public TouchGamepad(ControllerInputState input, TouchControlConfig? config = null)
    {
        this.input = input;
        Engine = new TouchControlEngine(input.ApplyTouch, config);
        Contacts = new TouchContactTracker(Engine);
    }

    public TouchControlEngine Engine { get; }

    public TouchContactTracker Contacts { get; }

    /// <summary>True while the on-screen controller is the authoritative gameplay input.</summary>
    public bool Active { get; private set; }

    /// <summary>
    /// What the engine is tuned to right now.
    ///
    /// Exposed so a caller can change ONE tunable without asserting the rest. Two
    /// different owners set config here — the settings screen owns the deadzone and the
    /// latch default, the platform adapter owns the gesture timings it reads from the
    /// toolkit — and rebuilding from the default would let whichever ran last silently
    /// discard the other's values.
    /// </summary>
    public TouchControlConfig Config => Engine.Config;

    public void SetFeedbackBackend(ITouchFeedbackBackend backend) =>
        Engine.SetFeedbackBackend(backend);

    /// <summary>Where double-tap-hold transitions are reported; the host picks its own log.</summary>
    public void SetLatchObserver(ITouchLatchObserver observer) => Engine.SetLatchObserver(observer);

    public void SetConfig(TouchControlConfig config) => Engine.SetConfig(config);

    /// <summary>Replace geometry without allowing already-held contacts to claim it.</summary>
    public void SetLayout(
        ResolvedTouchLayout layout,
        TouchReleaseReason reason = TouchReleaseReason.GeometryInvalidated)
    {
        Contacts.ReleaseAll(reason);
        Engine.InstallLayout(layout);
    }

    /// <summary>Take gameplay input. Idempotent.</summary>
    public void Activate()
    {
        if (Active)
        {
            return;
        }

        Contacts.ReleaseAll(TouchReleaseReason.AuthorityChanged);
        input.SetAuthority(InputAuthority.Touch);
        Active = true;
    }

    /// <summary>Give gameplay input back to the host's physical controls. Idempotent.</summary>
    public void Deactivate()
    {
        if (!Active)
        {
            return;
        }

        Contacts.ReleaseAll(TouchReleaseReason.ModeExit);
        input.SetAuthority(InputAuthority.Physical);
        Active = false;
    }

    /// <summary>Drop every held control without giving up authority.</summary>
    public void Release(TouchReleaseReason reason) => Contacts.ReleaseAll(reason);

    /// <summary>
    /// When the engine next has timed gesture work, in the host's contact clock.
    ///
    /// The host is the clock: it stamps contacts, so it is the only thing that can say
    /// what time it is in the same units. Consult this after every contact batch and
    /// after every <see cref="Tick"/>; null means purely event-driven.
    /// </summary>
    public long? NextDeadlineNanos() => Engine.NextDeadlineNanos();

    /// <summary>
    /// Advance timed gesture work. Safe to call late, twice, or after a teardown.
    /// </summary>
    public void Tick(long nowNanos) => Engine.OnTick(nowNanos);

    public TouchDiagnosticsSnapshot Diagnostics() => Engine.Diagnostics();
}
