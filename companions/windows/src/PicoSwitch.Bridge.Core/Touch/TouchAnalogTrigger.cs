namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// Where a trigger's invisible travel axis points, and how far along it the finger
/// has pulled.
///
/// Pure geometry over the resolved layout: no state, no rendering, no contacts.
///
/// ## The trigger is the handle; the screen is the travel space
///
/// A touchscreen has no trigger travel, and the two obvious substitutes are both
/// wrong. Finger PRESSURE is not reported usefully by ordinary capacitive panels. A
/// visible SLIDER spends permanent gameplay screen space on a control that is idle
/// almost all the time, and stops looking like the trigger it represents.
///
/// So travel is finger DISPLACEMENT after touching the trigger, projected onto an
/// invisible axis. The control stays a compact <c>L</c> or <c>R</c>; the travel space
/// costs nothing because it is only gesture space.
///
/// ## Why the direction is derived and never declared
///
/// The axis points from the control toward the middle of the playable rectangle, so
/// the gesture is always "pull it into the screen". Encoding "L means drag down"
/// instead would be correct exactly once — for the shipped layout — and wrong for
/// every user who moves the control, which the editor exists to let them do.
/// </summary>
public static class TouchTriggerTravel
{
    private static readonly TouchVector DegenerateAxis = new(0f, 1f);

    /// <summary>
    /// The direction in which pulling increases travel, as a unit vector in the
    /// layout's own coordinate space (<c>y</c> grows DOWNWARD).
    ///
    /// ## Derived in NORMALIZED region space, not in pixels
    ///
    /// "Toward the middle of the playable rectangle" is a statement about the LAYOUT,
    /// and the layout's own space is normalized. Taking the direction in pixels
    /// instead makes it a statement about the DEVICE: the same authored control
    /// produced an axis 55 degrees off vertical on one panel and 45 on another, purely
    /// because a wider window puts its centre further right. The gesture changed shape
    /// per device while the layout did not.
    ///
    /// That was measured, not reasoned. On a 1920x1025 handheld the shipped GameCube
    /// <c>L</c> resolved to <c>(0.818, 0.575)</c>, and a thumb pulling straight DOWN —
    /// the natural motion for a trigger at the top of the screen — recovered only
    /// <c>0.575</c> of its travel while paying a full-travel distance inflated by the
    /// horizontal budget it never spent. Dividing by the region's own extents first
    /// gives <c>(0.605, 0.796)</c> on EVERY window shape.
    ///
    /// The epsilon is still measured in real pixels, because "parked on the middle of
    /// the screen" is a physical fact rather than a normalized one. There the axis
    /// becomes the inward normal of the NEAREST EDGE, which is still position-derived
    /// and stable.
    /// </summary>
    public static TouchVector InwardAxis(
        float centerX, float centerY, TouchLayoutRegion region, float centerEpsilonUnits)
    {
        if (region.Width <= 0f || region.Height <= 0f ||
            !float.IsFinite(centerX) || !float.IsFinite(centerY))
        {
            return DegenerateAxis;
        }

        var dx = ((region.Left + region.Right) / 2f) - centerX;
        var dy = ((region.Top + region.Bottom) / 2f) - centerY;
        var length = MathF.Sqrt((dx * dx) + (dy * dy));
        var epsilon = centerEpsilonUnits * region.UnitScale;

        if (length > 0f && length >= epsilon)
        {
            // Normalized by the region's own extents, so the direction is the layout's
            // and not the window's.
            var nx = dx / region.Width;
            var ny = dy / region.Height;
            var normalized = MathF.Sqrt((nx * nx) + (ny * ny));
            if (normalized > 0f)
            {
                return new TouchVector(nx / normalized, ny / normalized);
            }
        }

        // Nearest edge, pointing inward. Ties resolve in a fixed order rather than by
        // whichever comparison happened to run first, so a control on an exact centre
        // line produces the same axis every time it is touched.
        var toTop = centerY - region.Top;
        var toBottom = region.Bottom - centerY;
        var toLeft = centerX - region.Left;
        var toRight = region.Right - centerX;
        var nearest = MathF.Min(MathF.Min(toTop, toBottom), MathF.Min(toLeft, toRight));

        if (nearest == toTop)
        {
            return new TouchVector(0f, 1f);
        }

        if (nearest == toBottom)
        {
            return new TouchVector(0f, -1f);
        }

        return nearest == toLeft ? new TouchVector(1f, 0f) : new TouchVector(-1f, 0f);
    }

    /// <summary>
    /// How far along <paramref name="axis"/> the finger must travel for a full pull,
    /// in the layout's real coordinates.
    ///
    /// ## Two budgets; the pull ends when either one is spent
    ///
    /// <code>
    ///   horizontal budget   Rx = travelFraction * min(width, height)
    ///   vertical budget     Ry = Rx * verticalTravelRatio
    ///
    ///   fullTravel(axis) = min( Rx / |axis.x| , Ry / |axis.y| )
    /// </code>
    ///
    /// The guarantee that buys: a full pull NEVER displaces the finger by more than
    /// <c>Ry</c> vertically or more than <c>Rx</c> horizontally, whatever direction
    /// the axis points.
    ///
    /// ## What this replaced, and why
    ///
    /// A single shared distance came first, and hardware feel testing rejected it: the
    /// same pixels are a quarter of the width but half of the height, so near-vertical
    /// pulls had to be dragged the whole way down the glass.
    ///
    /// The repair was a weighted BLEND, and device measurement rejected that too. It
    /// charges the horizontal budget in proportion to how much of the AXIS lies along
    /// X — but a thumb pulling a top-placed trigger moves DOWN, spending no width at
    /// all, and still paid for it. Measured: full travel came to 566 px and a
    /// straight-down stroke needed 985 px, 96% of the usable height.
    ///
    /// Taking the MINIMUM removes that term: the horizontal budget can only ever end
    /// the pull EARLY, never lengthen it.
    /// </summary>
    public static float FullTravelPx(
        TouchLayoutRegion region, TouchVector axis, float travelFraction, float verticalTravelRatio)
    {
        var horizontal = MathF.Min(region.Width, region.Height) * travelFraction;
        if (!float.IsFinite(horizontal) || horizontal <= 0f)
        {
            return 0f;
        }

        var vertical = horizontal * verticalTravelRatio;
        var alongX = MathF.Abs(axis.X);
        var alongY = MathF.Abs(axis.Y);

        // A component of zero cannot exhaust its budget at any distance, so it simply
        // does not bound the pull. Both being zero is not a direction at all; the
        // horizontal budget is the deterministic answer there.
        var byWidth = alongX > 0f ? horizontal / alongX : float.PositiveInfinity;
        var byHeight = alongY > 0f ? vertical / alongY : float.PositiveInfinity;
        var travel = MathF.Min(byWidth, byHeight);
        return float.IsFinite(travel) ? travel : horizontal;
    }

    /// <summary>
    /// How far the contact has pulled along <paramref name="axis"/>.
    ///
    /// The vector PROJECTION, not the straight-line distance, and that is what makes
    /// the invisible axis usable: a thumb sweeps an arc rather than a line, so
    /// perpendicular drift has to cost nothing. Motion opposite the axis is clamped
    /// away rather than allowed to go negative.
    /// </summary>
    public static float ProjectedTravelPx(float dx, float dy, TouchVector axis)
    {
        if (!float.IsFinite(dx) || !float.IsFinite(dy))
        {
            return 0f;
        }

        var projected = (dx * axis.X) + (dy * axis.Y);
        return projected > 0f ? projected : 0f;
    }

    /// <summary><see cref="ProjectedTravelPx"/> as a <c>0..1</c> trigger value.</summary>
    public static float AnalogValue(float dx, float dy, TouchVector axis, float fullTravelPx)
    {
        if (!float.IsFinite(fullTravelPx) || fullTravelPx <= 0f)
        {
            return 0f;
        }

        return Math.Clamp(ProjectedTravelPx(dx, dy, axis) / fullTravelPx, 0f, 1f);
    }

    /// <summary>
    /// The cardinal direction <paramref name="vector"/> leans toward.
    ///
    /// Four answers rather than a rotated diagonal bar: the fill is drawn inside a
    /// small pad whose silhouette is the control's own, and a diagonal wipe across a
    /// rounded rectangle reads as a shading artefact rather than as a level.
    ///
    /// ## What to pass, and why it is not the travel axis
    ///
    /// The vector is the DISPLACEMENT of the swipe in progress. It is deliberately NOT
    /// the inward axis, even though that is what the value is projected onto, because
    /// the two answer different questions and a diagonal axis makes them disagree. The
    /// shipped GameCube <c>R</c> has axis <c>(-0.732, +0.681)</c>: a straight DOWNWARD
    /// swipe projects positively onto it and correctly increases travel, but the axis
    /// leans horizontally, so filling from the axis showed a bar growing leftward
    /// while the thumb moved down.
    ///
    /// <code>
    ///   value  &lt;- projection onto the inward axis   (where the trigger is)
    ///   fill   &lt;- cardinal of the actual swipe      (where the thumb went)
    /// </code>
    /// </summary>
    public static TouchFillDirection FillDirection(TouchVector vector)
    {
        if (MathF.Abs(vector.Y) >= MathF.Abs(vector.X))
        {
            return vector.Y >= 0f ? TouchFillDirection.Down : TouchFillDirection.Up;
        }

        return vector.X >= 0f ? TouchFillDirection.Right : TouchFillDirection.Left;
    }
}

/// <summary>
/// Which edge of a trigger's pad its fill grows from, as a cardinal direction.
///
/// <see cref="Down"/> means the fill starts at the top edge and grows downward — the
/// direction the finger travels, so the pad empties toward where the thumb came from
/// and fills toward where it is going.
///
/// Chosen from the swipe the user is actually making, never from a control's
/// identity, so a trigger moved in the editor re-presents itself with nothing to
/// configure.
/// </summary>
public enum TouchFillDirection
{
    Down,
    Up,
    Right,
    Left,
}

/// <summary>
/// One analog trigger's live gesture, in the engine's ownership.
///
/// ## What the finger is doing, and what is published, are different questions
///
/// <code>
/// pressed, not yet moved       PendingTap    publishes nothing
/// moved past the drag slop     AnalogDrag    publishes the projection; fixes the fill
/// still, past the hold base    a full pull   publishes full travel + detent
/// still, but DEFINING a hold   a selection   publishes nothing until it slides
/// that selection, timed out    a full pull   publishes full travel; arm consumed
/// released without dragging    a tap         publishes a brief full pulse
/// nothing touching it          at rest       publishes the LATCHED level, if any
/// </code>
///
/// ## A press that is choosing a level is not a press that is holding one
///
/// Those two are the same motion for the first third of a second, and resolving the
/// wrong one first is observable on the wire:
///
/// <code>
///   tap, press, hold, slide to 40%, release      &lt;- the user's gesture
///
///   wrong:  ... 0 -&gt; 1.0 + DETENT -&gt; 0.9 -&gt; ... -&gt; 0.4      held at 0.4
///   right:  ... 0 ----------------&gt; 0.1 -&gt; ... -&gt; 0.4      held at 0.4
/// </code>
///
/// The excursion in the first line is not cosmetic. On this personality full travel
/// IS the terminal click, and a GameCube game can act on the click and on partial
/// travel differently — so a 40% hold that fires a click on its way there has sent an
/// input the user never made.
///
/// ## Nothing is published on the way down
///
/// The single most damaging thing this control could do is assert a full pull the
/// instant it is touched. So a fresh contact publishes NOTHING, and the press
/// resolves later. A tap therefore lands on RELEASE. That is a real cost in feel and
/// it is the side of the trade the spec picks deliberately: a late tap is worse
/// input, a speculative click is WRONG input.
///
/// ## The hold is the shared one
///
/// There is no second latch system here. <see cref="TouchControlLatch"/> decides
/// WHETHER this control is held, using the same gesture every digital control uses;
/// this class only remembers at what LEVEL, which is the one thing a Boolean cannot
/// carry.
/// </summary>
internal sealed class TouchAnalogTriggerState
{
    public const long NoPointer = -1L;

    private static readonly TouchVector DefaultAxis = new(0f, 1f);

    private float originX;
    private float originY;
    private long pressStartNanos;

    /// <summary>A still press was resolved into a deliberate full pull.</summary>
    private bool resolvedFull;

    /// <summary>This contact performed the gesture that created the current hold.</summary>
    private bool committedByThisContact;

    /// <summary>The contact that owns this trigger, or <see cref="NoPointer"/>.</summary>
    public long PointerId { get; private set; } = NoPointer;

    /// <summary>
    /// The travel axis, frozen for the whole gesture.
    ///
    /// Recomputed per gesture and never mid-gesture: an animated inset or a window
    /// resize would otherwise rotate the axis under a thumb that is already pulling,
    /// and the trigger would move on its own.
    /// </summary>
    public TouchVector Axis { get; private set; } = DefaultAxis;

    public float FullTravelPx { get; private set; }

    /// <summary>The contact crossed the drag slop; permanent for this gesture.</summary>
    public bool Dragging { get; private set; }

    /// <summary>
    /// Which way this gesture's swipe went, or null before there is a swipe.
    ///
    /// Established EXACTLY ONCE, at the moment the contact crosses the drag slop, from
    /// the whole displacement since pointer-down. Frozen from then on: a thumb sweeps
    /// an arc, and re-reading it per frame would flip the bar between down and
    /// sideways in the middle of a pull. Presentation only.
    /// </summary>
    public TouchFillDirection? SwipeFill { get; private set; }

    /// <summary>The press began on a control that was already held.</summary>
    public bool StartedLatched { get; private set; }

    /// <summary>
    /// This press is the second half of a double tap, so it is a candidate for
    /// DEFINING a hold rather than for being one.
    /// </summary>
    public bool LatchSelecting { get; private set; }

    /// <summary>When a still press becomes a full pull; zero when nothing is waiting.</summary>
    public long HoldResolveDeadlineNanos { get; private set; }

    /// <summary>What the finger is asking for right now, when it is asking for anything.</summary>
    public float PhysicalValue { get; private set; }

    public bool PhysicalDetent { get; private set; }

    /// <summary>Whether the finger currently overrides the held level.</summary>
    public bool PhysicalActive => Dragging || resolvedFull;

    /// <summary>What a tap publishes, and until when; zero deadline when idle.</summary>
    public float PulseValue { get; private set; }

    public long PulseDeadlineNanos { get; private set; }

    public bool Pulsing => PulseDeadlineNanos != 0L;

    /// <summary>
    /// The level this control holds when a latch is holding it.
    ///
    /// Meaningful only while the recognizer says latched — that, not this field, is
    /// what decides whether a hold exists. Cleared whenever the latch is, so a stale
    /// level can never be republished by a later hold.
    /// </summary>
    public float LatchedValue { get; private set; }

    public bool LatchedDetent { get; private set; }

    /// <summary>
    /// The detent, with the chatter guard the wire cannot supply.
    ///
    /// Equal thresholds would flicker the terminal click while a thumb sits on the
    /// boundary, and on this personality that click is a gameplay button.
    /// </summary>
    public static bool DetentWithHysteresis(float value, bool engaged, TouchTriggerConfig config) =>
        engaged ? value > config.DetentReleaseFraction : value >= config.DetentEngageFraction;

    /// <summary>
    /// A contact claimed the trigger. Publishes nothing; see the type doc.
    ///
    /// <paramref name="latchSelecting"/> is what stops a partial hold passing through
    /// a full trigger on its way to the level the user meant. That press starts with
    /// no resolve pending at all, and the recognizer re-arms one through
    /// <see cref="ArmLatchSelection"/> once the slide that would use it is available.
    /// </summary>
    public void OnDown(
        TouchContact contact,
        ResolvedTouchControl control,
        TouchLayoutRegion region,
        bool latched,
        bool latchSelecting,
        TouchTriggerConfig config,
        long holdResolveNanos)
    {
        PointerId = contact.Id;
        originX = contact.X;
        originY = contact.Y;
        pressStartNanos = contact.TimeNanos;
        Axis = TouchTriggerTravel.InwardAxis(
            control.CenterX, control.CenterY, region, config.CenterEpsilonUnits);
        FullTravelPx = TouchTriggerTravel.FullTravelPx(
            region, Axis, config.TravelFraction, config.VerticalTravelRatio);
        Dragging = false;
        SwipeFill = null;
        resolvedFull = false;
        committedByThisContact = false;
        StartedLatched = latched;
        LatchSelecting = latchSelecting;
        PhysicalValue = 0f;
        PhysicalDetent = false;

        // A new press supersedes the previous one's tail: the finger is back on the
        // control, so the pulse has already said what it had to say.
        PulseDeadlineNanos = 0L;

        // Suppressed on a latched control, where a still press is already the gesture
        // that REMOVES the hold and must not also mean "pull it fully", and on a
        // latch-defining press, which is on its way to selecting a level and must not
        // publish one it did not select.
        HoldResolveDeadlineNanos = !latched && !latchSelecting && contact.TimeNanos > 0L
            ? contact.TimeNanos + holdResolveNanos
            : 0L;
    }

    /// <summary>
    /// The hold gesture armed on this contact: a slide from here selects a level.
    ///
    /// Restarts the deliberate-hold wait that <see cref="OnDown"/> deliberately did
    /// not, so the two things a still press can mean stay ORDERED rather than raced.
    ///
    /// Ignored once the contact is already saying something: a pull in progress owns
    /// the value, and a resolve on top of it would snap the trigger to full out from
    /// under a thumb that is mid-slide.
    /// </summary>
    public void ArmLatchSelection(long nowNanos, long holdResolveNanos)
    {
        // LatchSelecting is the guard, not a note: only the press that WITHHELD a
        // resolve may be given one, so no path can hand a second deadline to a press
        // that already has one running.
        if (!LatchSelecting || PointerId == NoPointer || Dragging || resolvedFull ||
            nowNanos <= 0L)
        {
            return;
        }

        HoldResolveDeadlineNanos = nowNanos + holdResolveNanos;
    }

    /// <summary>
    /// The contact moved. Returns the travel that counts toward committing a hold,
    /// which is the projection and not the raw distance.
    ///
    /// Ownership is unconditional once dragging: leaving the visible trigger, drifting
    /// sideways, or wandering back over a neighbour changes nothing. The control is a
    /// handle on a travel surface half the screen across, so bounds have no part in it.
    /// </summary>
    public float OnMove(TouchContact contact, TouchTriggerConfig config, float slopPx)
    {
        if (PointerId != contact.Id)
        {
            return 0f;
        }

        var dx = contact.X - originX;
        var dy = contact.Y - originY;

        if (!Dragging)
        {
            if ((dx * dx) + (dy * dy) <= slopPx * slopPx)
            {
                return 0f;
            }

            Dragging = true;

            // The press turned out to be a pull, so it is no longer a candidate for
            // either of the two things a still press can become.
            HoldResolveDeadlineNanos = 0L;

            // And the swipe has now declared which way it went. Read here rather than
            // on the first pixel of movement because below the slop the direction is
            // jitter, and read only here because after this the thumb is arcing.
            SwipeFill = TouchTriggerTravel.FillDirection(new TouchVector(dx, dy));
        }

        var value = TouchTriggerTravel.AnalogValue(dx, dy, Axis, FullTravelPx);
        PhysicalValue = value;
        PhysicalDetent = DetentWithHysteresis(value, PhysicalDetent, config);
        return TouchTriggerTravel.ProjectedTravelPx(dx, dy, Axis);
    }

    /// <summary>
    /// The still press outlasted the deliberate-hold base: it is a full pull.
    ///
    /// Returns true when this press was a latch CANDIDATE that has just lost it. The
    /// fallback is a state transition and not merely an output: a press that has been
    /// answered as an ordinary held trigger must not still be able to lock a partial
    /// hold if the finger moves afterwards, because that hold is persistent state
    /// reached through a gesture already resolved as something else.
    /// </summary>
    public bool ResolveFullPull()
    {
        HoldResolveDeadlineNanos = 0L;
        resolvedFull = true;
        PhysicalValue = 1f;
        PhysicalDetent = true;
        var wasSelecting = LatchSelecting;
        LatchSelecting = false;
        return wasSelecting;
    }

    /// <summary>
    /// The latch gesture committed on this contact. Records the level so a cancelled
    /// contact still leaves a sensible hold behind.
    /// </summary>
    public void CommitLatch()
    {
        committedByThisContact = true;
        LatchedValue = PhysicalValue;
        LatchedDetent = PhysicalDetent;
    }

    /// <summary>
    /// A contact ended. Returns true when the release should publish a tap pulse.
    ///
    /// A tap is a press that never became a pull, never resolved into a full one, did
    /// not remove the hold it started on, and was short enough to be a tap at all. The
    /// last clause is what stops a long still press — an abandoned latch attempt, say
    /// — ending in a full trigger click nobody asked for.
    /// </summary>
    /// <param name="latched">Whether a hold is on the control NOW, after whatever this press did.</param>
    public bool OnEnd(
        TouchContact contact,
        bool cancelled,
        bool latched,
        long maxTapDurationNanos,
        long pulseNanos)
    {
        var duration = contact.TimeNanos - pressStartNanos;
        var removedTheHold = StartedLatched && !latched;
        var tapped = !cancelled && !Dragging && !resolvedFull && !removedTheHold &&
            contact.TimeNanos > 0L && pressStartNanos > 0L &&
            duration >= 0 && duration <= maxTapDurationNanos;

        if (committedByThisContact && !cancelled)
        {
            // The value at RELEASE is the held level: the whole point of the slide is
            // that the user is choosing it while they can see it.
            LatchedValue = PhysicalValue;
            LatchedDetent = PhysicalDetent;
        }

        if (!latched)
        {
            LatchedValue = 0f;
            LatchedDetent = false;
        }

        PointerId = NoPointer;
        pressStartNanos = 0L;
        Dragging = false;
        SwipeFill = null;
        resolvedFull = false;
        committedByThisContact = false;
        StartedLatched = false;
        LatchSelecting = false;
        HoldResolveDeadlineNanos = 0L;
        PhysicalValue = 0f;
        PhysicalDetent = false;

        if (!tapped)
        {
            return false;
        }

        // A hold already at full travel cannot be re-fired by pulling harder, so the
        // pulse becomes a RELEASE edge instead, exactly as the digital retrigger mask
        // does. Either way the console sees an edge.
        PulseValue = latched && LatchedDetent ? 0f : 1f;
        PulseDeadlineNanos = contact.TimeNanos + pulseNanos;
        return true;
    }

    /// <summary>The pulse window expired.</summary>
    public void EndPulse() => PulseDeadlineNanos = 0L;

    /// <summary>The hold was removed by any path; the level it carried goes with it.</summary>
    public void ClearLatch()
    {
        LatchedValue = 0f;
        LatchedDetent = false;
        committedByThisContact = false;
    }

    /// <summary>The earliest timed transition this trigger is waiting for, if any.</summary>
    public long? NextDeadlineNanos()
    {
        if (HoldResolveDeadlineNanos == 0L)
        {
            return PulseDeadlineNanos == 0L ? null : PulseDeadlineNanos;
        }

        return PulseDeadlineNanos == 0L
            ? HoldResolveDeadlineNanos
            : Math.Min(HoldResolveDeadlineNanos, PulseDeadlineNanos);
    }

    /// <summary>
    /// What this trigger publishes right now, given whether a latch holds it.
    ///
    /// Priority is pulse, then finger, then hold. A finger temporarily overriding a
    /// hold is the point of allowing both: a trigger held at 55% is still a trigger,
    /// and pulling it to 85% has to reach the console while the thumb is down and go
    /// back to 55% when it lifts.
    /// </summary>
    public float EffectiveValue(bool latched)
    {
        if (Pulsing)
        {
            return PulseValue;
        }

        if (PhysicalActive)
        {
            return PhysicalValue;
        }

        return latched ? LatchedValue : 0f;
    }

    public bool EffectiveDetent(bool latched, TouchTriggerConfig config)
    {
        if (Pulsing)
        {
            return PulseValue >= config.DetentEngageFraction;
        }

        if (PhysicalActive)
        {
            return PhysicalDetent;
        }

        return latched && LatchedDetent;
    }
}
