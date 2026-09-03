using PicoSwitch.Bridge.Touch;

namespace PicoSwitch.Bridge.Core;

/// <summary>
/// The platform-neutral controller state machine.
///
/// A platform backend translates its own events into calls on this object; this
/// object owns everything that is the same on every platform:
///
/// - which buttons are held, and from which independent origin,
/// - merging D-pad keys with a D-pad/hat axis pair,
/// - applying the face-layout mapping,
/// - publishing one complete <see cref="ControllerState"/> snapshot per input event,
/// - neutralizing on every boundary (source change, layout change, teardown).
///
/// ## Why buttons are held AS REPORTED
///
/// <see cref="PressButton"/> takes the button exactly as the platform named it,
/// BEFORE the face layout is applied, and the layout is applied at publish time.
/// That is what makes a layout change safe while keys are held: the same physical
/// key resolves to its new logical meaning instead of a stale one being stuck
/// down. (The state is neutralized on a layout change anyway — belt and braces,
/// because a stuck button on a console is one of the worst failure modes this
/// bridge has.)
///
/// ## Why virtual buttons are a separate set
///
/// A physical key and an on-screen press are independent origins for the same
/// logical button; releasing one must not cancel the other. Virtual buttons are
/// also NOT gated on a selected input source: they belong to the host device
/// itself and stay usable when the host has no gamepad to select. Most host
/// devices have no Home, Capture or C/GameChat key, so for those three the
/// virtual path is the only path.
///
/// ## The three gameplay origins
///
/// <code>
/// physical controls   ---\
///                         >--  [authority] picks ONE  --\
/// on-screen controller ---/                              >-- published state
/// software/meta buttons  ------- always contribute ------/
/// </code>
///
/// <see cref="Authority"/> is the explicit answer to "which host control set is
/// the controller right now". A merge would be indefensible — a physical stick
/// left and a touch stick right have no combined meaning — so the inactive
/// origin's mutations are discarded rather than retained, and switching authority
/// neutralizes. See <see cref="InputAuthority"/>.
///
/// Not thread-safe by design; drive it from one input thread, exactly as every
/// platform delivers input events.
/// </summary>
public sealed class ControllerInputState
{
    private readonly StateValue<ControllerState> state = new(ControllerState.Neutral);

    private ControllerButtonSet heldPhysicalButtons = ControllerButtonSet.Empty;
    private ControllerButtonSet virtualButtons = ControllerButtonSet.Empty;
    private DpadState keyDpad = DpadState.None;
    private DpadState hatDpad = DpadState.None;
    private AnalogFrame physicalAnalog = AnalogFrame.Neutral;
    private TouchContribution touch = TouchContribution.Neutral;

    public IReadOnlyStateValue<ControllerState> State => state;

    public ControllerSourceIdentity? Source { get; private set; }

    public ControllerFaceLayout RequestedLayout { get; private set; } = ControllerFaceLayout.Auto;

    public ResolvedControllerLayout ResolvedLayout { get; private set; } =
        ControllerLayoutResolver.Resolve(ControllerFaceLayout.Auto, null);

    /// <summary>Which host control set drives gameplay input. See <see cref="InputAuthority"/>.</summary>
    public InputAuthority Authority { get; private set; } = InputAuthority.Physical;

    /// <summary>
    /// The on-screen controller's current contribution, whether or not it is
    /// currently authoritative.
    ///
    /// Exposed so a diagnostic can distinguish "touch is holding nothing" from
    /// "touch is holding something the authority is discarding" — two states that
    /// look identical in the published <see cref="State"/> and mean completely
    /// different things when a control appears not to work.
    /// </summary>
    public TouchContribution TouchContribution => touch;

    /// <summary>
    /// Point the state machine at a new input source, or at none.
    ///
    /// Re-resolves the face layout for the new identity and neutralizes, because
    /// nothing held on the previous source has any meaning on this one.
    /// </summary>
    public void SetSource(ControllerSourceIdentity? identity)
    {
        Source = identity;
        ResolvedLayout = ControllerLayoutResolver.Resolve(RequestedLayout, identity);
        Neutralize();
    }

    /// <summary>Apply the user's layout preference. Held input is cleared; see the class doc.</summary>
    public void SetRequestedLayout(ControllerFaceLayout layout)
    {
        RequestedLayout = layout;
        ResolvedLayout = ControllerLayoutResolver.Resolve(layout, Source);
        Neutralize();
    }

    /// <summary>
    /// Hand gameplay input to a different host control set.
    ///
    /// Always neutralizes, even when nothing appears to be held: a control that
    /// was down at the moment of the switch belongs to the origin being left, and
    /// carrying it across the boundary is exactly how a console ends up walking
    /// into a wall after the user opened a different screen.
    /// </summary>
    public void SetAuthority(InputAuthority next)
    {
        if (next == Authority)
        {
            return;
        }

        Authority = next;
        Neutralize();
    }

    /// <summary>
    /// A physical button, named exactly as the source device reported it.
    ///
    /// Face keys are still in the source's own dialect here — positional on an
    /// Xbox-style pad, printed-legend on a Nintendo-labelled handheld — and are
    /// translated by <see cref="ControllerLayoutResolver.MapPhysicalFaceKey"/> at
    /// publish time.
    /// </summary>
    public void PressButton(ControllerButton reported, bool pressed)
    {
        if (Authority != InputAuthority.Physical)
        {
            return;
        }

        heldPhysicalButtons = heldPhysicalButtons.With(reported, pressed);
        Publish();
    }

    /// <summary>An on-screen / software button, already in logical bridge semantics.</summary>
    public void SetVirtualButton(ControllerButton button, bool pressed)
    {
        virtualButtons = virtualButtons.With(button, pressed);
        Publish();
    }

    /// <summary>A discrete D-pad key. Merged with any hat axes; opposites cancel at encode time.</summary>
    public void PressDpad(bool? up = null, bool? right = null, bool? down = null, bool? left = null)
    {
        if (Authority != InputAuthority.Physical)
        {
            return;
        }

        keyDpad = new DpadState(
            Up: up ?? keyDpad.Up,
            Right: right ?? keyDpad.Right,
            Down: down ?? keyDpad.Down,
            Left: left ?? keyDpad.Left);
        Publish();
    }

    /// <summary>
    /// One complete analog event: both sticks, both triggers, and the hat.
    ///
    /// Applied as a single state change so one physical event produces one
    /// observable snapshot rather than three.
    /// </summary>
    public void ApplyAnalog(AnalogFrame frame)
    {
        if (Authority != InputAuthority.Physical)
        {
            return;
        }

        if (frame.Dpad is { } dpad)
        {
            hatDpad = dpad;
        }

        physicalAnalog = frame;
        Publish();
    }

    /// <summary>
    /// Replace the complete physical-controller contribution in one publish.
    /// Host polling APIs return one coherent snapshot;
    /// splitting that snapshot into a button call per flag plus an analog call
    /// would expose transient half-frames to the 125 Hz report scheduler.
    /// </summary>
    public void ApplyPhysicalFrame(ControllerButtonSet reportedButtons, AnalogFrame frame)
    {
        if (Authority != InputAuthority.Physical)
        {
            return;
        }

        heldPhysicalButtons = reportedButtons;
        keyDpad = DpadState.None;
        hatDpad = frame.Dpad ?? DpadState.None;
        physicalAnalog = frame;
        Publish();
    }

    /// <summary>
    /// One complete on-screen controller event: both sticks, both triggers, the
    /// D-pad and every held button.
    ///
    /// Whole rather than incremental for the same reason as
    /// <see cref="ApplyAnalog"/>: one contact event can change several controls at
    /// once, and a half-applied snapshot must never be observable.
    /// </summary>
    public void ApplyTouch(TouchContribution contribution)
    {
        if (Authority != InputAuthority.Touch)
        {
            return;
        }

        touch = contribution;
        Publish();
    }

    /// <summary>
    /// Drop every held input and publish neutral.
    ///
    /// Called on source change, layout change, authority change, link loss and
    /// teardown. A held input that outlives its own boundary reaches the console
    /// as a stuck button.
    /// </summary>
    public void Neutralize()
    {
        heldPhysicalButtons = ControllerButtonSet.Empty;
        virtualButtons = ControllerButtonSet.Empty;
        keyDpad = DpadState.None;
        hatDpad = DpadState.None;
        physicalAnalog = AnalogFrame.Neutral;
        touch = TouchContribution.Neutral;
        state.Set(ControllerState.Neutral);
    }

    /// <summary>
    /// Compose the published snapshot from the currently authoritative gameplay
    /// origin plus the always-allowed software/meta buttons.
    ///
    /// One method rather than one per input kind: every mutator ends here, so a
    /// new origin cannot accidentally publish a state that omits another origin's
    /// contribution. <see cref="StateValue{T}"/> drops an unchanged value, so
    /// recomposing the whole snapshot costs no extra notification.
    /// </summary>
    private void Publish()
    {
        var logical = virtualButtons;

        AnalogFrame analog;
        DpadState dpad;

        // Each origin brings its face buttons in its own dialect and gets the
        // mapper for that dialect. They are NOT interchangeable: a physical key
        // needs the source device's legend corrected, an on-screen slot needs the
        // drawn presentation honoured, and those are opposite under the same
        // layout. See the ControllerLayout.cs header.
        switch (Authority)
        {
            case InputAuthority.Physical:
                foreach (var button in heldPhysicalButtons.Values)
                {
                    logical = logical.With(
                        ControllerLayoutResolver.MapPhysicalFaceKey(button, ResolvedLayout.Layout));
                }

                analog = physicalAnalog;
                dpad = new DpadState(
                    Up: keyDpad.Up || hatDpad.Up,
                    Right: keyDpad.Right || hatDpad.Right,
                    Down: keyDpad.Down || hatDpad.Down,
                    Left: keyDpad.Left || hatDpad.Left);
                break;

            case InputAuthority.Touch:
            default:
                logical = logical.Union(touch.LogicalButtons);
                foreach (var button in touch.PositionalButtons.Values)
                {
                    // The on-screen pad's own presentation, NOT ResolvedLayout.
                    //
                    // ResolvedLayout describes the printed legend on somebody's
                    // PHYSICAL controller, so that a positional pad reporting "A"
                    // for its bottom button lands on the console's bottom button.
                    // This surface has no printed legend to describe: the app
                    // draws its letters, from TouchControlNaming.FaceLayout, and
                    // FaceLabel is derived from this very call. Passing anything
                    // else here makes the drawn letter and the transmitted button
                    // disagree — observed as pressing A and getting B — and the
                    // player has no way to tell which one is lying.
                    //
                    // It matters more now than when this was written: with a
                    // freeform layout editor a control's LABEL is its binding,
                    // because the user put it wherever they liked and there is no
                    // meaningful "position" left to reinterpret.
                    logical = logical.With(
                        ControllerLayoutResolver.MapTouchFacePosition(
                            button, TouchControlNaming.FaceLayout));
                }

                analog = new AnalogFrame(
                    LeftX: touch.LeftX,
                    LeftY: touch.LeftY,
                    RightX: touch.RightX,
                    RightY: touch.RightY,
                    LeftTrigger: touch.LeftTrigger,
                    RightTrigger: touch.RightTrigger);
                dpad = touch.Dpad;
                break;
        }

        state.Set(new ControllerState
        {
            LeftX = analog.LeftX,
            LeftY = analog.LeftY,
            RightX = analog.RightX,
            RightY = analog.RightY,
            LeftTrigger = analog.LeftTrigger,
            RightTrigger = analog.RightTrigger,
            Buttons = logical,
            DpadUp = dpad.Up,
            DpadRight = dpad.Right,
            DpadDown = dpad.Down,
            DpadLeft = dpad.Left,
        });
    }
}
