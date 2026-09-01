using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// The default on-screen controller.
///
/// ## Reading the numbers
///
/// Every coordinate below is written against a reference window of
/// <see cref="TouchLayoutResolver.ReferenceWidthUnits"/> x
/// <see cref="TouchLayoutResolver.ReferenceHeightUnits"/> logical units and then
/// stored normalized, so the file reads as a drawing rather than as a list of
/// ratios. The resolver scales sizes and stretches anchors; see its documentation
/// for why those two are not the same operation.
///
/// ## Why the controls are where they are
///
/// <code>
/// ZL  L       [capture][home][C]     R  ZR
///
///   (D-pad)                          (N)
///                                 (W)   (E)
///              (-)      (+)          (S)
///
///      (left stick)  (L3) (R3) (right stick)
/// </code>
///
/// - The two primary left-thumb controls (D-pad, left stick) and the two primary
///   right-thumb controls (face diamond, right stick) sit at the outer edges,
///   where a thumb holding the device already rests.
/// - Shoulders and triggers occupy the top corners, inside the safe rectangle so
///   they never land in a system gesture strip.
/// - Everything low-frequency — -, +, Home, Capture, C, the stick clicks — lives
///   toward the centre, deliberately away from the territory a thumb sweeps during
///   play. They are reachable without being in the way.
/// - The middle stays quiet. That is not wasted space: it separates the hands,
///   keeps a custom background visible, and leaves somewhere for future per-game
///   controls to go without moving anything muscle memory has learned.
///
/// The stick clicks are their own controls rather than a press gesture on the
/// stick, because a long press on the knob would either delay ordinary movement or
/// fire while aiming.
/// </summary>
public static class TouchLayoutV1
{
    /// <summary>
    /// Persisted schema version.
    ///
    /// Present from the first release even though nothing migrates yet. A stored
    /// layout with no way to say which shape it is leaves a future reader only one
    /// safe option, which is to discard the user's configuration.
    /// </summary>
    public const int SchemaVersion = 1;

    public const string Id = "picoswitch.touch.v1";
    public const int TemplateRevision = 2;

    internal const float LeftPrimaryXUnits = 100f;
    internal const float LeftPrimaryYUnits = 164f;
    internal const float LeftSecondaryXUnits = 216f;
    internal const float LeftSecondaryYUnits = 312f;
    internal const float RightSecondaryXUnits = 584f;
    internal const float RightSecondaryYUnits = 312f;
    internal const float FaceClusterXUnits = 700f;
    internal const float FaceClusterYUnits = 184f;

    private const string FaceGroup = "face-cluster";

    // Control ids. Referenced by the renderer and by tests, so they are constants
    // rather than strings repeated at each site.
    public const string TriggerLeft = "trigger-left";
    public const string TriggerRight = "trigger-right";
    public const string ShoulderLeft = "shoulder-left";
    public const string ShoulderRight = "shoulder-right";
    public const string Capture = "capture";
    public const string Home = "home";
    public const string Chat = "chat";
    public const string Dpad = "dpad";
    public const string FaceNorth = "face-north";
    public const string FaceEast = "face-east";
    public const string FaceSouth = "face-south";
    public const string FaceWest = "face-west";
    public const string Minus = "minus";
    public const string Plus = "plus";
    public const string StickLeft = "stick-left";
    public const string StickRight = "stick-right";
    public const string StickClickLeft = "stick-click-left";
    public const string StickClickRight = "stick-click-right";

    /// <summary>Optional; present in the template, hidden until the user adds them.</summary>
    public const string GripLeft = "grip-left";

    public const string GripRight = "grip-right";

    private static readonly IReadOnlyDictionary<TouchCardinalSlot, TouchGroupPlacement> FaceDiamond =
        new TouchGroupGeometry(FaceClusterXUnits, FaceClusterYUnits).SquareDiamond(60f);

    public static readonly TouchLayout Layout = new(
        Id,
        SchemaVersion,
        [
            // ------------------------------------------------------- top edge
            Shoulder(TriggerLeft, TouchOutputControl.ZL,
                new TouchControlAction.Trigger(ControlSide.Left), 62f, 42f, TouchControlKind.Trigger),
            Shoulder(ShoulderLeft, TouchOutputControl.L,
                new TouchControlAction.Logical(ControllerButton.L1), 166f, 42f),
            Utility(Capture, TouchOutputControl.Capture,
                new TouchControlAction.Logical(ControllerButton.Capture), 330f, TouchControlGlyph.Capture),
            Utility(Home, TouchOutputControl.Home,
                new TouchControlAction.Logical(ControllerButton.Home), 400f, TouchControlGlyph.Home),
            Utility(Chat, TouchOutputControl.C,
                new TouchControlAction.Logical(ControllerButton.C), 470f, label: "C"),
            Shoulder(ShoulderRight, TouchOutputControl.R,
                new TouchControlAction.Logical(ControllerButton.R1), 634f, 42f),
            Shoulder(TriggerRight, TouchOutputControl.ZR,
                new TouchControlAction.Trigger(ControlSide.Right), 738f, 42f, TouchControlKind.Trigger),

            // ------------------------------------------------- primary clusters
            Spec(Dpad, TouchOutputControl.Dpad, TouchControlKind.Dpad,
                TouchControlAction.Directions.Instance,
                LeftSecondaryXUnits, LeftSecondaryYUnits, 146f, 146f,
                visualRole: TouchVisualRole.UnifiedDpad),
            Face(FaceNorth, TouchOutputControl.FaceNorth, FaceButtonPosition.North,
                FaceDiamond[TouchCardinalSlot.North]),
            Face(FaceWest, TouchOutputControl.FaceWest, FaceButtonPosition.West,
                FaceDiamond[TouchCardinalSlot.West]),
            Face(FaceEast, TouchOutputControl.FaceEast, FaceButtonPosition.East,
                FaceDiamond[TouchCardinalSlot.East]),
            Face(FaceSouth, TouchOutputControl.FaceSouth, FaceButtonPosition.South,
                FaceDiamond[TouchCardinalSlot.South]),

            // ------------------------------------------------------- centre band
            Centre(Minus, TouchOutputControl.Minus,
                new TouchControlAction.Logical(ControllerButton.Select), 330f, 244f, "-"),
            Centre(Plus, TouchOutputControl.Plus,
                new TouchControlAction.Logical(ControllerButton.Start), 470f, 244f, "+"),

            // ------------------------------------------------------------ sticks
            Spec(StickLeft, TouchOutputControl.PrimaryStick, TouchControlKind.Stick,
                new TouchControlAction.Stick(ControlSide.Left),
                LeftPrimaryXUnits, LeftPrimaryYUnits, 146f, 146f,
                visualRole: TouchVisualRole.AnalogStick),
            Spec(StickRight, TouchOutputControl.SecondaryStick, TouchControlKind.Stick,
                new TouchControlAction.Stick(ControlSide.Right),
                RightSecondaryXUnits, RightSecondaryYUnits, 146f, 146f,
                visualRole: TouchVisualRole.AnalogStick),
            StickClick(StickClickLeft, TouchOutputControl.PrimaryStickClick,
                ControllerButton.LeftStick, 350f, 332f, "L3"),
            StickClick(StickClickRight, TouchOutputControl.SecondaryStickClick,
                ControllerButton.RightStick, 450f, 332f, "R3"),
        ])
    {
        ProfileId = TouchProfileId.Pro2,
        TemplateId = Id,
        TemplateRevision = TemplateRevision,
    };

    /// <summary>
    /// Controls this personality HAS but the shipped layout does not place.
    ///
    /// Catalog entries, appended to <see cref="Template"/> and never to
    /// <see cref="Layout"/>: the default document instantiates only what the shipped
    /// controller shows, and Add Control offers everything in the catalog. There is
    /// no hidden ghost instance sitting in the layout waiting to be revealed.
    ///
    /// GL/GR are real Pro Controller 2 grip buttons. Their authored position is the
    /// outer BOTTOM corners — below the sticks and outboard of the D-pad and the
    /// face diamond — which is both the closest a flat screen gets to where the
    /// hands already are and the only region wide enough that adding BOTH still
    /// passes the overlap audit at authored size.
    /// </summary>
    private static readonly TouchTemplateControl[] OptionalTemplateControls =
    [
        Grip(GripLeft, TouchOutputControl.GL, 48f, 352f, "GL"),
        Grip(GripRight, TouchOutputControl.GR, 752f, 352f, "GR"),
    ];

    /// <summary>The immutable profile-backed form of the already validated V1 layout.</summary>
    public static readonly TouchLayoutTemplate Template = new(
        Id,
        TouchProfileId.Pro2,
        SchemaVersion,
        TemplateRevision,
        [
            .. Layout.Controls.Select(control => new TouchTemplateControl
            {
                Id = control.Id,
                Output = control.Output,
                Interaction = control.Kind,
                Geometry = new TouchControlGeometry
                {
                    AnchorX = control.AnchorX,
                    AnchorY = control.AnchorY,
                    WidthUnits = control.WidthUnits,
                    HeightUnits = control.HeightUnits,
                    Shape = control.Shape,
                    HitMarginUnits = control.HitMarginUnits,
                    Priority = control.Priority,
                    GroupOffsetXUnits = control.GroupOffsetXUnits,
                    GroupOffsetYUnits = control.GroupOffsetYUnits,
                },
                Visual = new TouchVisualSpec(
                    control.VisualRole,
                    control.Label,
                    control.Glyph,
                    control.VisualRotationDegrees),
                EditGroupId = control.EditGroupId,
                Category = CategoryOf(control),
            }),
            .. OptionalTemplateControls,
        ]);

    private static TouchControlCategory CategoryOf(TouchControlSpec control) => control.Output switch
    {
        TouchOutputControl.FaceNorth or TouchOutputControl.FaceEast or
        TouchOutputControl.FaceSouth or TouchOutputControl.FaceWest => TouchControlCategory.Face,
        TouchOutputControl.Dpad => TouchControlCategory.Directions,
        TouchOutputControl.PrimaryStick or TouchOutputControl.SecondaryStick or
        TouchOutputControl.PrimaryStickClick or TouchOutputControl.SecondaryStickClick =>
            TouchControlCategory.Sticks,
        TouchOutputControl.L or TouchOutputControl.R or
        TouchOutputControl.ZL or TouchOutputControl.ZR => TouchControlCategory.Shoulders,
        _ => TouchControlCategory.System,
    };

    private static TouchTemplateControl Grip(
        string id, TouchOutputControl output, float x, float y, string label) => new()
    {
        Id = id,
        Output = output,
        Interaction = TouchControlKind.Button,
        Geometry = new TouchControlGeometry
        {
            AnchorX = x / TouchLayoutResolver.ReferenceWidthUnits,
            AnchorY = y / TouchLayoutResolver.ReferenceHeightUnits,
            WidthUnits = 64f,
            HeightUnits = 64f,
            Shape = TouchControlShape.Rectangle,
            HitMarginUnits = 2f,
        },
        Visual = new TouchVisualSpec(TouchVisualRole.RectangularButton, label),
        InDefaultLayout = false,
        Category = TouchControlCategory.Grip,
    };

    // ------------------------------------------------------------------ builders

    private static TouchControlSpec Spec(
        string id,
        TouchOutputControl output,
        TouchControlKind kind,
        TouchControlAction action,
        float x,
        float y,
        float width,
        float height,
        TouchControlShape shape = TouchControlShape.Circle,
        float margin = 0f,
        string label = "",
        TouchControlGlyph? glyph = null,
        TouchVisualRole visualRole = TouchVisualRole.Default,
        float visualRotationDegrees = 0f,
        string? editGroupId = null,
        float groupOffsetXUnits = 0f,
        float groupOffsetYUnits = 0f) => new()
    {
        Id = id,
        CatalogId = id,
        Kind = kind,
        Action = action,
        AnchorX = x / TouchLayoutResolver.ReferenceWidthUnits,
        AnchorY = y / TouchLayoutResolver.ReferenceHeightUnits,
        WidthUnits = width,
        HeightUnits = height,
        Shape = shape,
        HitMarginUnits = margin,
        Label = label,
        Glyph = glyph,
        Output = output,
        VisualRole = visualRole,
        VisualRotationDegrees = visualRotationDegrees,
        AuthoredRotationDegrees = visualRotationDegrees,
        EditGroupId = editGroupId,
        GroupOffsetXUnits = groupOffsetXUnits,
        GroupOffsetYUnits = groupOffsetYUnits,
    };

    /// <summary>
    /// A shoulder or trigger pad.
    ///
    /// Wider than tall because the reachable band along the top edge is a strip, and
    /// because a wide target survives a thumb that arrives at an angle.
    /// </summary>
    private static TouchControlSpec Shoulder(
        string id,
        TouchOutputControl output,
        TouchControlAction action,
        float x,
        float y,
        TouchControlKind kind = TouchControlKind.Button) => Spec(
        id, output, kind, action, x, y, 92f, 56f,
        TouchControlShape.Rectangle, 2f,
        label: action switch
        {
            TouchControlAction.Trigger trigger => trigger.Side == ControlSide.Left ? "ZL" : "ZR",
            TouchControlAction.Logical logical =>
                logical.Button == ControllerButton.L1 ? "L" : "R",
            _ => "",
        },
        visualRole: TouchVisualRole.RectangularButton);

    private static TouchControlSpec Face(
        string id,
        TouchOutputControl output,
        FaceButtonPosition position,
        TouchGroupPlacement placement) => Spec(
        id,
        output,
        TouchControlKind.FaceButton,
        new TouchControlAction.Face(position),
        placement.AnchorX * TouchLayoutResolver.ReferenceWidthUnits,
        placement.AnchorY * TouchLayoutResolver.ReferenceHeightUnits,
        60f,
        60f,
        // No hit margin, deliberately. Adjacent centres are separated by a
        // consistent square-diamond edge, so any expansion here starts eating the
        // neighbour and z-order would begin deciding which button a roll between
        // them lands on.
        margin: 0f,
        visualRole: TouchVisualRole.RoundButton,
        editGroupId: FaceGroup,
        groupOffsetXUnits: placement.OffsetXUnits,
        groupOffsetYUnits: placement.OffsetYUnits);

    /// <summary>Compact utility controls share one rounded-square silhouette.</summary>
    private static TouchControlSpec Utility(
        string id,
        TouchOutputControl output,
        TouchControlAction action,
        float x,
        TouchControlGlyph? glyph = null,
        string label = "") => Spec(
        id, output, TouchControlKind.Button, action, x, 44f, 54f, 54f,
        TouchControlShape.Rectangle, 3f, label, glyph, TouchVisualRole.Utility);

    private static TouchControlSpec Centre(
        string id,
        TouchOutputControl output,
        TouchControlAction action,
        float x,
        float y,
        string label) => Spec(
        id, output, TouchControlKind.Button, action, x, y, 58f, 58f,
        margin: 3f, label: label, visualRole: TouchVisualRole.RoundButton);

    private static TouchControlSpec StickClick(
        string id,
        TouchOutputControl output,
        ControllerButton button,
        float x,
        float y,
        string label) => Spec(
        id, output, TouchControlKind.Button, new TouchControlAction.Logical(button),
        x, y, 56f, 56f, margin: 3f, label: label, visualRole: TouchVisualRole.RoundButton);
}
