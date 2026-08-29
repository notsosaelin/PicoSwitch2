namespace PicoSwitch.Bridge.Core;

/// <summary>
/// Which host-local control set is currently driving gameplay input.
///
/// This is HOST INPUT AUTHORITY, and it is deliberately not either of the other
/// two "source" ideas this product has:
///
/// - the adapter's active input — which controller the adapter forwards to the
///   console — is a management concept and is untouched by this;
/// - the selected host input device — which physical pad feeds this client — is a
///   platform concept and keeps its own persistence.
///
/// Authority answers only "are the host's physical controls or its on-screen
/// controls the gameplay controller right now". Both can exist at once on a
/// handheld PC, and merging them is not a feature: a physical stick pushed left
/// and a touch stick pushed right have no defensible combined meaning, and
/// whichever event happened last would win by accident. So exactly one origin
/// contributes gameplay state, chosen explicitly.
///
/// Software/meta buttons (<see cref="ControllerInputState.SetVirtualButton"/>)
/// are outside this rule on purpose — Home, Capture and C/GameChat have no
/// physical key on this hardware, so they are host actions rather than a second
/// controller.
/// </summary>
public enum InputAuthority
{
    Physical,
    Touch,
}

/// <summary>
/// One on-screen controller's complete gameplay contribution, in bridge units.
///
/// Produced whole rather than field by field: a single contact event can move a
/// stick, cross a D-pad sector boundary and release a button at once, and
/// publishing those separately would let a sender observe a half-applied event.
/// This mirrors why <see cref="AnalogFrame"/> exists for physical analog input.
///
/// Axes are already <c>0..255</c> with <c>128</c> neutral and triggers
/// <c>0..255</c> at rest — the conversion from the touch engine's
/// <c>[-1,+1]</c> geometry happens exactly once, in the touch layer, so no
/// renderer or platform adapter can introduce a second, subtly different scaling.
///
/// <c>PositionalButtons</c> holds face-diamond presses in their POSITION, exactly
/// as a physical backend reports them, so the one face-layout resolver decides
/// what they mean. <c>LogicalButtons</c> holds actions that are already logical
/// (shoulders, triggers, stick clicks, <c>-</c>/<c>+</c>, Home, Capture, C) and
/// are never face-swapped.
/// </summary>
// A record CLASS, not a record struct: the neutral value of this type is 128 on
// the sticks, and `default(T)` on a struct is unavoidably all zeros — which is
// both sticks pinned to their top-left corner. A type whose zero value is a live
// input is a trap, so the neutral value is a real instance.
public sealed record TouchContribution
{
    public static readonly TouchContribution Neutral = new();

    public int LeftX { get; init; } = 128;

    public int LeftY { get; init; } = 128;

    public int RightX { get; init; } = 128;

    public int RightY { get; init; } = 128;

    public int LeftTrigger { get; init; }

    public int RightTrigger { get; init; }

    public DpadState Dpad { get; init; } = DpadState.None;

    public ControllerButtonSet PositionalButtons { get; init; } = ControllerButtonSet.Empty;

    public ControllerButtonSet LogicalButtons { get; init; } = ControllerButtonSet.Empty;
}
