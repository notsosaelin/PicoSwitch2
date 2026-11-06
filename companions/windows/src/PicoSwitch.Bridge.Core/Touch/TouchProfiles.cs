using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Bridge.Touch;

/// <summary>Console-facing controller identities for which a touch layout is shipped.</summary>
public enum TouchProfileId
{
    Pro2,
    GameCube,
    JoyConLeft,
    JoyConRight,
}

public static class TouchProfileIds
{
    public static string Key(this TouchProfileId value) => value switch
    {
        TouchProfileId.Pro2 => "pro2",
        TouchProfileId.GameCube => "gc",
        TouchProfileId.JoyConLeft => "jcl",
        _ => "jcr",
    };

    public static TouchProfileId? FromKey(string? value) =>
        Enum.GetValues<TouchProfileId>()
            .Cast<TouchProfileId?>()
            .FirstOrDefault(id => id!.Value.Key() == value);
}

/// <summary>
/// What a control visibly represents on the selected console-facing controller.
///
/// Deliberately distinct from <see cref="TouchControlAction"/>. A sideways Joy-Con
/// direction button, for example, is produced through a generic face-button bridge
/// usage, while a GameCube Z control is produced through the generic R shoulder
/// usage. The profile owns that fixed binding; user state never does.
/// </summary>
public enum TouchOutputControl
{
    Unspecified,
    FaceSouth,
    FaceEast,
    FaceWest,
    FaceNorth,
    A,
    B,
    X,
    Y,
    DirectionUp,
    DirectionLeft,
    DirectionRight,
    DirectionDown,
    Dpad,
    PrimaryStick,
    SecondaryStick,
    PrimaryStickClick,
    SecondaryStickClick,
    L,
    R,
    ZL,
    ZR,
    Z,
    SL,
    SR,
    Minus,
    Plus,
    Home,
    Capture,
    C,

    /// <summary>
    /// Pro Controller 2 grip buttons.
    ///
    /// Real inputs on that controller, carried to the adapter as bridge usages
    /// 16/17 since contract 4. Deliberately NOT offered by any personality whose
    /// hardware has no grips.
    /// </summary>
    GL,
    GR,
}

/// <summary>Geometry authored in the reference layout's logical coordinate system.</summary>
public sealed record TouchControlGeometry
{
    public required float AnchorX { get; init; }

    public required float AnchorY { get; init; }

    public required float WidthUnits { get; init; }

    public required float HeightUnits { get; init; }

    public TouchControlShape Shape { get; init; } = TouchControlShape.Circle;

    public float HitMarginUnits { get; init; }

    public int Priority { get; init; }

    /// <summary>Logical-unit offset from a shared group anchor; immune to aspect-ratio distortion.</summary>
    public float GroupOffsetXUnits { get; init; }

    public float GroupOffsetYUnits { get; init; }
}

/// <summary>Portable visual intent. A host renderer supplies the actual paths.</summary>
public sealed record TouchVisualSpec(
    TouchVisualRole Role,
    string Label = "",
    TouchControlGlyph? Glyph = null,
    /// <summary>Clockwise rotation in screen coordinates; immutable template art direction.</summary>
    float RotationDegrees = 0f);

/// <summary>
/// Where a catalog entry appears in the editor's Add Control picker.
///
/// Presentation grouping only. It never affects bindings, geometry or audit, and
/// two personalities are free to file the same output differently — a GameCube
/// <c>Z</c> is a shoulder, a Joy-Con <c>SL</c> is a rail control.
/// </summary>
public enum TouchControlCategory
{
    Face,
    Directions,
    Sticks,
    Shoulders,
    System,
    Grip,
}

public static class TouchControlCategories
{
    public static string Title(this TouchControlCategory value) => value switch
    {
        TouchControlCategory.Face => "Face buttons",
        TouchControlCategory.Directions => "D-pad",
        TouchControlCategory.Sticks => "Sticks",
        TouchControlCategory.Shoulders => "Shoulders and triggers",
        TouchControlCategory.Grip => "Grip",
        _ => "System",
    };
}

/// <summary>
/// One entry in a personality's control CATALOG.
///
/// The catalog answers "what may this controller produce, and what does such a
/// control look like?". It is emphatically NOT the layout: a user document holds
/// instances, any number of which may point at the same entry.
///
/// <code>
///   the profile BINDS it            -> the personality has the hardware
///   InDefaultLayout                 -> the SHIPPED layout instantiates one
///   a user document instantiates it -> it is on screen, once per instance
/// </code>
///
/// Those three are independent. In particular <see cref="InDefaultLayout"/> is a
/// statement about the authored starting point and nothing else: a control absent
/// from the default layout is still fully addable, and one present there may be
/// deleted.
/// </summary>
public sealed record TouchTemplateControl
{
    public required string Id { get; init; }

    public required TouchOutputControl Output { get; init; }

    public required TouchControlKind Interaction { get; init; }

    public required TouchControlGeometry Geometry { get; init; }

    public required TouchVisualSpec Visual { get; init; }

    /// <summary>The authored cluster this control belongs to in the shipped layout.</summary>
    public string? EditGroupId { get; init; }

    /// <summary>
    /// Whether the SHIPPED layout places an instance of this control.
    ///
    /// <c>false</c> means the personality supports the control and the authored
    /// default simply does not use it — Pro Controller 2's grips are the shipped
    /// example. It does not make the control second-class: Add Control offers it
    /// from this catalog like any other.
    /// </summary>
    public bool InDefaultLayout { get; init; } = true;

    public TouchControlCategory Category { get; init; } = TouchControlCategory.System;
}

/// <summary>
/// A personality's shipped control catalog and its authored default layout.
///
/// One value carries both because they are authored together, but the code must
/// keep them apart: <see cref="Controls"/> is the catalog (everything
/// instantiable) and the subset marked
/// <see cref="TouchTemplateControl.InDefaultLayout"/> is what a fresh layout
/// document starts with.
/// </summary>
public sealed record TouchLayoutTemplate(
    string Id,
    TouchProfileId ProfileId,
    int SchemaVersion,
    int TemplateRevision,
    IReadOnlyList<TouchTemplateControl> Controls);

public sealed class TouchControllerProfile
{
    private readonly Dictionary<string, TouchTemplateControl> catalogById;

    public TouchControllerProfile(
        TouchProfileId id,
        string displayName,
        TouchLayoutTemplate defaultTemplate,
        IReadOnlyDictionary<TouchOutputControl, TouchControlAction> bindings)
    {
        if (defaultTemplate.ProfileId != id)
        {
            throw new ArgumentException(
                $"Template {defaultTemplate.Id} belongs to {defaultTemplate.ProfileId}, not {id}");
        }

        Id = id;
        DisplayName = displayName;
        DefaultTemplate = defaultTemplate;
        Bindings = bindings;
        Outputs = bindings.Keys.ToHashSet();

        catalogById = defaultTemplate.Controls.ToDictionary(
            control => control.Id, StringComparer.Ordinal);
    }

    public TouchProfileId Id { get; }

    public string DisplayName { get; }

    public TouchLayoutTemplate DefaultTemplate { get; }

    public IReadOnlySet<TouchOutputControl> Outputs { get; }

    public IReadOnlyDictionary<TouchOutputControl, TouchControlAction> Bindings { get; }

    /// <summary>Everything this personality can instantiate, in authored order.</summary>
    public IReadOnlyList<TouchTemplateControl> Catalog => DefaultTemplate.Controls;

    public TouchTemplateControl? CatalogEntry(string id) =>
        catalogById.TryGetValue(id, out var entry) ? entry : null;
}

/// <summary>The complete, exhaustive shipped touch-profile catalog.</summary>
public static class TouchProfileCatalog
{
    public static IReadOnlyDictionary<TouchProfileId, TouchControllerProfile> Profiles { get; } =
        new[] { Pro2(), GameCube(), JoyConLeft(), JoyConRight() }
            .ToDictionary(profile => profile.Id);

    public static TouchControllerProfile? Profile(TouchProfileId id) =>
        Profiles.TryGetValue(id, out var profile) ? profile : null;

    public static TouchControllerProfile Require(TouchProfileId id) =>
        Profile(id) ?? throw new ArgumentException($"No touch profile for {id}");

    private static TouchControllerProfile Pro2()
    {
        var bindings = new Dictionary<TouchOutputControl, TouchControlAction>
        {
            [TouchOutputControl.FaceSouth] = new TouchControlAction.Face(FaceButtonPosition.South),
            [TouchOutputControl.FaceEast] = new TouchControlAction.Face(FaceButtonPosition.East),
            [TouchOutputControl.FaceWest] = new TouchControlAction.Face(FaceButtonPosition.West),
            [TouchOutputControl.FaceNorth] = new TouchControlAction.Face(FaceButtonPosition.North),
            [TouchOutputControl.Dpad] = TouchControlAction.Directions.Instance,
            [TouchOutputControl.PrimaryStick] = new TouchControlAction.Stick(ControlSide.Left),
            [TouchOutputControl.SecondaryStick] = new TouchControlAction.Stick(ControlSide.Right),
            [TouchOutputControl.PrimaryStickClick] =
                new TouchControlAction.Logical(ControllerButton.LeftStick),
            [TouchOutputControl.SecondaryStickClick] =
                new TouchControlAction.Logical(ControllerButton.RightStick),
            [TouchOutputControl.L] = new TouchControlAction.Logical(ControllerButton.L1),
            [TouchOutputControl.R] = new TouchControlAction.Logical(ControllerButton.R1),
            [TouchOutputControl.ZL] = new TouchControlAction.Trigger(ControlSide.Left),
            [TouchOutputControl.ZR] = new TouchControlAction.Trigger(ControlSide.Right),
            [TouchOutputControl.Minus] = new TouchControlAction.Logical(ControllerButton.Select),
            [TouchOutputControl.Plus] = new TouchControlAction.Logical(ControllerButton.Start),
            [TouchOutputControl.Home] = new TouchControlAction.Logical(ControllerButton.Home),
            [TouchOutputControl.Capture] = new TouchControlAction.Logical(ControllerButton.Capture),
            [TouchOutputControl.C] = new TouchControlAction.Logical(ControllerButton.C),

            // The grip buttons. Bound like any other digital control, straight to
            // the logical inputs the firmware already routes to SWITCH_EXTRA_GL/GR
            // — no alias, no synthetic action. Shipped absent from the default
            // LAYOUT but present in the CATALOG, so Add Control can offer them.
            [TouchOutputControl.GL] = new TouchControlAction.Logical(ControllerButton.GL),
            [TouchOutputControl.GR] = new TouchControlAction.Logical(ControllerButton.GR),
        };

        return new TouchControllerProfile(
            TouchProfileId.Pro2, "Pro Controller 2", TouchLayoutV1.Template, bindings);
    }

    private static TouchControllerProfile GameCube()
    {
        var bindings = new Dictionary<TouchOutputControl, TouchControlAction>
        {
            [TouchOutputControl.A] = new TouchControlAction.Logical(ControllerButton.A),
            [TouchOutputControl.B] = new TouchControlAction.Logical(ControllerButton.B),
            [TouchOutputControl.X] = new TouchControlAction.Logical(ControllerButton.X),
            [TouchOutputControl.Y] = new TouchControlAction.Logical(ControllerButton.Y),
            [TouchOutputControl.Dpad] = TouchControlAction.Directions.Instance,
            [TouchOutputControl.PrimaryStick] = new TouchControlAction.Stick(ControlSide.Left),
            [TouchOutputControl.SecondaryStick] = new TouchControlAction.Stick(ControlSide.Right),

            // The firmware's GameCube policy maps generic L/R shoulders to ZL/Z.
            //
            // `Z` is the control the GameCube shell PRINTS as Z; the host-facing
            // semantic is ZR — a Switch 2's Test Input screen names it that, and so
            // does Windows/Steam. The legend stays the shell's, because nothing on
            // the device the user is holding says ZR.
            [TouchOutputControl.ZL] = new TouchControlAction.Logical(ControllerButton.L1),
            [TouchOutputControl.Z] = new TouchControlAction.Logical(ControllerButton.R1),

            // The only two touch controls in the catalog with REAL travel: the
            // GameCube encoder passes the trigger byte through continuously and the
            // firmware seam derives the terminal detent from it. Everything else
            // that looks like a trigger is digital on the far side.
            [TouchOutputControl.L] = new TouchControlAction.Trigger(ControlSide.Left, Analog: true),
            [TouchOutputControl.R] = new TouchControlAction.Trigger(ControlSide.Right, Analog: true),
            [TouchOutputControl.Plus] = new TouchControlAction.Logical(ControllerButton.Start),
            [TouchOutputControl.Home] = new TouchControlAction.Logical(ControllerButton.Home),
            [TouchOutputControl.Capture] = new TouchControlAction.Logical(ControllerButton.Capture),
            [TouchOutputControl.C] = new TouchControlAction.Logical(ControllerButton.C),
        };

        return new TouchControllerProfile(
            TouchProfileId.GameCube, "NSO GameCube",
            TouchPersonalityTemplates.GameCube, bindings);
    }

    private static TouchControllerProfile JoyConLeft()
    {
        var bindings = new Dictionary<TouchOutputControl, TouchControlAction>
        {
            [TouchOutputControl.PrimaryStick] = new TouchControlAction.Stick(ControlSide.Left),
            [TouchOutputControl.PrimaryStickClick] =
                new TouchControlAction.Logical(ControllerButton.LeftStick),
            [TouchOutputControl.DirectionUp] = new TouchControlAction.Logical(ControllerButton.X),
            [TouchOutputControl.DirectionLeft] = new TouchControlAction.Logical(ControllerButton.A),
            [TouchOutputControl.DirectionRight] = new TouchControlAction.Logical(ControllerButton.Y),
            [TouchOutputControl.DirectionDown] = new TouchControlAction.Logical(ControllerButton.B),
            [TouchOutputControl.SL] = new TouchControlAction.Logical(ControllerButton.L1),
            [TouchOutputControl.SR] = new TouchControlAction.Logical(ControllerButton.R1),
            [TouchOutputControl.L] = new TouchControlAction.Trigger(ControlSide.Left),
            [TouchOutputControl.ZL] = new TouchControlAction.Trigger(ControlSide.Right),
            [TouchOutputControl.Minus] = new TouchControlAction.Logical(ControllerButton.Select),
            [TouchOutputControl.Capture] = new TouchControlAction.Logical(ControllerButton.Capture),
        };

        return new TouchControllerProfile(
            TouchProfileId.JoyConLeft, "Joy-Con 2 (L), sideways",
            TouchPersonalityTemplates.JoyConLeft, bindings);
    }

    private static TouchControllerProfile JoyConRight()
    {
        var bindings = new Dictionary<TouchOutputControl, TouchControlAction>
        {
            [TouchOutputControl.PrimaryStick] = new TouchControlAction.Stick(ControlSide.Left),
            [TouchOutputControl.PrimaryStickClick] =
                new TouchControlAction.Logical(ControllerButton.LeftStick),
            [TouchOutputControl.A] = new TouchControlAction.Logical(ControllerButton.A),
            [TouchOutputControl.B] = new TouchControlAction.Logical(ControllerButton.X),
            [TouchOutputControl.X] = new TouchControlAction.Logical(ControllerButton.B),
            [TouchOutputControl.Y] = new TouchControlAction.Logical(ControllerButton.Y),
            [TouchOutputControl.SL] = new TouchControlAction.Logical(ControllerButton.L1),
            [TouchOutputControl.SR] = new TouchControlAction.Logical(ControllerButton.R1),
            [TouchOutputControl.R] = new TouchControlAction.Trigger(ControlSide.Left),
            [TouchOutputControl.ZR] = new TouchControlAction.Trigger(ControlSide.Right),
            [TouchOutputControl.Plus] = new TouchControlAction.Logical(ControllerButton.Start),
            [TouchOutputControl.Home] = new TouchControlAction.Logical(ControllerButton.Home),
            [TouchOutputControl.C] = new TouchControlAction.Logical(ControllerButton.C),
        };

        return new TouchControllerProfile(
            TouchProfileId.JoyConRight, "Joy-Con 2 (R), sideways",
            TouchPersonalityTemplates.JoyConRight, bindings);
    }
}
