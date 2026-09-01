namespace PicoSwitch.Bridge.Touch;

/// <summary>Which gesture the press in progress is a candidate for.</summary>
internal enum TouchDwell
{
    None,
    Engage,
    Release,
}

/// <summary>What a movement did to the hold, if anything.</summary>
internal enum TouchLatchMove
{
    None,

    /// <summary>The slide reached the commit distance and the control is now held.</summary>
    Committed,

    /// <summary>
    /// The same contact came back to where it started and took the hold away again.
    ///
    /// Distinct from a release: the finger is still down and the control is still
    /// physically pressed, so nothing about what the console sees changes here. Only
    /// the hold that would have OUTLIVED the finger is gone.
    /// </summary>
    Cancelled,
}

/// <summary>
/// One control's hold state: whether it is latched, and the timed and spatial
/// gestures that can change what it publishes.
///
/// Three facts, deliberately kept apart:
///
/// <code>
/// touchPressed    a contact owns the control right now   (the engine's ownership map)
/// latchedPressed  the control was toggled into a hold    (this)
/// effectivePressed = touchPressed || latchedPressed      (masked while retriggering)
/// </code>
///
/// ## The gesture vocabulary
///
/// <code>
/// UNLATCHED   tap                              ordinary press
///             tap, press, hold                 armed; STILL an ordinary press
///             tap, press, hold, slide away     latch
///             ... and slide back to the start  cancel; armed again
///
/// LATCHED     quick tap                        retrigger: release edge, then held again
///             press held 1x the base           unlatch
/// </code>
///
/// ## The slide is reversible until the finger lifts
///
/// Two distances rather than one, and that is the whole of the hysteresis:
///
/// <code>
///   0                    cancel                     commit
///   |----- cancelled -----|------ unchanged ---------|----- committed ----->
/// </code>
///
/// A single threshold would flap: a thumb resting exactly on it produces a stream of
/// lock/unlock transitions, each one a real change to what the console is told after
/// the finger lifts.
///
/// ## Why timing alone cannot create a hold
///
/// Two earlier gestures were tried and both collided with real play. A plain double
/// tap collides with mashing, because mashing IS a stream of double taps. A double
/// tap whose second press is merely HELD collides with the very ordinary "double
/// tap, then keep holding" that games ask for directly — and no dwell, however long,
/// separates those two, because they are the same input.
///
/// So the committing act is not a duration at all. After the dwell the gesture is
/// ARMED and nothing has changed. Only a deliberate SLIDE away from where that press
/// began commits the latch. Nothing a game asks a player to do involves pressing a
/// button and dragging off it, which is exactly why that motion is safe to spend on
/// this.
///
/// ## Nothing here delays input
///
/// The recognizer OBSERVES presses the engine has already applied, so it can never
/// delay, swallow or reorder ordinary input. A detector that waited to see what the
/// user meant would make every first tap late.
///
/// ## Deliberately not Boolean-shaped
///
/// Nothing here knows what the control DOES. The recognizer is about contacts,
/// distance and time only, so the analog-trigger hold — where the armed contact's
/// displacement selects a held trigger value rather than simply committing — reuses
/// this same gesture rather than growing a parallel one.
/// </summary>
internal sealed class TouchControlLatch
{
    /// <summary>No first tap is pending, so no gap can fall inside the double-tap window.</summary>
    private const long NoPendingTap = long.MaxValue;

    /// <summary>When the last QUALIFYING tap released; zero when no tap is pending.</summary>
    private long lastTapEndNanos;

    /// <summary>When the current press began; zero when nothing is pressed.</summary>
    private long pressStartNanos;

    /// <summary>Where it began, so displacement is measured from the contact's own origin.</summary>
    private float originX;

    private float originY;

    /// <summary>
    /// Whether the press in progress may become half of a double tap.
    ///
    /// Cleared by the press that became a candidate, so one gesture attempt consumes
    /// one sequence: a candidate the user abandoned does not leave a half-open
    /// recognizer for the next press to trip over.
    /// </summary>
    private bool tapSequenceOpen = true;

    /// <summary>
    /// The hold currently standing was committed by the contact still down.
    ///
    /// The gate on cancelling, and it is load-bearing rather than bookkeeping: a press
    /// on an ALREADY-held control begins inside the cancel radius by definition — the
    /// finger is on the control it is about to press — so without this, the smallest
    /// jitter would silently drop a hold the user made earlier and is now merely
    /// touching. Only a hold this gesture created may be undone by this gesture.
    /// </summary>
    private bool committedHere;

    /// <summary>The control is toggled into a persistent hold.</summary>
    public bool Latched { get; private set; }

    /// <summary>
    /// The engage gesture is armed: the dwell elapsed and a slide would commit it.
    ///
    /// Nothing about the published state changes here. This is the window in which the
    /// control is simultaneously an ordinary held button and one movement away from
    /// becoming a hold.
    /// </summary>
    public bool Armed { get; private set; }

    /// <summary>
    /// Absolute host time at which the press in progress completes its dwell, or zero
    /// when it is not a candidate. Same clock as <see cref="TouchContact.TimeNanos"/>.
    /// </summary>
    public long HoldDeadlineNanos { get; private set; }

    /// <summary>What reaching <see cref="HoldDeadlineNanos"/> would do.</summary>
    public TouchDwell Dwell { get; private set; } = TouchDwell.None;

    /// <summary>Absolute host time at which a retrigger pulse reasserts the hold; zero when idle.</summary>
    public long RetriggerDeadlineNanos { get; private set; }

    /// <summary>The hold is currently masked so a fresh press edge can be observed.</summary>
    public bool Retriggering => RetriggerDeadlineNanos != 0L;

    /// <summary>
    /// A press began. Returns true when it is a candidate for either gesture, which
    /// starts the corresponding dwell.
    ///
    /// Nothing toggles here, nothing is armed here and nothing is masked here. The
    /// press is an ordinary press until a gesture completes, which is what makes rapid
    /// tapping ordinary rapid tapping.
    /// </summary>
    public bool OnDown(long nowNanos, float x, float y, TouchLatchConfig config)
    {
        var hasClock = nowNanos > 0L;
        var gap = lastTapEndNanos > 0L ? nowNanos - lastTapEndNanos : NoPendingTap;
        pressStartNanos = nowNanos;
        originX = x;
        originY = y;
        lastTapEndNanos = 0L;
        Armed = false;
        committedHere = false;

        if (Latched)
        {
            // Already held, so there is nothing to build up to: one press is the whole
            // release gesture, and no slide is required. Its outcome — unlatch or
            // retrigger — is not decided until it ends.
            tapSequenceOpen = false;
            Dwell = hasClock ? TouchDwell.Release : TouchDwell.None;
            HoldDeadlineNanos = hasClock ? nowNanos + config.LatchReleaseThresholdNanos : 0L;
            return hasClock;
        }

        var qualifies = hasClock &&
            gap >= config.MinTapGapNanos &&
            gap <= config.DoubleTapWindowNanos;

        tapSequenceOpen = !qualifies;
        Dwell = qualifies ? TouchDwell.Engage : TouchDwell.None;
        HoldDeadlineNanos = qualifies ? nowNanos + config.LatchEngageThresholdNanos : 0L;
        return qualifies;
    }

    /// <summary>The dwell elapsed on an engage candidate: armed, but nothing is held yet.</summary>
    public void ArmEngage()
    {
        CancelDwell();
        Armed = true;
    }

    /// <summary>
    /// The armed candidate was spent on something else; no slide may commit it.
    ///
    /// The one caller is an analog trigger whose full-pull fallback won: the press
    /// waited out the selection window without moving, so it has already been answered
    /// as an ordinary held trigger. Leaving the arm standing would let a slide made
    /// AFTER that answer still lock a partial hold, which is a persistent state reached
    /// through a gesture the recognizer had already resolved as something else.
    /// </summary>
    public void AbandonArm()
    {
        CancelDwell();
        Armed = false;
    }

    /// <summary>The dwell elapsed on a release candidate.</summary>
    public void CompleteRelease()
    {
        CancelDwell();
        Latched = false;
    }

    /// <summary>
    /// The contact moved.
    ///
    /// Two different jobs either side of arming, both measured as displacement from
    /// the contact's own origin:
    ///
    /// - before arming, travelling past <paramref name="slop"/> abandons the
    ///   candidate, because a drag is not the hold the gesture asked for;
    /// - after arming, travelling past <paramref name="commitDistance"/> commits it.
    ///
    /// Direction is irrelevant and neither distance has anything to do with the
    /// control's bounds. A bounds test would make the gesture easy on a large button,
    /// hard on a small one, and unreliable anywhere near an edge.
    /// </summary>
    /// <param name="commitTravel">
    /// How much of the slide COUNTS toward committing, when that is not simply how far
    /// the contact moved. Null — every digital control — means straight-line
    /// displacement. An analog trigger supplies its projection onto its own inward
    /// axis instead: there the slide is not merely a confirmation, it is the act that
    /// CHOOSES the held level, so a sideways or outward slide must not be able to lock
    /// a trigger to a level nobody selected.
    /// </param>
    /// <param name="cancelDistance">
    /// Undoes a hold this same contact just made, and is always a plain RADIUS around
    /// the origin even for an analog trigger. "Back where I started" is a question
    /// about the finger's position, not about the trigger's axis.
    /// </param>
    public TouchLatchMove OnMove(
        float x,
        float y,
        float slop,
        float commitDistance,
        float cancelDistance,
        float? commitTravel = null)
    {
        var dx = x - originX;
        var dy = y - originY;
        var travelled = (dx * dx) + (dy * dy);

        if (Armed)
        {
            var reached = commitTravel is { } travel
                ? travel >= commitDistance
                : travelled >= commitDistance * commitDistance;
            if (!reached)
            {
                return TouchLatchMove.None;
            }

            Armed = false;
            Latched = true;
            committedHere = true;
            return TouchLatchMove.Committed;
        }

        if (committedHere && Latched)
        {
            if (travelled > cancelDistance * cancelDistance)
            {
                return TouchLatchMove.None;
            }

            Latched = false;
            committedHere = false;

            // Straight back to ARMED rather than to nothing, so the user who overshot
            // can simply slide out again. That is what makes the band above a
            // hysteresis rather than a one-way door.
            Armed = true;
            return TouchLatchMove.Cancelled;
        }

        if (HoldDeadlineNanos == 0L)
        {
            return TouchLatchMove.None;
        }

        if (travelled > slop * slop)
        {
            CancelDwell();
        }

        return TouchLatchMove.None;
    }

    /// <summary>
    /// A press ended. Returns true when it was a quick tap on a latched control, which
    /// is the caller's cue to start a retrigger pulse.
    ///
    /// Releasing while armed but before the slide is not a latch and never was: the
    /// press simply ends, exactly as an ordinary held button would.
    ///
    /// A cancelled contact is never a tap and never a retrigger: the platform took the
    /// gesture away, so the user did not release anything.
    /// </summary>
    public bool OnEnd(long nowNanos, bool cancelled, TouchLatchConfig config)
    {
        var retrigger = !cancelled && Dwell == TouchDwell.Release;
        CancelDwell();
        Armed = false;
        committedHere = false;

        var duration = nowNanos - pressStartNanos;
        var tapped = !cancelled && tapSequenceOpen && nowNanos > 0L && pressStartNanos > 0L &&
            duration >= 0 && duration <= config.MaxTapDurationNanos;

        lastTapEndNanos = tapped ? nowNanos : 0L;
        pressStartNanos = 0L;
        tapSequenceOpen = true;
        return retrigger;
    }

    public void CancelDwell()
    {
        HoldDeadlineNanos = 0L;
        Dwell = TouchDwell.None;
    }

    /// <summary>
    /// Start masking the hold so a fresh press edge exists. Returns false when a pulse
    /// is already in flight — a burst faster than the mask would otherwise read as one
    /// long release instead of repeated presses.
    /// </summary>
    public bool BeginRetrigger(long nowNanos, TouchLatchConfig config)
    {
        if (Retriggering || nowNanos <= 0L)
        {
            return false;
        }

        RetriggerDeadlineNanos = nowNanos + config.RetriggerReleaseNanos;
        return true;
    }

    /// <summary>The mask expired; the hold reasserts itself.</summary>
    public void EndRetrigger() => RetriggerDeadlineNanos = 0L;

    /// <summary>The earliest timed transition this control is waiting for, if any.</summary>
    public long? NextDeadlineNanos()
    {
        if (HoldDeadlineNanos == 0L)
        {
            return RetriggerDeadlineNanos == 0L ? null : RetriggerDeadlineNanos;
        }

        return RetriggerDeadlineNanos == 0L
            ? HoldDeadlineNanos
            : Math.Min(HoldDeadlineNanos, RetriggerDeadlineNanos);
    }
}

/// <summary>
/// A change to what the on-screen controller is holding by itself.
///
/// Emitted only on latch transitions — never per contact, never per frame, and NOT
/// for retrigger pulses, which are ordinary presses of an already-held control. This
/// is the one part of the touch path that can leave the console believing a button is
/// down with nothing on screen touching it, and the log that explains a stuck button
/// has to stay readable.
/// </summary>
public abstract record TouchLatchEvent
{
    private TouchLatchEvent()
    {
    }

    /// <summary>
    /// The user slid an armed contact far enough to commit the control into a hold.
    ///
    /// <paramref name="AnalogValue"/> is the level an analog trigger was locked at,
    /// and null for every digital control, where "held" has no level to state.
    /// Reported at COMMIT rather than at release, so the log records the moment the
    /// console started being told something no finger is doing.
    /// </summary>
    public sealed record Engaged(string ControlId, float? AnalogValue = null) : TouchLatchEvent;

    /// <summary>The user pressed and held the control out of its hold.</summary>
    public sealed record Released(string ControlId) : TouchLatchEvent;

    /// <summary>
    /// The user slid back to where the committing gesture began and took the hold off
    /// again, without ever lifting the finger.
    ///
    /// Kept apart from <see cref="Released"/> because the two answer different
    /// questions about a control that is no longer held: one is a deliberate
    /// press-and-hold some time later, the other is the same gesture being taken back.
    /// </summary>
    public sealed record Cancelled(string ControlId) : TouchLatchEvent;

    /// <summary>
    /// Every hold was dropped at a boundary.
    ///
    /// The reason is the same vocabulary every other global release uses, so "the
    /// session ended" and "the link dropped" stay distinguishable after the fact.
    /// </summary>
    public sealed record Cleared(IReadOnlySet<string> ControlIds, TouchReleaseReason Reason)
        : TouchLatchEvent;
}

/// <summary>
/// Host-side observer for latch transitions.
///
/// Deliberately shaped like <see cref="ITouchFeedbackBackend"/>: the portable engine
/// knows when something happened, and the host knows which of its own diagnostic
/// facilities should hear about it.
/// </summary>
public interface ITouchLatchObserver
{
    void OnLatchEvent(TouchLatchEvent value);

    public static ITouchLatchObserver None { get; } = new SilentObserver();

    private sealed class SilentObserver : ITouchLatchObserver
    {
        public void OnLatchEvent(TouchLatchEvent value)
        {
        }
    }
}
