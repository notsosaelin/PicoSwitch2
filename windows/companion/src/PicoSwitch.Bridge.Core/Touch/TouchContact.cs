namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// What one touch contact is doing right now.
///
/// <see cref="Cancel"/> is not a synonym for <see cref="Up"/>. A platform reports
/// it when the gesture was taken away — a system bar was swiped in, a dialog
/// appeared, the window lost the gesture — and the contact's last known position
/// is meaningless afterwards. Both end ownership, but only <see cref="Up"/> is a
/// deliberate release by the user.
/// </summary>
public enum TouchPhase
{
    Down,
    Move,
    Up,
    Cancel,
}

/// <summary>
/// One touch contact, in the portable vocabulary the control engine needs and
/// nothing more.
/// </summary>
/// <param name="Id">
/// MUST be stable for the whole lifetime of the contact and MUST NOT be a
/// position in whatever array the platform delivered. Every touch platform this
/// project is likely to target reports a stable identifier alongside an index
/// that reorders between events, and keying ownership on the index is the classic
/// bug that works perfectly with two fingers and breaks the moment a third
/// arrives or the first lifts.
/// </param>
/// <param name="X">
/// In the same coordinate space the layout was resolved into — see
/// <see cref="TouchLayoutRegion"/>. The platform adapter is the only thing that
/// knows about display density, window origins or rotation; by the time a contact
/// gets here it is just a point in the same plane as the control geometry.
/// </param>
/// <param name="TimeNanos">
/// A monotonic host stamp. Nothing in the CONTROL MATH reads it — a stick's value
/// is its position and nothing else — but the hold-to-latch recognizer does, so a
/// platform that passes zero gets diagnostics and no latch gesture rather than a
/// latch that fires on every second tap. That refusal is deliberate: a recognizer
/// running on a clock stuck at zero would toggle a persistent hold at random,
/// which is the worst failure this feature has.
/// </param>
/// <remarks>
/// Pressure, tool type, contact ellipse and historical samples are deliberately
/// absent: none of them change what a gamepad control does, and every one of them
/// would be a platform-shaped field in a portable model.
/// </remarks>
public readonly record struct TouchContact(
    long Id,
    TouchPhase Phase,
    float X,
    float Y,
    long TimeNanos = 0L);

/// <summary>
/// Why every touch contribution was dropped.
///
/// Recorded rather than merely logged: "the console kept walking" and "the app
/// cleared input but the link was already gone" are different defects with the
/// same symptom, and the reason of the last global release is what separates
/// them. Kept as project vocabulary so a platform adapter names a boundary rather
/// than inventing free text at each call site.
/// </summary>
public enum TouchReleaseReason
{
    /// <summary>The contact lifted or the platform cancelled it; ordinary per-contact end.</summary>
    ContactEnded,

    /// <summary>The user left the on-screen controller.</summary>
    ModeExit,

    /// <summary>The host client stopped being visible or focused.</summary>
    HostInactive,

    /// <summary>Control geometry changed, so every retained contact position is stale.</summary>
    GeometryInvalidated,

    /// <summary>The confirmed console-facing controller profile changed.</summary>
    PersonalityChanged,

    /// <summary>Gameplay routing stopped while the user edits a draft layout.</summary>
    EditorEntered,

    /// <summary>Gameplay input moved to another host control set.</summary>
    AuthorityChanged,

    /// <summary>The controller link dropped or was stopped.</summary>
    LinkEnded,

    /// <summary>The control surface was torn down.</summary>
    Disposed,

    /// <summary>A fault was caught at a boundary and the retained state is not trustworthy.</summary>
    Fault,

    /// <summary>
    /// Configuration changed under a hold that outlives contacts.
    ///
    /// Used only for double-tap latches: turning the setting off, or retiming the
    /// gesture, must not leave a control held under rules that no longer apply.
    /// </summary>
    SettingsChanged,
}

/// <summary>
/// What <see cref="TouchContactTracker"/> forwards to.
///
/// An interface rather than the concrete engine, which is the one deliberate
/// shape difference from the Kotlin original. The tracker's whole job is
/// reconciliation — forwarding in order, and cancelling contacts the platform
/// stopped mentioning — and that is worth testing without standing up the engine,
/// its config and a layout. The engine implements it.
/// </summary>
public interface ITouchContactSink
{
    void OnContact(TouchContact contact);

    void ReleaseAll(TouchReleaseReason reason);
}
