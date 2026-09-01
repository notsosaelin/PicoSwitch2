namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// Cardinal slots shared by face and independent-direction button diamonds.
///
/// These name a position on the PHYSICAL controller being represented — the top
/// button of its diamond is <see cref="North"/> whether or not that button ends up
/// at the top of the screen. Where the two differ,
/// <see cref="TouchClusterRotation"/> is what maps one to the other; nothing
/// should read a slot as a screen position directly.
/// </summary>
public enum TouchCardinalSlot
{
    North,
    East,
    South,
    West,
}

/// <summary>
/// How a physical control cluster is turned before it is drawn.
///
/// A single Joy-Con used sideways is a whole controller rotated a quarter turn,
/// and the four buttons on its face go with it. Writing their screen positions out
/// by hand is how the layout ends up lying: the control named <c>direction-up</c>
/// gets placed at the top of the screen because the name says "up", when the button
/// it represents is physically pointing at the player's LEFT once the shell is
/// turned. The console reads the raw direction bits and applies its own sideways
/// interpretation, so the on-screen arrangement is the only thing that can be
/// wrong — and it was.
///
/// Stating the rotation once keeps the three things the layout has to get right
/// separable and separately checkable:
///
/// <code>
/// physical identity   the button on the shell   TouchCardinalSlot / TouchOutputControl
/// logical action      what it sends             TouchControllerProfile.Bindings
/// screen position     where it is drawn         this
/// </code>
///
/// The direction matches the firmware's own statement of how each half is held —
/// <c>joycon2_pack_sideways_stick</c> rotates the left half's stick axes one way
/// and the right half's the other — so the touch layout and the report encoder
/// cannot disagree about which way a shell is turned.
/// </summary>
public enum TouchClusterRotation
{
    /// <summary>Drawn as held: the shell's top is the screen's top.</summary>
    Upright,

    /// <summary>Joy-Con (R) sideways: the rail edge, and with it SL/SR, comes to the top.</summary>
    QuarterClockwise,

    /// <summary>Joy-Con (L) sideways: the rail edge, and with it SL/SR, comes to the top.</summary>
    QuarterCounterClockwise,
}

public static class TouchClusterRotations
{
    /// <summary>Clockwise screen-space rotation, matching <c>TouchControlSpec.VisualRotationDegrees</c>.</summary>
    public static float Degrees(this TouchClusterRotation value) => value switch
    {
        TouchClusterRotation.QuarterClockwise => 90f,
        TouchClusterRotation.QuarterCounterClockwise => -90f,
        _ => 0f,
    };

    /// <summary>
    /// Where a control that is physically at <paramref name="physical"/> appears on
    /// screen.
    ///
    /// Turn a clock face a quarter turn anticlockwise and 12 lands where 9 was;
    /// that is the whole of it.
    /// </summary>
    public static TouchCardinalSlot ScreenSlot(
        this TouchClusterRotation rotation, TouchCardinalSlot physical) => rotation switch
    {
        TouchClusterRotation.QuarterClockwise => physical switch
        {
            TouchCardinalSlot.North => TouchCardinalSlot.East,
            TouchCardinalSlot.East => TouchCardinalSlot.South,
            TouchCardinalSlot.South => TouchCardinalSlot.West,
            _ => TouchCardinalSlot.North,
        },
        TouchClusterRotation.QuarterCounterClockwise => physical switch
        {
            TouchCardinalSlot.North => TouchCardinalSlot.West,
            TouchCardinalSlot.West => TouchCardinalSlot.South,
            TouchCardinalSlot.South => TouchCardinalSlot.East,
            _ => TouchCardinalSlot.North,
        },
        _ => physical,
    };
}

/// <summary>
/// One control's placement relative to a group anchor.
///
/// The anchor follows the available interaction rectangle while the offset stays in
/// logical units. That distinction is what keeps a square diamond square on screens
/// whose aspect ratio differs from the 800 x 400 authoring reference.
/// </summary>
public readonly record struct TouchGroupPlacement(
    float AnchorX,
    float AnchorY,
    float OffsetXUnits,
    float OffsetYUnits);

/// <summary>Defined geometry for a related control cluster, never four unrelated points.</summary>
public readonly record struct TouchGroupGeometry
{
    public TouchGroupGeometry(float centerXUnits, float centerYUnits)
    {
        if (!float.IsFinite(centerXUnits) || !float.IsFinite(centerYUnits))
        {
            throw new ArgumentException("A group centre must be finite");
        }

        CenterXUnits = centerXUnits;
        CenterYUnits = centerYUnits;
    }

    public float CenterXUnits { get; }

    public float CenterYUnits { get; }

    public float AnchorX => CenterXUnits / TouchLayoutResolver.ReferenceWidthUnits;

    public float AnchorY => CenterYUnits / TouchLayoutResolver.ReferenceHeightUnits;

    public TouchGroupPlacement At(float offsetXUnits, float offsetYUnits) =>
        new(AnchorX, AnchorY, offsetXUnits, offsetYUnits);

    /// <summary>
    /// A four-button diamond, keyed by the slot each control occupies on the
    /// PHYSICAL controller and placed where <paramref name="rotation"/> puts that
    /// slot on screen.
    ///
    /// The key stays physical on purpose. A template that looked up "the button I
    /// want at the top of the screen" would have to re-derive the rotation at every
    /// call site and would silently drift out of step with the report mapping;
    /// asking for the shell's own north button and letting the rotation decide where
    /// it lands cannot.
    /// </summary>
    public IReadOnlyDictionary<TouchCardinalSlot, TouchGroupPlacement> SquareDiamond(
        float radiusUnits, TouchClusterRotation rotation = TouchClusterRotation.Upright)
    {
        if (radiusUnits <= 0f || !float.IsFinite(radiusUnits))
        {
            throw new ArgumentOutOfRangeException(nameof(radiusUnits));
        }

        var screen = new Dictionary<TouchCardinalSlot, TouchGroupPlacement>
        {
            [TouchCardinalSlot.North] = At(0f, -radiusUnits),
            [TouchCardinalSlot.East] = At(radiusUnits, 0f),
            [TouchCardinalSlot.South] = At(0f, radiusUnits),
            [TouchCardinalSlot.West] = At(-radiusUnits, 0f),
        };

        return Enum.GetValues<TouchCardinalSlot>()
            .ToDictionary(physical => physical, physical => screen[rotation.ScreenSlot(physical)]);
    }
}
