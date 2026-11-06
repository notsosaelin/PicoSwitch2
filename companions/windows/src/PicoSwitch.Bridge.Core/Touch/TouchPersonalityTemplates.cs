namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// Personality-specific shipped geometry. The Pro2 baseline remains in
/// <see cref="TouchLayoutV1"/>.
/// </summary>
public static class TouchPersonalityTemplates
{
    private const int Schema = 1;
    private const int Revision = 2;

    /// <summary>
    /// Both sideways halves moved their action cluster into the shell's real
    /// orientation, so their shipped geometry is no longer revision 2.
    ///
    /// A revision is metadata, not a schema gate: no control id changed, so a stored
    /// profile written against revision 2 still composes cleanly and every
    /// customized position the user saved is applied exactly as before. What the
    /// bump records is that the DEFAULTS moved, which is the difference between
    /// "this layout is stale" and "this layout is wrong".
    /// </summary>
    private const int JoyConRevision = 3;

    /// <summary>
    /// Centre of the GameCube shoulder/trigger strip.
    ///
    /// 42 is the established, hardware-validated position and must stay there. An
    /// Editor 2.0 pass briefly moved it to 34 because the audit — newly taught to
    /// measure rotated geometry — reported <c>z</c> overlapping the <c>Y</c> bean.
    /// The overlap is real, but it lies entirely between the two controls' HIT
    /// MARGINS: the drawn shapes clear each other by about a unit. Courtesy margins
    /// meeting is not a control the user cannot press, and
    /// <see cref="TouchLayoutAudit"/> says so rather than refusing the layout.
    /// </summary>
    private const float GameCubeTopRowY = 42f;

    private const string FaceGroup = "face-cluster";
    private const string DirectionGroup = "direction-cluster";
    private const string UtilityGroup = "utility-cluster";
    private const string SecondaryGroup = "secondary-cluster";

    private const float GameCubeFaceScale = 1.00963f;
    private const float GameCubeFaceCenterX = 682.737f;
    private const float GameCubeFaceCenterY = 166.4221f;
    private const float GameCubeFaceNudgeX = -2.72f;
    private const float GameCubeFaceNudgeY = 2.25f;
    private const float GameCubeUtilitySpacing = 70f;
    private const float GameCubeSecondaryOffsetX = 170f;
    private const float JoyConPrimaryCenterY = 255f;
    private const float JoyConButtonClusterCenterX = 636f;

    // These are the approved no-override defaults. Keep the complete asymmetric
    // cluster opposite the upper-left main stick; do not rely on a persisted editor
    // scale or translation to supply the shipped GameCube geometry.
    private static readonly TouchGroupGeometry GameCubeFaceGroup =
        new(GameCubeFaceCenterX, GameCubeFaceCenterY);

    private static readonly TouchGroupGeometry GameCubeTopUtilities = new(400f, 44f);

    private static readonly TouchGroupGeometry GameCubeSecondaryControls =
        new(400f, TouchLayoutV1.LeftSecondaryYUnits);

    // Both halves are drawn as the physical shell they are, turned the way it is
    // actually held. Each map is keyed by the button's slot ON THE JOY-CON, so
    // North below means "the shell's top button" and the rotation decides where that
    // lands on screen. Writing screen positions here directly is what previously put
    // `direction-up` at the top of the display, where it reads as the X-position
    // button while the console treats it as Y.
    private const TouchClusterRotation JoyConLeftRotation =
        TouchClusterRotation.QuarterCounterClockwise;

    private const TouchClusterRotation JoyConRightRotation =
        TouchClusterRotation.QuarterClockwise;

    private static readonly IReadOnlyDictionary<TouchCardinalSlot, TouchGroupPlacement>
        JoyConLeftDirections =
            new TouchGroupGeometry(JoyConButtonClusterCenterX, JoyConPrimaryCenterY)
                .SquareDiamond(60f, JoyConLeftRotation);

    private static readonly IReadOnlyDictionary<TouchCardinalSlot, TouchGroupPlacement>
        JoyConRightFace =
            new TouchGroupGeometry(JoyConButtonClusterCenterX, JoyConPrimaryCenterY)
                .SquareDiamond(60f, JoyConRightRotation);

    public static readonly TouchLayoutTemplate GameCube = new(
        "picoswitch.touch.gc.v1",
        TouchProfileId.GameCube,
        Schema,
        Revision,
        [
            Pad("trigger-l", TouchOutputControl.L, 60f, GameCubeTopRowY, "L", TouchControlKind.Trigger),
            Pad("zl", TouchOutputControl.ZL, 160f, GameCubeTopRowY, "ZL"),
            GroupUtility("capture", TouchOutputControl.Capture,
                GameCubeTopUtilities.At(-GameCubeUtilitySpacing, 0f), TouchControlGlyph.Capture),
            GroupUtility("home", TouchOutputControl.Home,
                GameCubeTopUtilities.At(0f, 0f), TouchControlGlyph.Home),
            GroupUtility("chat", TouchOutputControl.C,
                GameCubeTopUtilities.At(GameCubeUtilitySpacing, 0f), label: "C"),
            Pad("z", TouchOutputControl.Z, 640f, GameCubeTopRowY, "Z"),
            Pad("trigger-r", TouchOutputControl.R, 740f, GameCubeTopRowY, "R", TouchControlKind.Trigger),

            // GameCube controls substitute directly into the proven Pro2
            // major-control composition; only their personality-specific art differs.
            Vector("main-stick", TouchOutputControl.PrimaryStick, TouchControlKind.Stick,
                TouchLayoutV1.LeftPrimaryXUnits, TouchLayoutV1.LeftPrimaryYUnits, 140f),
            GroupVector("dpad", TouchOutputControl.Dpad, TouchControlKind.Dpad,
                GameCubeSecondaryControls.At(-GameCubeSecondaryOffsetX, 0f), 140f),
            GroupVector("c-stick", TouchOutputControl.SecondaryStick, TouchControlKind.Stick,
                GameCubeSecondaryControls.At(GameCubeSecondaryOffsetX, 0f), 140f),
            Round("plus", TouchOutputControl.Plus,
                TouchLayoutResolver.ReferenceWidthUnits / 2f, 240f, 52f, "+"),

            // Physical GameCube relationship: large A at the centre, B south-west,
            // vertical X to the east, and horizontal Y north-west.
            GcFace("a", TouchOutputControl.A, GameCubeFaceAt(0f, 0f),
                88f, 88f, "A", TouchVisualRole.GameCubeLargeA),
            GcFace("b", TouchOutputControl.B, GameCubeFaceAt(-68.5f, 57.5f),
                56f, 56f, "B", TouchVisualRole.GameCubeSmallB, margin: 1f),
            GcFace("x", TouchOutputControl.X, GameCubeFaceAt(77f, -5f),
                52f, 84f, "X", TouchVisualRole.GameCubeBeanX, margin: 7f, rotationDegrees: 10.7f),
            GcFace("y", TouchOutputControl.Y, GameCubeFaceAt(-37.5f, -67.25f),
                84f, 54f, "Y", TouchVisualRole.GameCubeBeanY, rotationDegrees: -11.0f),
        ]);

    public static readonly TouchLayoutTemplate JoyConLeft = new(
        "picoswitch.touch.jcl.v1",
        TouchProfileId.JoyConLeft,
        Schema,
        JoyConRevision,
        [
            Pad("sl", TouchOutputControl.SL, 60f, 42f, "SL"),
            Pad("l", TouchOutputControl.L, 160f, 42f, "L", TouchControlKind.Trigger),
            Utility("minus", TouchOutputControl.Minus, 365f, label: "-"),
            Utility("capture", TouchOutputControl.Capture, 435f, TouchControlGlyph.Capture),
            Pad("zl", TouchOutputControl.ZL, 640f, 42f, "ZL", TouchControlKind.Trigger),
            Pad("sr", TouchOutputControl.SR, 740f, 42f, "SR"),
            Vector("primary-stick", TouchOutputControl.PrimaryStick, TouchControlKind.Stick,
                150f, JoyConPrimaryCenterY, 150f),
            Round("stick-click", TouchOutputControl.PrimaryStickClick, 285f, 320f, 56f, "L3",
                TouchControlCategory.Sticks),

            // Slots are the Joy-Con's own: its up button is the north button of its
            // diamond, and the rotation puts it at the player's left. The triangles
            // are physical MARKINGS and turn with the shell, so the one at the left
            // points left — which is also what makes the cluster read as an ordinary
            // face diamond in the hand.
            JoyButton("direction-up", TouchOutputControl.DirectionUp,
                JoyConLeftDirections[TouchCardinalSlot.North], group: DirectionGroup,
                role: TouchVisualRole.JoyConDirectionButton,
                rotationDegrees: JoyConLeftRotation.Degrees()),
            JoyButton("direction-left", TouchOutputControl.DirectionLeft,
                JoyConLeftDirections[TouchCardinalSlot.West], group: DirectionGroup,
                role: TouchVisualRole.JoyConDirectionButton,
                rotationDegrees: JoyConLeftRotation.Degrees()),
            JoyButton("direction-right", TouchOutputControl.DirectionRight,
                JoyConLeftDirections[TouchCardinalSlot.East], group: DirectionGroup,
                role: TouchVisualRole.JoyConDirectionButton,
                rotationDegrees: JoyConLeftRotation.Degrees()),
            JoyButton("direction-down", TouchOutputControl.DirectionDown,
                JoyConLeftDirections[TouchCardinalSlot.South], group: DirectionGroup,
                role: TouchVisualRole.JoyConDirectionButton,
                rotationDegrees: JoyConLeftRotation.Degrees()),
        ]);

    public static readonly TouchLayoutTemplate JoyConRight = new(
        "picoswitch.touch.jcr.v1",
        TouchProfileId.JoyConRight,
        Schema,
        JoyConRevision,
        [
            Pad("sl", TouchOutputControl.SL, 60f, 42f, "SL"),
            Pad("r", TouchOutputControl.R, 160f, 42f, "R", TouchControlKind.Trigger),
            Utility("plus", TouchOutputControl.Plus, 330f, label: "+"),
            Utility("home", TouchOutputControl.Home, 400f, TouchControlGlyph.Home),
            Utility("chat", TouchOutputControl.C, 470f, label: "C"),
            Pad("zr", TouchOutputControl.ZR, 640f, 42f, "ZR", TouchControlKind.Trigger),
            Pad("sr", TouchOutputControl.SR, 740f, 42f, "SR"),
            Vector("primary-stick", TouchOutputControl.PrimaryStick, TouchControlKind.Stick,
                150f, JoyConPrimaryCenterY, 150f),
            Round("stick-click", TouchOutputControl.PrimaryStickClick, 285f, 320f, 56f, "R3",
                TouchControlCategory.Sticks),

            // Same rule, opposite turn. The letters are identity markings rather than
            // direction markings, so they stay upright and readable while only their
            // POSITIONS rotate: the X printed at the top of an upright Joy-Con (R)
            // really is at the player's right once the shell is turned clockwise, and
            // the console reads it as the A position.
            JoyButton("x", TouchOutputControl.X,
                JoyConRightFace[TouchCardinalSlot.North], label: "X", group: FaceGroup),
            JoyButton("y", TouchOutputControl.Y,
                JoyConRightFace[TouchCardinalSlot.West], label: "Y", group: FaceGroup),
            JoyButton("a", TouchOutputControl.A,
                JoyConRightFace[TouchCardinalSlot.East], label: "A", group: FaceGroup),
            JoyButton("b", TouchOutputControl.B,
                JoyConRightFace[TouchCardinalSlot.South], label: "B", group: FaceGroup),
        ]);

    // ------------------------------------------------------------------ builders

    private static TouchControlGeometry Geometry(
        float x,
        float y,
        float width,
        float height,
        TouchControlShape shape = TouchControlShape.Circle,
        float margin = 0f,
        float groupOffsetXUnits = 0f,
        float groupOffsetYUnits = 0f) => new()
    {
        AnchorX = x / TouchLayoutResolver.ReferenceWidthUnits,
        AnchorY = y / TouchLayoutResolver.ReferenceHeightUnits,
        WidthUnits = width,
        HeightUnits = height,
        Shape = shape,
        HitMarginUnits = margin,
        GroupOffsetXUnits = groupOffsetXUnits,
        GroupOffsetYUnits = groupOffsetYUnits,
    };

    private static TouchTemplateControl Pad(
        string id,
        TouchOutputControl output,
        float x,
        float y,
        string label,
        TouchControlKind kind = TouchControlKind.Button) => new()
    {
        Id = id,
        Output = output,
        Interaction = kind,
        Geometry = Geometry(x, y, 88f, 56f, TouchControlShape.Rectangle, 2f),
        Visual = new TouchVisualSpec(TouchVisualRole.RectangularButton, label),
        Category = TouchControlCategory.Shoulders,
    };

    private static TouchTemplateControl Utility(
        string id,
        TouchOutputControl output,
        float x,
        TouchControlGlyph? glyph = null,
        string label = "") => new()
    {
        Id = id,
        Output = output,
        Interaction = TouchControlKind.Button,
        Geometry = Geometry(x, 44f, 50f, 50f, TouchControlShape.Rectangle, 3f),
        Visual = new TouchVisualSpec(TouchVisualRole.Utility, label, glyph),
        Category = TouchControlCategory.System,
    };

    private static TouchTemplateControl GroupUtility(
        string id,
        TouchOutputControl output,
        TouchGroupPlacement placement,
        TouchControlGlyph? glyph = null,
        string label = "") => new()
    {
        Id = id,
        Output = output,
        Interaction = TouchControlKind.Button,
        Geometry = Geometry(
            placement.AnchorX * TouchLayoutResolver.ReferenceWidthUnits,
            placement.AnchorY * TouchLayoutResolver.ReferenceHeightUnits,
            50f, 50f, TouchControlShape.Rectangle, 3f,
            placement.OffsetXUnits, placement.OffsetYUnits),
        Visual = new TouchVisualSpec(TouchVisualRole.Utility, label, glyph),
        EditGroupId = UtilityGroup,
        Category = TouchControlCategory.System,
    };

    private static TouchTemplateControl Round(
        string id,
        TouchOutputControl output,
        float x,
        float y,
        float size,
        string label,
        TouchControlCategory category = TouchControlCategory.System) => new()
    {
        Id = id,
        Output = output,
        Interaction = TouchControlKind.Button,
        Geometry = Geometry(x, y, size, size, margin: 2f),
        Visual = new TouchVisualSpec(TouchVisualRole.RoundButton, label),
        Category = category,
    };

    private static TouchTemplateControl Vector(
        string id,
        TouchOutputControl output,
        TouchControlKind kind,
        float x,
        float y,
        float size,
        float margin = 0f,
        TouchVisualRole? visualRole = null) => new()
    {
        Id = id,
        Output = output,
        Interaction = kind,
        Geometry = Geometry(x, y, size, size, margin: margin),
        Visual = new TouchVisualSpec(visualRole ?? DefaultVectorRole(kind)),
        Category = VectorCategory(kind),
    };

    private static TouchTemplateControl GroupVector(
        string id,
        TouchOutputControl output,
        TouchControlKind kind,
        TouchGroupPlacement placement,
        float size) => new()
    {
        Id = id,
        Output = output,
        Interaction = kind,
        Geometry = Geometry(
            placement.AnchorX * TouchLayoutResolver.ReferenceWidthUnits,
            placement.AnchorY * TouchLayoutResolver.ReferenceHeightUnits,
            size, size,
            groupOffsetXUnits: placement.OffsetXUnits,
            groupOffsetYUnits: placement.OffsetYUnits),
        Visual = new TouchVisualSpec(DefaultVectorRole(kind)),
        EditGroupId = SecondaryGroup,
        Category = VectorCategory(kind),
    };

    private static TouchVisualRole DefaultVectorRole(TouchControlKind kind) =>
        kind == TouchControlKind.Dpad ? TouchVisualRole.UnifiedDpad : TouchVisualRole.AnalogStick;

    private static TouchControlCategory VectorCategory(TouchControlKind kind) =>
        kind == TouchControlKind.Dpad ? TouchControlCategory.Directions : TouchControlCategory.Sticks;

    private static TouchGroupPlacement GameCubeFaceAt(float x, float y) =>
        GameCubeFaceGroup.At(x + GameCubeFaceNudgeX, y + GameCubeFaceNudgeY);

    private static TouchTemplateControl GcFace(
        string id,
        TouchOutputControl output,
        TouchGroupPlacement placement,
        float width,
        float height,
        string label,
        TouchVisualRole role,
        float margin = 0f,
        float rotationDegrees = 0f)
    {
        var scaledWidth = width * GameCubeFaceScale;
        var scaledHeight = height * GameCubeFaceScale;
        var minimumAuthoredTarget = TouchLayoutAudit.MinTargetUnits / TouchLayoutResolver.MinScale;
        var accessibleMargin = MathF.Max(
            (minimumAuthoredTarget - MathF.Min(scaledWidth, scaledHeight)) / 2f, 0f);

        return new TouchTemplateControl
        {
            Id = id,
            Output = output,
            Interaction = TouchControlKind.Button,
            Geometry = Geometry(
                placement.AnchorX * TouchLayoutResolver.ReferenceWidthUnits,
                placement.AnchorY * TouchLayoutResolver.ReferenceHeightUnits,
                scaledWidth,
                scaledHeight,
                shape: role is TouchVisualRole.GameCubeBeanX or TouchVisualRole.GameCubeBeanY
                    ? TouchControlShape.GameCubeContour
                    : TouchControlShape.Circle,
                margin: MathF.Max(margin * GameCubeFaceScale, accessibleMargin),
                groupOffsetXUnits: placement.OffsetXUnits * GameCubeFaceScale,
                groupOffsetYUnits: placement.OffsetYUnits * GameCubeFaceScale),
            Visual = new TouchVisualSpec(role, label, RotationDegrees: rotationDegrees),
            EditGroupId = FaceGroup,
            Category = TouchControlCategory.Face,
        };
    }

    private static TouchTemplateControl JoyButton(
        string id,
        TouchOutputControl output,
        TouchGroupPlacement placement,
        string group,
        string label = "",
        TouchVisualRole role = TouchVisualRole.JoyConButton,
        /// <summary>Turns the DRAWN MARKING with the shell; never the label text.</summary>
        float rotationDegrees = 0f) => new()
    {
        Id = id,
        Output = output,
        Interaction = TouchControlKind.Button,
        Geometry = Geometry(
            placement.AnchorX * TouchLayoutResolver.ReferenceWidthUnits,
            placement.AnchorY * TouchLayoutResolver.ReferenceHeightUnits,
            58f, 58f,
            groupOffsetXUnits: placement.OffsetXUnits,
            groupOffsetYUnits: placement.OffsetYUnits),
        Visual = new TouchVisualSpec(role, label, RotationDegrees: rotationDegrees),
        EditGroupId = group,
        Category = TouchControlCategory.Face,
    };
}
