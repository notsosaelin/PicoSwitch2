using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// Live picture of what the on-screen controller is doing, for diagnostics.
///
/// Counters and last-known values only. Nothing here is written per contact move
/// beyond incrementing an int, because the failure this feature is most likely to
/// have is a stall, and a diagnostic that samples at the contact rate would be the
/// thing causing it.
/// </summary>
public sealed record TouchDiagnosticsSnapshot
{
    public int OwnedControls { get; init; }

    /// <summary>
    /// The controls a CONTACT is holding right now, by id.
    /// </summary>
    /// <remarks>
    /// The ids behind <see cref="OwnedControls"/>. A surface drawing presses needs
    /// to know which control a finger is on, and asking the engine control by
    /// control would sample a moving state several times per frame -- a press
    /// could appear and vanish inside one repaint. One snapshot is one instant.
    ///
    /// Distinct from <see cref="LatchedControls"/> on purpose: a latch holds a
    /// control with no contact on it, and the two must be drawn differently.
    /// </remarks>
    public IReadOnlySet<string> HeldControls { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    public int ActiveContacts { get; init; }

    public long ContactsClaimed { get; init; }

    public long ContactsUnclaimed { get; init; }

    public long ContactsContested { get; init; }

    public long ContactsCancelled { get; init; }

    public long ReleaseAllCount { get; init; }

    public TouchReleaseReason? LastReleaseReason { get; init; }

    /// <summary>Controls currently held by a latch rather than by a finger.</summary>
    public IReadOnlySet<string> LatchedControls { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    /// <summary>Controls whose engage gesture is armed and awaiting a slide.</summary>
    public IReadOnlySet<string> ArmedControls { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    public long LatchesArmed { get; init; }

    public long LatchesEngaged { get; init; }

    public long LatchesReleased { get; init; }

    public long LatchesCleared { get; init; }

    /// <summary>Taps on an already-latched control that produced a fresh press edge.</summary>
    public long RetriggerPulses { get; init; }

    /// <summary>Holds taken back off by sliding to where the committing gesture began.</summary>
    public long LatchesCancelled { get; init; }

    /// <summary>Analog triggers that reached the terminal click.</summary>
    public long TriggerDetents { get; init; }

    /// <summary>Taps on an analog trigger that published a brief full pull.</summary>
    public long TriggerPulses { get; init; }

    /// <summary>
    /// What each analog trigger is publishing, <c>0..1</c>, by control id.
    ///
    /// A value rather than a counter because it is the only part of this feature a
    /// user can see being wrong: a trigger held at some level with nothing touching it
    /// looks identical to one at rest in every other diagnostic.
    /// </summary>
    public IReadOnlyDictionary<string, float> AnalogTriggers { get; init; } =
        new Dictionary<string, float>(StringComparer.Ordinal);

    /// <summary>
    /// Which way each analog trigger's fill should grow, by control id.
    ///
    /// Reported by the engine rather than by the renderer for one reason: while a
    /// gesture is live this is the axis FROZEN at pointer-down, and only the engine
    /// has that. A surface deriving it from the layout every frame would agree with
    /// the engine right up until the moment the two could differ.
    /// </summary>
    public IReadOnlyDictionary<string, TouchFillDirection> AnalogTriggerFills { get; init; } =
        new Dictionary<string, TouchFillDirection>(StringComparer.Ordinal);

    /// <summary>
    /// Where each stick instance is pushed, <c>-1..1</c> in SCREEN axes, by control id.
    /// </summary>
    /// <remarks>
    /// Per control rather than per side, and separate from
    /// <see cref="LeftStick"/>/<see cref="RightStick"/>, because those report what
    /// the CONSOLE is being sent -- one vector per side, from whichever instance
    /// won ownership. A layout may carry several instances of the same side, and a
    /// surface drawing from the published side vector would move the wrong stick
    /// under the thumb.
    ///
    /// Screen axes (y grows downward), matching the deltas the engine resolved,
    /// so a surface can add this to a control centre without re-deriving a
    /// convention the two ends could disagree about.
    /// </remarks>
    public IReadOnlyDictionary<string, TouchVector> StickVectors { get; init; } =
        new Dictionary<string, TouchVector>(StringComparer.Ordinal);

    public long LastContactTimeNanos { get; init; }

    public TouchVector LeftStick { get; init; } = TouchVector.Zero;

    public TouchVector RightStick { get; init; } = TouchVector.Zero;

    public DpadState Dpad { get; init; } = DpadState.None;
}

/// <summary>
/// The on-screen controller itself: contact ownership, control state, and one
/// coherent contribution per event.
///
/// ## Ownership
///
/// <code>
/// contact id  -&gt;  control id      (one control per contact)
/// control id  -&gt;  contact id      (one contact per control)
/// </code>
///
/// Keyed by the contact's STABLE id, never by its position in whatever array the
/// platform delivered. That distinction is invisible with two fingers and is the
/// single most common way an on-screen controller breaks.
///
/// A claim is made once, on Down, against the resolved layout. After that the contact
/// belongs to that control until it ends. In particular a stick keeps its contact when
/// the thumb leaves the visual circle — the stick clamps to full deflection instead —
/// because the alternative is the thumb wandering into a face button mid-turn.
///
/// A second contact landing on an already-owned control is ignored rather than
/// stealing it: two contradictory positions for one stick have no correct answer.
///
/// ## Holding without a finger
///
/// <code>
/// effectivePressed = (touchPressed || latchedPressed) &amp;&amp; !retriggering
/// </code>
///
/// The dwell only ARMS the engage gesture; a deliberate slide commits it, because
/// timing alone cannot be told apart from the ordinary "double tap, then keep
/// holding" a game may ask for.
///
/// ## Time
///
/// Two parts are TIMED rather than event-driven: the deliberate dwell that completes
/// a latch gesture, and the brief mask that gives a retrigger its release edge. A
/// still finger produces no events, so the engine cannot discover either on its own.
///
/// It does not own a clock either. <see cref="NextDeadlineNanos"/> states when it next
/// has work, the host sleeps until then and calls <see cref="OnTick"/>. A pull model
/// rather than a scheduler on purpose: there is no queued closure that could carry
/// stale state across a teardown, so a tick that arrives after
/// <see cref="ReleaseAll"/> finds nothing to do.
///
/// ## Threading
///
/// Not thread-safe, by the same rule as the state machine it feeds: drive it from the
/// one thread the host delivers contacts on.
/// </summary>
public sealed class TouchControlEngine : ITouchContactSink
{
    private readonly Action<TouchContribution> onContribution;

    private readonly Dictionary<long, string> contactToControl = [];
    private readonly Dictionary<string, long> controlToContact = new(StringComparer.Ordinal);

    /// <summary>
    /// Per-control latch state and tap recognition, created on first tap.
    ///
    /// Bounded by the layout's control count and cleared at every boundary that clears
    /// input, so it cannot accumulate across sessions or personalities.
    /// </summary>
    private readonly Dictionary<string, TouchControlLatch> latches = new(StringComparer.Ordinal);

    /// <summary>
    /// Per-control travel state for the triggers that have any.
    ///
    /// Keyed by CONTROL, not by side, so two triggers pulled at once are two
    /// independent gestures with two frozen axes and two owning contacts.
    /// </summary>
    private readonly Dictionary<string, TouchAnalogTriggerState> analogTriggers =
        new(StringComparer.Ordinal);

    /// <summary>
    /// What each INSTANCE is contributing, never what each binding is doing.
    ///
    /// This is the whole of duplicate safety. Two A buttons are two keys here;
    /// releasing one removes one key and the other still maps to the same position, so
    /// the aggregate stays pressed. Keyed by binding, the second release would have
    /// taken the first one's press with it.
    /// </summary>
    private readonly Dictionary<string, FaceButtonPosition> facePresses =
        new(StringComparer.Ordinal);

    private readonly Dictionary<string, ControllerButton> logicalPresses =
        new(StringComparer.Ordinal);

    /// <summary>Per-instance trigger level, by side, aggregated with max at publish.</summary>
    private readonly Dictionary<string, float> leftTriggerLevels = new(StringComparer.Ordinal);

    private readonly Dictionary<string, float> rightTriggerLevels = new(StringComparer.Ordinal);

    /// <summary>
    /// Per-instance stick and direction values, plus which instance currently SPEAKS
    /// for each logical control.
    ///
    /// Sticks and the D-pad cannot be aggregated the way buttons can: two full
    /// deflections in different directions have no meaningful sum, and averaging them
    /// would invent a third direction the user never asked for. Ownership instead.
    /// </summary>
    private readonly Dictionary<string, TouchVector> stickVectors = new(StringComparer.Ordinal);

    private readonly Dictionary<ControlSide, string> stickOwners = [];

    private readonly Dictionary<string, DpadState> dpadStates = new(StringComparer.Ordinal);

    private string? dpadOwner;

    private ITouchFeedbackBackend feedback;
    private ITouchLatchObserver latchObserver;
    private ResolvedTouchLayout layout = ResolvedTouchLayout.Empty;
    private TouchContribution published = TouchContribution.Neutral;

    private long contactsClaimed;
    private long contactsUnclaimed;
    private long contactsContested;
    private long contactsCancelled;
    private long releaseAllCount;
    private TouchReleaseReason? lastReleaseReason;
    private long lastContactTimeNanos;
    private long latchesArmed;
    private long latchesEngaged;
    private long latchesReleased;
    private long latchesCleared;
    private long retriggerPulses;
    private long latchesCancelled;
    private long triggerDetents;
    private long triggerPulses;

    public TouchControlEngine(
        Action<TouchContribution> onContribution,
        TouchControlConfig? config = null,
        ITouchFeedbackBackend? feedback = null,
        ITouchLatchObserver? latchObserver = null)
    {
        this.onContribution = onContribution;
        Config = config ?? TouchControlConfig.Default;
        this.feedback = feedback ?? ITouchFeedbackBackend.None;
        this.latchObserver = latchObserver ?? ITouchLatchObserver.None;
    }

    public TouchControlConfig Config { get; private set; }

    /// <summary>What touch is holding right now, independent of whether it is authoritative.</summary>
    public TouchContribution Contribution => published;

    public ResolvedTouchLayout ResolvedLayout => layout;

    public void SetFeedbackBackend(ITouchFeedbackBackend backend) => feedback = backend;

    public void SetLatchObserver(ITouchLatchObserver observer) => latchObserver = observer;

    /// <summary>
    /// Retune the engine.
    ///
    /// Changing the LATCH configuration drops whatever is currently latched. A user who
    /// has just turned hold-to-latch off means the button that is stuck down, and a
    /// window where the setting says off while a control is still held would be the
    /// exact confusion the setting exists to end.
    /// </summary>
    public void SetConfig(TouchControlConfig next)
    {
        var latchChanged = !Config.Latch.Equals(next.Latch);
        Config = next;
        if (latchChanged)
        {
            ClearLatches(TouchReleaseReason.SettingsChanged);
        }
    }

    /// <summary>
    /// Point the engine at freshly resolved geometry.
    ///
    /// Always releases first. Every retained contact position was measured against the
    /// old rectangle, so after a rotation or a window resize the engine's idea of where
    /// a thumb is has no relationship to where the control now is.
    /// </summary>
    public void SetLayout(ResolvedTouchLayout resolved)
    {
        ReleaseAll(TouchReleaseReason.GeometryInvalidated);
        InstallLayout(resolved);
    }

    /// <summary>Install geometry after the tracker has released and quarantined contacts.</summary>
    internal void InstallLayout(ResolvedTouchLayout resolved) => layout = resolved;

    void ITouchContactSink.OnContact(TouchContact contact) => OnContact(contact);

    /// <summary>
    /// Handle one contact event.
    ///
    /// Returns true when the event was consumed by a control, so a platform adapter can
    /// decide whether to let it continue to whatever is behind.
    /// </summary>
    public bool OnContact(TouchContact contact)
    {
        lastContactTimeNanos = contact.TimeNanos;
        return contact.Phase switch
        {
            TouchPhase.Down => OnDown(contact),
            TouchPhase.Move => OnMove(contact),
            TouchPhase.Up => OnEnd(contact, cancelled: false),
            _ => OnEnd(contact, cancelled: true),
        };
    }

    private bool OnDown(TouchContact contact)
    {
        if (!layout.Fits)
        {
            contactsUnclaimed++;
            return false;
        }

        var target = HitTest(contact.X, contact.Y);
        if (target is null)
        {
            contactsUnclaimed++;
            return false;
        }

        if (controlToContact.ContainsKey(target.Id))
        {
            // Exclusivity: the control already has an owner. Not an error, and not a
            // steal — the second thumb simply does nothing here.
            contactsContested++;
            return false;
        }

        contactToControl[contact.Id] = target.Id;
        controlToContact[target.Id] = contact.Id;
        contactsClaimed++;
        Engage(target, contact.X, contact.Y);

        // AFTER the press has been applied, so a gesture can only ever change what
        // happens LATER, never what this press itself sends.
        NoteLatchDown(target, contact.TimeNanos, contact.X, contact.Y);

        // AFTER the latch, because whether the control is already held is what decides
        // whether a still press here means "pull it fully" or "let go of it".
        BeginAnalogTrigger(target, contact);
        Publish();
        return true;
    }

    private bool OnMove(TouchContact contact)
    {
        if (!contactToControl.TryGetValue(contact.Id, out var controlId))
        {
            return false;
        }

        var control = layout.Control(controlId);
        if (control is null)
        {
            return false;
        }

        // Only the vector controls track movement. A held button stays held for as long
        // as its contact lives, including outside its own bounds: lifting the thumb is
        // how a button is released, not sliding off it.
        if (control.Spec.Action is TouchControlAction.Stick or TouchControlAction.Directions)
        {
            Engage(control, contact.X, contact.Y);
            Publish();
            return true;
        }

        // An analog trigger tracks movement like a vector control AND recognizes the
        // hold gesture like a button, because for it they are the same motion: the
        // slide that locks the control is the slide that chooses what it locks at.
        if (IsAnalogTrigger(control))
        {
            MoveAnalogTrigger(control, contact);
            return true;
        }

        // A button's movement means one of exactly three things: a drag that abandons a
        // pending dwell, the slide that commits an armed one, or the slide back that
        // takes it off again. NONE of them changes what is published — the control is
        // already pressed by this very contact — so nothing is published here.
        if (latches.TryGetValue(controlId, out var latch))
        {
            switch (latch.OnMove(
                        contact.X, contact.Y, GestureSlop(),
                        LatchCommitDistance(), LatchCancelDistance()))
            {
                case TouchLatchMove.Committed:
                    CommitLatch(controlId);
                    break;
                case TouchLatchMove.Cancelled:
                    CancelLatch(controlId);
                    break;
            }
        }

        return true;
    }

    private bool OnEnd(TouchContact contact, bool cancelled)
    {
        if (cancelled)
        {
            contactsCancelled++;
        }

        if (!contactToControl.Remove(contact.Id, out var controlId))
        {
            return false;
        }

        controlToContact.Remove(controlId);
        var control = layout.Control(controlId);

        if (control is not null)
        {
            NoteLatchEnd(control, contact.TimeNanos, cancelled);
            EndAnalogTrigger(control, contact, cancelled);

            // A latched control keeps its value when the finger leaves; that is the
            // entire feature, so nothing is disengaged.
            if (!IsLatched(controlId))
            {
                Disengage(control);
            }

            if (!cancelled &&
                control.Spec.Kind is not (TouchControlKind.Stick or TouchControlKind.Dpad))
            {
                // Buttons only. A stick or D-pad release is the end of a continuous
                // gesture, and buzzing there turns ordinary play into a rattle.
                //
                // Fired whether or not anything was disengaged: lifting off a held
                // button is still a release the user performed, and a latched control
                // that felt dead to the touch would be the clearest possible way to
                // say "this control is broken now".
                feedback.Perform(TouchFeedbackEvent.Release);
            }
        }

        Publish();
        return true;
    }

    // -------------------------------------------------------------------- latch

    /// <summary>
    /// Whether this control may be locked into a hold at all.
    ///
    /// Two gates that answer different questions: the kind is structural — a stick has
    /// no state to hold — and the spec's own tri-state is the user's answer, falling
    /// back to the global setting when they have not given one.
    /// </summary>
    private bool LatchEnabled(ResolvedTouchControl control) =>
        control.Spec.Kind.SupportsLatch() &&
        (control.Spec.Latch ?? Config.Latch.EnabledByDefault);

    private bool IsLatched(string controlId) =>
        latches.TryGetValue(controlId, out var latch) && latch.Latched;

    /// <summary>
    /// How far a dwelling contact may drift, in this layout's real coordinates.
    ///
    /// Converted through the region's density scale and NOT through the layout scale:
    /// the tolerance is a distance on the glass, so it must not shrink because the
    /// controller was laid out smaller.
    /// </summary>
    private float GestureSlop() => Config.Latch.GestureSlopUnits * layout.Region.UnitScale;

    private float LatchCommitDistance() =>
        Config.Latch.LatchCommitDistanceUnits * layout.Region.UnitScale;

    private float LatchCancelDistance() =>
        Config.Latch.LatchCancelDistanceUnits * layout.Region.UnitScale;

    private float AnalogDragSlop() => Config.Trigger.DragSlopUnits * layout.Region.UnitScale;

    /// <summary>
    /// The slide crossed the commit distance.
    ///
    /// Nothing is published: the control is already pressed by the contact that
    /// performed the gesture, so the hold changes only what happens when that contact
    /// eventually lifts.
    /// </summary>
    private void CommitLatch(string controlId, float? analogValue = null)
    {
        latchesEngaged++;
        feedback.Perform(TouchFeedbackEvent.LatchEngaged);
        latchObserver.OnLatchEvent(new TouchLatchEvent.Engaged(controlId, analogValue));
    }

    /// <summary>
    /// The committing contact came back to where it started; the hold is off.
    ///
    /// Nothing is published, and that is the point: the finger is still down, so the
    /// control is still physically pressed and the console sees no edge at all. Only
    /// the hold that would have outlived the finger is gone.
    /// </summary>
    private void CancelLatch(string controlId)
    {
        latchesCancelled++;
        feedback.Perform(TouchFeedbackEvent.LatchReleased);
        latchObserver.OnLatchEvent(new TouchLatchEvent.Cancelled(controlId));
    }

    private void NoteLatchDown(ResolvedTouchControl control, long timeNanos, float x, float y)
    {
        if (!LatchEnabled(control))
        {
            // A control whose latch was turned off while it was held would otherwise
            // keep the hold with no gesture left to release it.
            if (latches.Remove(control.Id, out var stale) && stale.Latched)
            {
                latchesCleared++;
                if (analogTriggers.TryGetValue(control.Id, out var trigger))
                {
                    trigger.ClearLatch();
                }

                latchObserver.OnLatchEvent(new TouchLatchEvent.Cleared(
                    new HashSet<string>(StringComparer.Ordinal) { control.Id },
                    TouchReleaseReason.SettingsChanged));
            }

            return;
        }

        if (!latches.TryGetValue(control.Id, out var latch))
        {
            latch = new TouchControlLatch();
            latches[control.Id] = latch;
        }

        // The control is already published as pressed, so Engage found nothing to add
        // and stayed silent. It is still a press the user made. An analog trigger is
        // excluded because it acknowledges EVERY press from its own path.
        if (latch.Latched && !IsAnalogTrigger(control))
        {
            feedback.Perform(TouchFeedbackEvent.Press);
        }

        latch.OnDown(timeNanos, x, y, Config.Latch);
    }

    /// <summary>
    /// A contact ended. A quick tap on a latched control becomes a retrigger.
    ///
    /// The pulse is started HERE rather than on the press because a press on a latched
    /// control is ambiguous until it ends: quick means "press it again", held means
    /// "stop holding it".
    /// </summary>
    private void NoteLatchEnd(ResolvedTouchControl control, long timeNanos, bool cancelled)
    {
        if (!LatchEnabled(control) || !latches.TryGetValue(control.Id, out var latch))
        {
            return;
        }

        var retrigger = latch.OnEnd(timeNanos, cancelled, Config.Latch);

        // An analog trigger re-fires by PUBLISHING a value rather than by having its
        // hold masked away, so it runs its own pulse and must not also take the digital
        // mask; two of them would cancel out.
        if (IsAnalogTrigger(control))
        {
            return;
        }

        if (retrigger && latch.BeginRetrigger(timeNanos, Config.Latch))
        {
            retriggerPulses++;
        }
    }

    // ------------------------------------------------------------ analog trigger

    /// <summary>Whether this control has real travel on the far side.</summary>
    private static bool IsAnalogTrigger(ResolvedTouchControl control) =>
        control.Spec.Action is TouchControlAction.Trigger { Analog: true };

    /// <summary>
    /// A contact claimed an analog trigger.
    ///
    /// Deliberately publishes nothing at all. The press haptic still fires, because the
    /// user did hit a control and a control that feels dead to the touch reads as
    /// broken.
    /// </summary>
    private void BeginAnalogTrigger(ResolvedTouchControl control, TouchContact contact)
    {
        if (!IsAnalogTrigger(control))
        {
            return;
        }

        if (!analogTriggers.TryGetValue(control.Id, out var state))
        {
            state = new TouchAnalogTriggerState();
            analogTriggers[control.Id] = state;
        }

        latches.TryGetValue(control.Id, out var latch);
        state.OnDown(
            contact,
            control,
            layout.Region,
            latched: latch?.Latched == true,

            // The recognizer has already classified this press; read its answer rather
            // than re-deriving one. An Engage candidate is the second press of a double
            // tap, so it is on its way to CHOOSING a held level and must not resolve
            // into a full pull first.
            latchSelecting: latch?.Dwell == TouchDwell.Engage,
            Config.Trigger,

            // The SAME deliberate-hold base both latch dwells derive from: a press held
            // that long is deliberate, whichever gesture it turns out to belong to.
            holdResolveNanos: Config.Latch.HoldThresholdNanos);

        feedback.Perform(TouchFeedbackEvent.Press);
        ApplyAnalogTrigger(control);
    }

    private void MoveAnalogTrigger(ResolvedTouchControl control, TouchContact contact)
    {
        if (!analogTriggers.TryGetValue(control.Id, out var state))
        {
            return;
        }

        var detentWas = state.PhysicalDetent;
        var travel = state.OnMove(contact, Config.Trigger, AnalogDragSlop());
        if (state.PhysicalDetent && !detentWas)
        {
            triggerDetents++;
            feedback.Perform(TouchFeedbackEvent.TriggerDetent);
        }

        if (latches.TryGetValue(control.Id, out var latch))
        {
            switch (latch.OnMove(
                        contact.X, contact.Y, GestureSlop(),
                        LatchCommitDistance(), LatchCancelDistance(), commitTravel: travel))
            {
                case TouchLatchMove.Committed:
                    state.CommitLatch();
                    CommitLatch(control.Id, state.LatchedValue);
                    break;

                // The level goes with the hold. Nothing published moves: the finger is
                // still down and a live pull outranks a held level anyway.
                case TouchLatchMove.Cancelled:
                    state.ClearLatch();
                    CancelLatch(control.Id);
                    break;
            }
        }

        ApplyAnalogTrigger(control);
        Publish();
    }

    private void EndAnalogTrigger(
        ResolvedTouchControl control, TouchContact contact, bool cancelled)
    {
        if (!IsAnalogTrigger(control) ||
            !analogTriggers.TryGetValue(control.Id, out var state))
        {
            return;
        }

        var pulsed = state.OnEnd(
            contact,
            cancelled,

            // Read AFTER the recognizer has seen the release, so a press that removed
            // the hold is recognizable as the release gesture it was and does not also
            // fire a trigger click on the way out.
            latched: IsLatched(control.Id),
            Config.Latch.MaxTapDurationNanos,
            Config.Latch.RetriggerReleaseNanos);

        if (pulsed)
        {
            triggerPulses++;
        }

        ApplyAnalogTrigger(control);
    }

    /// <summary>
    /// Push one analog trigger's effective state into the published accumulators.
    ///
    /// Assigns rather than adjusts, exactly like <see cref="Disengage"/>: the trigger's
    /// value and its terminal click are both derived from one place every time, so no
    /// path can leave half of the pair behind.
    /// </summary>
    private void ApplyAnalogTrigger(ResolvedTouchControl control)
    {
        if (control.Spec.Action is not TouchControlAction.Trigger { Analog: true } action)
        {
            return;
        }

        analogTriggers.TryGetValue(control.Id, out var state);
        var latched = IsLatched(control.Id);
        var detent = state?.EffectiveDetent(latched, Config.Trigger) ?? false;

        // Capped below the detent, because on this personality the published BYTE is
        // the only thing that says whether the trigger clicked.
        var value = state is null
            ? 0f
            : detent
                ? 1f
                : MathF.Min(state.EffectiveValue(latched), Config.Trigger.SubDetentCeiling);

        // Per INSTANCE, exactly like every other contributor: two L triggers on screen
        // are two independent gestures, and the console is told the deeper of the two
        // rather than whichever moved last.
        TriggerLevels(action.Side)[control.Id] = value;

        var button = action.Side == ControlSide.Left
            ? ControllerButton.L2
            : ControllerButton.R2;

        if (detent)
        {
            logicalPresses[control.Id] = button;
        }
        else
        {
            logicalPresses.Remove(control.Id);
        }
    }

    /// <summary>Every analog trigger's published level, for the surface and diagnostics.</summary>
    private Dictionary<string, float> AnalogTriggerLevels()
    {
        var levels = new Dictionary<string, float>(StringComparer.Ordinal);
        foreach (var (id, state) in analogTriggers)
        {
            var control = layout.Control(id);
            if (control is null || !IsAnalogTrigger(control))
            {
                continue;
            }

            var latched = IsLatched(id);
            levels[id] = state.EffectiveDetent(latched, Config.Trigger)
                ? 1f
                : MathF.Min(state.EffectiveValue(latched), Config.Trigger.SubDetentCeiling);
        }

        return levels;
    }

    /// <summary>
    /// Which way each analog trigger's fill grows, for the surface.
    ///
    /// Every analog trigger in the layout, not only the ones that have been touched: a
    /// control at rest still has to know which way it WOULD fill.
    ///
    /// <code>
    ///   a swipe has declared itself   the SWIPE's own direction, frozen
    ///   a contact but no swipe yet    the axis frozen at pointer-down
    ///   nothing touching it           the axis for where the control now is
    /// </code>
    ///
    /// Routing all three through here rather than letting a surface re-derive them is
    /// what makes the freeze real.
    /// </summary>
    private Dictionary<string, TouchFillDirection> AnalogTriggerFills()
    {
        var fills = new Dictionary<string, TouchFillDirection>(StringComparer.Ordinal);
        foreach (var control in layout.Controls)
        {
            if (!IsAnalogTrigger(control))
            {
                continue;
            }

            analogTriggers.TryGetValue(control.Id, out var state);
            var live = state is not null && state.PointerId != TouchAnalogTriggerState.NoPointer
                ? state
                : null;

            fills[control.Id] = live?.SwipeFill ?? TouchTriggerTravel.FillDirection(
                live?.Axis ?? TouchTriggerTravel.InwardAxis(
                    control.CenterX, control.CenterY, layout.Region,
                    Config.Trigger.CenterEpsilonUnits));
        }

        return fills;
    }

    /// <summary>Drop every trigger's travel state and the levels any hold was carrying.</summary>
    private void ClearAnalogTriggers()
    {
        foreach (var id in analogTriggers.Keys)
        {
            leftTriggerLevels.Remove(id);
            rightTriggerLevels.Remove(id);
            logicalPresses.Remove(id);
        }

        analogTriggers.Clear();
    }

    /// <summary>
    /// The next moment the engine has timed work, in the same clock as
    /// <see cref="TouchContact.TimeNanos"/>. Null when it is purely event-driven.
    ///
    /// A deadline can only ever APPEAR as a result of a contact event, so a host that
    /// consults this after every contact batch — and again after each
    /// <see cref="OnTick"/> — cannot miss one.
    /// </summary>
    public long? NextDeadlineNanos()
    {
        long? best = null;

        void Consider(long? deadline)
        {
            if (deadline is { } value && (best is null || value < best))
            {
                best = value;
            }
        }

        foreach (var latch in latches.Values)
        {
            Consider(latch.NextDeadlineNanos());
        }

        foreach (var trigger in analogTriggers.Values)
        {
            Consider(trigger.NextDeadlineNanos());
        }

        return best;
    }

    /// <summary>
    /// Advance timed gesture work to <paramref name="nowNanos"/>.
    ///
    /// Safe to call at any time: it reads live engine state rather than anything
    /// captured when the work was scheduled, so a tick that lands after a release, a
    /// layout change or a teardown finds an empty map and does nothing. That is
    /// deliberately the only mechanism preventing a pending retrigger from resurrecting
    /// a button after the session ended — there is no queue to invalidate.
    /// </summary>
    public void OnTick(long nowNanos)
    {
        if (latches.Count == 0 && analogTriggers.Count == 0)
        {
            return;
        }

        var changed = false;

        foreach (var (id, latch) in latches.ToList())
        {
            if (latch.HoldDeadlineNanos >= 1 && latch.HoldDeadlineNanos <= nowNanos)
            {
                switch (latch.Dwell)
                {
                    // Armed only. The control is still an ordinary held button and
                    // letting go now simply ends the press; the slide is what commits.
                    case TouchDwell.Engage:
                        latch.ArmEngage();
                        latchesArmed++;
                        feedback.Perform(TouchFeedbackEvent.LatchArmed);

                        // A trigger's arming press deferred its full-pull resolve so the
                        // level the slide selects is the first thing it ever publishes.
                        // Restart it from HERE.
                        if (analogTriggers.TryGetValue(id, out var arming))
                        {
                            arming.ArmLatchSelection(nowNanos, Config.Latch.HoldThresholdNanos);
                        }

                        break;

                    case TouchDwell.Release:
                    {
                        latch.CompleteRelease();
                        latchesReleased++;
                        feedback.Perform(TouchFeedbackEvent.LatchReleased);
                        latchObserver.OnLatchEvent(new TouchLatchEvent.Released(id));

                        var control = layout.Control(id);
                        if (analogTriggers.TryGetValue(id, out var releasing))
                        {
                            releasing.ClearLatch();
                        }

                        // The finger that performed the release gesture is still down,
                        // and stays authoritative until it lifts: dropping the button at
                        // this instant would be a release edge the user never made.
                        if (!controlToContact.ContainsKey(id))
                        {
                            if (control is not null)
                            {
                                Disengage(control);
                            }
                        }
                        else if (control is not null && IsAnalogTrigger(control))
                        {
                            // The hold is gone but the finger that removed it is still on
                            // the trigger. Re-arm, so the SAME contact can slide to a new
                            // level. Silently — the release tick has just fired, and a
                            // second identical tick on top of it reads as one blurred buzz.
                            latch.ArmEngage();
                            latchesArmed++;
                        }

                        if (control is not null && IsAnalogTrigger(control))
                        {
                            ApplyAnalogTrigger(control);
                        }

                        changed = true;
                        break;
                    }

                    default:
                        latch.CancelDwell();
                        break;
                }
            }

            if (latch.RetriggerDeadlineNanos >= 1 && latch.RetriggerDeadlineNanos <= nowNanos)
            {
                latch.EndRetrigger();
                changed = true;
            }
        }

        foreach (var (id, state) in analogTriggers.ToList())
        {
            var control = layout.Control(id);
            if (control is null)
            {
                continue;
            }

            if (state.HoldResolveDeadlineNanos >= 1 && state.HoldResolveDeadlineNanos <= nowNanos)
            {
                // Runs for an arming press too, but only AFTER it has armed. "Tap it,
                // then keep holding it" is ordinary play that games ask for directly, and
                // a trigger that published nothing for as long as the thumb stayed down
                // would break exactly that. Ordering it after the arm is what stops the
                // other half — a press on its way to selecting a PARTIAL level — sending
                // a full pull and the detent first.
                //
                // The fallback WINNING is what consumes the hold candidate: the press has
                // now been answered as an ordinary held trigger, and a slide made after
                // that answer must not still be able to lock a partial level.
                if (state.ResolveFullPull() && latches.TryGetValue(id, out var armed))
                {
                    armed.AbandonArm();
                }

                triggerDetents++;
                feedback.Perform(TouchFeedbackEvent.TriggerDetent);
                ApplyAnalogTrigger(control);
                changed = true;
            }

            if (state.PulseDeadlineNanos >= 1 && state.PulseDeadlineNanos <= nowNanos)
            {
                state.EndPulse();
                ApplyAnalogTrigger(control);
                changed = true;
            }
        }

        if (changed)
        {
            Publish();
        }
    }

    /// <summary>
    /// Drop every latch without touching contact ownership.
    ///
    /// A control a finger is still on stays pressed — that finger is now the only thing
    /// holding it, and it will release normally on lift. Anything else returns to rest
    /// here, which is the point.
    /// </summary>
    private void ClearLatches(TouchReleaseReason reason)
    {
        if (latches.Count == 0)
        {
            return;
        }

        var cleared = latches
            .Where(pair => pair.Value.Latched)
            .Select(pair => pair.Key)
            .ToHashSet(StringComparer.Ordinal);

        latches.Clear();

        foreach (var id in cleared)
        {
            // The level goes with the hold. A trigger that kept it would republish it
            // the moment any later gesture latched the same control.
            if (analogTriggers.TryGetValue(id, out var trigger))
            {
                trigger.ClearLatch();
            }

            var control = layout.Control(id);
            if (control is null)
            {
                continue;
            }

            if (!controlToContact.ContainsKey(id))
            {
                Disengage(control);
            }

            if (IsAnalogTrigger(control))
            {
                ApplyAnalogTrigger(control);
            }
        }

        if (cleared.Count > 0)
        {
            latchesCleared += cleared.Count;
            latchObserver.OnLatchEvent(new TouchLatchEvent.Cleared(cleared, reason));
        }

        // Unconditional: an in-flight retrigger mask that was dropped here would
        // otherwise leave the published state suppressed with nothing left to lift it.
        Publish();
    }

    /// <summary>
    /// Drop every touch contribution and every ownership, for a stated reason.
    ///
    /// The one operation every invalidating boundary calls. Idempotent by construction:
    /// it assigns rather than toggles, so calling it twice cannot invent a press, and
    /// re-publishing an already-neutral contribution is a no-op at the sink.
    /// </summary>
    public void ReleaseAll(TouchReleaseReason reason)
    {
        releaseAllCount++;
        lastReleaseReason = reason;

        var clearedLatches = latches
            .Where(pair => pair.Value.Latched)
            .Select(pair => pair.Key)
            .ToHashSet(StringComparer.Ordinal);

        latches.Clear();
        ClearAnalogTriggers();
        contactToControl.Clear();
        controlToContact.Clear();
        facePresses.Clear();
        logicalPresses.Clear();
        leftTriggerLevels.Clear();
        rightTriggerLevels.Clear();
        stickVectors.Clear();
        stickOwners.Clear();
        dpadStates.Clear();
        dpadOwner = null;

        if (clearedLatches.Count > 0)
        {
            latchesCleared += clearedLatches.Count;
            latchObserver.OnLatchEvent(new TouchLatchEvent.Cleared(clearedLatches, reason));
        }

        Publish();
    }

    public TouchDiagnosticsSnapshot Diagnostics() => new()
    {
        OwnedControls = controlToContact.Count,
        HeldControls = controlToContact.Keys.ToHashSet(StringComparer.Ordinal),
        ActiveContacts = contactToControl.Count,
        ContactsClaimed = contactsClaimed,
        ContactsUnclaimed = contactsUnclaimed,
        ContactsContested = contactsContested,
        ContactsCancelled = contactsCancelled,
        ReleaseAllCount = releaseAllCount,
        LastReleaseReason = lastReleaseReason,
        LatchedControls = LatchedControlIds(),
        ArmedControls = ArmedControlIds(),
        LatchesArmed = latchesArmed,
        LatchesEngaged = latchesEngaged,
        LatchesReleased = latchesReleased,
        LatchesCleared = latchesCleared,
        RetriggerPulses = retriggerPulses,
        LatchesCancelled = latchesCancelled,
        TriggerDetents = triggerDetents,
        TriggerPulses = triggerPulses,
        AnalogTriggers = AnalogTriggerLevels(),
        AnalogTriggerFills = AnalogTriggerFills(),
        StickVectors = new Dictionary<string, TouchVector>(stickVectors, StringComparer.Ordinal),
        LastContactTimeNanos = lastContactTimeNanos,
        LeftStick = PublishedStick(ControlSide.Left),
        RightStick = PublishedStick(ControlSide.Right),
        Dpad = PublishedDpad(),
    };

    /// <summary>The controls a latch is holding down right now.</summary>
    public IReadOnlySet<string> LatchedControlIds() => latches
        .Where(pair => pair.Value.Latched)
        .Select(pair => pair.Key)
        .ToHashSet(StringComparer.Ordinal);

    /// <summary>The controls one deliberate slide away from becoming a hold.</summary>
    public IReadOnlySet<string> ArmedControlIds() => latches
        .Where(pair => pair.Value.Armed)
        .Select(pair => pair.Key)
        .ToHashSet(StringComparer.Ordinal);

    /// <summary>Which control, if any, currently owns this contact.</summary>
    public string? OwnerOf(long contactId) =>
        contactToControl.TryGetValue(contactId, out var id) ? id : null;

    /// <summary>Which contact, if any, owns this control.</summary>
    public long? ContactOn(string controlId) =>
        controlToContact.TryGetValue(controlId, out var id) ? id : null;

    // ------------------------------------------------------------------ internals

    /// <summary>
    /// Pick the control a Down at this point claims.
    ///
    /// <code>
    /// 1  highest declared priority        authored, rarely used
    /// 2  highest z-order                  the control drawn in front
    /// 3  most plainly inside              a deterministic last resort
    /// </code>
    ///
    /// Z-order sits above centrality on purpose: once instances may be freely placed
    /// and stacked, the control the user can SEE on top is the one they believe they
    /// are pressing, and any other answer is a surprise.
    /// </summary>
    private ResolvedTouchControl? HitTest(float x, float y)
    {
        ResolvedTouchControl? best = null;
        var bestDistance = float.MaxValue;

        foreach (var control in layout.Controls)
        {
            if (!control.HitTest(x, y))
            {
                continue;
            }

            if (best is null)
            {
                best = control;
                bestDistance = control.NormalizedDistance(x, y);
                continue;
            }

            var order = control.Spec.Priority.CompareTo(best.Spec.Priority);
            if (order == 0)
            {
                order = control.Spec.ZIndex.CompareTo(best.Spec.ZIndex);
            }

            if (order > 0)
            {
                best = control;
                bestDistance = control.NormalizedDistance(x, y);
                continue;
            }

            if (order < 0)
            {
                continue;
            }

            var distance = control.NormalizedDistance(x, y);
            if (distance < bestDistance)
            {
                best = control;
                bestDistance = distance;
            }
        }

        return best;
    }

    /// <summary>Apply a control's value for a contact at this point.</summary>
    private void Engage(ResolvedTouchControl control, float x, float y)
    {
        var id = control.Id;

        switch (control.Spec.Action)
        {
            case TouchControlAction.Face face:
                if (!facePresses.ContainsKey(id))
                {
                    facePresses[id] = face.Position;
                    feedback.Perform(TouchFeedbackEvent.Press);
                }

                break;

            case TouchControlAction.Logical logical:
                if (!logicalPresses.ContainsKey(id))
                {
                    logicalPresses[id] = logical.Button;
                    feedback.Perform(TouchFeedbackEvent.Press);
                }

                break;

            case TouchControlAction.Trigger trigger:
            {
                // A trigger with real travel says nothing on the way down: what the
                // press means is not known yet, and full travel IS the terminal click on
                // the one personality that has one.
                if (trigger.Analog)
                {
                    return;
                }

                // Both halves, coherently. A physical trigger publishes its digital bit
                // AND its analog value, and the adapter's seam reads either; a touch
                // trigger that published only one would be a second contract.
                TriggerLevels(trigger.Side)[id] = 1f;
                var button = trigger.Side == ControlSide.Left
                    ? ControllerButton.L2
                    : ControllerButton.R2;

                if (!logicalPresses.ContainsKey(id))
                {
                    logicalPresses[id] = button;
                    feedback.Perform(TouchFeedbackEvent.Press);
                }

                break;
            }

            case TouchControlAction.Stick stick:
                stickVectors[id] = TouchStick.Resolve(
                    x - control.CenterX,
                    y - control.CenterY,
                    control.TrackingRadius,
                    Config.StickDeadzone);

                // First mover wins, and keeps it until its contact ends. A second stick
                // instance being touched at the same time is not an error and is not a
                // steal: it simply says nothing yet.
                if (!stickOwners.ContainsKey(stick.Side))
                {
                    stickOwners[stick.Side] = id;
                }

                break;

            case TouchControlAction.Directions:
            {
                var owned = dpadOwner is null || dpadOwner == id;
                var previous = dpadStates.TryGetValue(id, out var held) ? held : DpadState.None;
                var next = TouchDpad.Resolve(
                    x - control.CenterX,
                    y - control.CenterY,
                    control.TrackingRadius,
                    Config,

                    // Hysteresis is a property of the gesture in progress, so it reads
                    // THIS instance's previous direction rather than the published one,
                    // which may belong to a different instance.
                    previous);

                var changed = !dpadStates.TryGetValue(id, out var was) || was != next;
                dpadStates[id] = next;
                dpadOwner ??= id;

                // Only the instance the console is listening to may buzz; a second D-pad
                // being brushed must not rattle.
                if (changed && owned)
                {
                    feedback.Perform(next == DpadState.None
                        ? TouchFeedbackEvent.Release
                        : TouchFeedbackEvent.DirectionChange);
                }

                break;
            }
        }
    }

    /// <summary>Return a control to rest. Assigns rest values rather than undoing a delta.</summary>
    private void Disengage(ResolvedTouchControl control)
    {
        var id = control.Id;

        switch (control.Spec.Action)
        {
            case TouchControlAction.Face:
                facePresses.Remove(id);
                break;

            case TouchControlAction.Logical:
                logicalPresses.Remove(id);
                break;

            // An analog trigger's rest value is not necessarily zero — a hold may still
            // be on it — and ApplyAnalogTrigger is the one place that decides. Two
            // writers for one axis is how half a state survives.
            case TouchControlAction.Trigger trigger:
                if (trigger.Analog)
                {
                    ApplyAnalogTrigger(control);
                }
                else
                {
                    TriggerLevels(trigger.Side).Remove(id);
                    logicalPresses.Remove(id);
                }

                break;

            case TouchControlAction.Stick stick:
                // Exact centre immediately. A knob may animate home for looks, but the
                // axis is neutral the instant the thumb leaves.
                stickVectors.Remove(id);
                if (stickOwners.TryGetValue(stick.Side, out var owner) && owner == id)
                {
                    stickOwners.Remove(stick.Side);
                    HandOffStick(stick.Side);
                }

                break;

            case TouchControlAction.Directions:
                dpadStates.Remove(id);
                if (dpadOwner == id)
                {
                    dpadOwner = null;
                    HandOffDpad();
                }

                break;
        }
    }

    private Dictionary<string, float> TriggerLevels(ControlSide side) =>
        side == ControlSide.Left ? leftTriggerLevels : rightTriggerLevels;

    /// <summary>
    /// Give a released stick to another instance that is still being held.
    ///
    /// In layout order, so the answer is deterministic rather than whatever the hash map
    /// happened to iterate first. Without this, letting go of one of two duplicated
    /// sticks would leave the other one dead until the thumb on it moved again.
    /// </summary>
    private void HandOffStick(ControlSide side)
    {
        var next = layout.Controls.FirstOrDefault(candidate =>
            candidate.Spec.Action is TouchControlAction.Stick stick && stick.Side == side &&
            controlToContact.ContainsKey(candidate.Id));

        if (next is not null)
        {
            stickOwners[side] = next.Id;
        }
    }

    private void HandOffDpad() =>
        dpadOwner = layout.Controls.FirstOrDefault(candidate =>
            candidate.Spec.Action is TouchControlAction.Directions &&
            controlToContact.ContainsKey(candidate.Id))?.Id;

    /// <summary>What the console is being told each vector control is doing.</summary>
    private TouchVector PublishedStick(ControlSide side) =>
        stickOwners.TryGetValue(side, out var owner) &&
        stickVectors.TryGetValue(owner, out var vector)
            ? vector
            : TouchVector.Zero;

    private DpadState PublishedDpad() =>
        dpadOwner is { } owner && dpadStates.TryGetValue(owner, out var state)
            ? state
            : DpadState.None;

    /// <summary>Highest level any live instance of this trigger is asking for.</summary>
    private float PublishedTrigger(ControlSide side, IReadOnlySet<string> masked)
    {
        var levels = TriggerLevels(side);
        var best = 0f;
        foreach (var (id, value) in levels)
        {
            if (!masked.Contains(id) && value > best)
            {
                best = value;
            }
        }

        return best;
    }

    /// <summary>
    /// Compose and emit the whole contribution.
    ///
    /// <code>
    /// digital   any live instance holding a binding keeps that binding pressed
    /// trigger   the deepest live instance of that side wins
    /// stick     the owning instance speaks; the others say nothing
    /// D-pad     the owning instance speaks; the others say nothing
    /// </code>
    ///
    /// The digital rule is what makes duplicates behave: pressing a second A and then
    /// releasing the first leaves one contributor, so the console never sees a release
    /// edge the user did not make.
    ///
    /// The retrigger mask is applied HERE as a set of INSTANCE ids to skip, rather than
    /// by mutating the accumulators. Ownership, latch state and the press/release
    /// bookkeeping all stay exactly as they were; nothing has to be undone when it
    /// expires, and — because the mask is per instance — tapping one held A to re-fire
    /// it cannot silence the other one.
    /// </summary>
    private void Publish()
    {
        var masked = latches.Any(pair => pair.Value.Retriggering)
            ? latches.Where(pair => pair.Value.Retriggering)
                .Select(pair => pair.Key)
                .ToHashSet(StringComparer.Ordinal)
            : (IReadOnlySet<string>)new HashSet<string>(StringComparer.Ordinal);

        var faces = ControllerButtonSet.Empty;
        foreach (var (id, position) in facePresses)
        {
            if (!masked.Contains(id))
            {
                faces = faces.With(position.Positional());
            }
        }

        var logicals = ControllerButtonSet.Empty;
        foreach (var (id, button) in logicalPresses)
        {
            if (!masked.Contains(id))
            {
                logicals = logicals.With(button);
            }
        }

        var leftStick = PublishedStick(ControlSide.Left);
        var rightStick = PublishedStick(ControlSide.Right);

        var next = new TouchContribution
        {
            LeftX = TouchAxis.ToBridge(leftStick.X),
            LeftY = TouchAxis.ToBridge(leftStick.Y),
            RightX = TouchAxis.ToBridge(rightStick.X),
            RightY = TouchAxis.ToBridge(rightStick.Y),
            LeftTrigger = TouchAxis.TriggerToBridge(PublishedTrigger(ControlSide.Left, masked)),
            RightTrigger = TouchAxis.TriggerToBridge(PublishedTrigger(ControlSide.Right, masked)),
            Dpad = PublishedDpad(),
            PositionalButtons = faces,
            LogicalButtons = logicals,
        };

        if (next == published)
        {
            return;
        }

        published = next;
        onContribution(next);
    }
}
