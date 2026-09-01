using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Bridge.Touch;

/// <summary>Which side of a two-of-a-kind control this is.</summary>
public enum ControlSide
{
    Left,
    Right,
}

/// <summary>
/// What a control does to the controller state.
///
/// Separated from <see cref="TouchControlKind"/> because kind describes
/// INTERACTION (does it track a vector, or is it a press?) while action describes
/// MEANING. A trigger and a shoulder are both pressed the same way and mean
/// different things; a D-pad and a stick mean different things and are tracked the
/// same way.
/// </summary>
public abstract record TouchControlAction
{
    private TouchControlAction()
    {
    }

    /// <summary>Already-logical bridge button: shoulders, -/+, stick clicks, Home, Capture, C.</summary>
    public sealed record Logical(ControllerButton Button) : TouchControlAction;

    /// <summary>A face-diamond POSITION; the shared layout resolver decides label and bit.</summary>
    public sealed record Face(FaceButtonPosition Position) : TouchControlAction;

    /// <summary>The eight-way directional control.</summary>
    public sealed record Directions : TouchControlAction
    {
        public static readonly Directions Instance = new();
    }

    public sealed record Stick(ControlSide Side) : TouchControlAction;

    /// <summary>
    /// A trigger, and whether the personality behind it has real travel.
    ///
    /// <paramref name="Analog"/> is a statement about the CONSOLE-FACING
    /// controller, not about the on-screen control: only the NSO GameCube
    /// personality carries a continuous trigger byte the console acts on. Pro
    /// Controller 2 and Joy-Con triggers are digital on the far side however hard
    /// they are pulled, so giving them a travel gesture would let a stray drag
    /// silently send nothing at all.
    /// </summary>
    public sealed record Trigger(ControlSide Side, bool Analog = false) : TouchControlAction;
}

/// <summary>
/// How a control is driven by a contact.
///
/// <see cref="Stick"/> and <see cref="Dpad"/> track the contact's position for as
/// long as they own it; everything else is a press that lasts while the contact is
/// held.
/// </summary>
public enum TouchControlKind
{
    Button,
    FaceButton,
    Dpad,
    Stick,
    Trigger,
}

public static class TouchControlKinds
{
    /// <summary>
    /// Whether a persistent hold is even meaningful for this kind of control.
    ///
    /// Digital only. A stick and the unified D-pad are CONTINUOUS controls whose
    /// value is the contact's position, so there is no single state to hold; a
    /// latched one would also be the most disruptive thing on the layout, because
    /// a direction the user cannot see themselves holding walks the character into
    /// a wall. Excluded structurally rather than by configuration so no stored
    /// document can ask for it.
    /// </summary>
    public static bool SupportsLatch(this TouchControlKind value) =>
        value is TouchControlKind.Button or TouchControlKind.FaceButton or TouchControlKind.Trigger;
}

/// <summary>Hit-region shape. Visual rendering may differ; this is what the router tests.</summary>
public enum TouchControlShape
{
    Circle,
    Rectangle,
    GameCubeContour,
}

/// <summary>
/// Platform-neutral drawing role.
///
/// The role says what silhouette the control has, not how a host draws it. In
/// particular it never contains a resource id or asset path. This keeps shipped
/// templates portable while still allowing the GameCube and Joy-Con profiles to
/// look like the controller they actually produce.
/// </summary>
public enum TouchVisualRole
{
    Default,
    RoundButton,
    RectangularButton,
    UnifiedDpad,
    AnalogStick,
    GameCubeLargeA,
    GameCubeSmallB,
    GameCubeBeanX,
    GameCubeBeanY,
    JoyConButton,
    JoyConDirectionButton,
    Utility,
}

/// <summary>
/// Platform-neutral glyph role. The host renderer owns the actual drawing paths;
/// no platform resource id or asset path crosses into the bridge core.
/// </summary>
public enum TouchControlGlyph
{
    Capture,
    Home,
}

/// <summary>
/// One control, described declaratively.
///
/// Three things are kept apart on purpose, because collapsing them is what makes a
/// touch controller impossible to re-lay-out later:
///
/// - <see cref="Action"/> — the semantic effect on controller state;
/// - the geometry fields — where the hit region is and how big;
/// - <see cref="Label"/>/<see cref="Glyph"/> — portable visual content which the
///   host renderer implements.
///
/// Geometry is platform-neutral. <see cref="AnchorX"/>/<see cref="AnchorY"/> are
/// normalized within the interaction region so the same layout describes a phone
/// and a tablet, and sizes are in LOGICAL UNITS — a density-independent unit the
/// platform adapter converts — so a control is a thumb-sized thing rather than a
/// pixel count from whichever device it was authored on.
/// </summary>
public sealed record TouchControlSpec
{
    /// <summary>The INSTANCE id. Unique within a layout; not a binding and not a kind.</summary>
    public required string Id { get; init; }

    /// <summary>Which catalog entry this instance was made from. Not unique.</summary>
    public string CatalogId { get; init; } = string.Empty;

    public required TouchControlKind Kind { get; init; }

    public required TouchControlAction Action { get; init; }

    /// <summary>Centre within the interaction region, <c>0..1</c>.</summary>
    public required float AnchorX { get; init; }

    public required float AnchorY { get; init; }

    /// <summary>Nominal size in logical units, before layout scaling.</summary>
    public required float WidthUnits { get; init; }

    public required float HeightUnits { get; init; }

    public TouchControlShape Shape { get; init; } = TouchControlShape.Circle;

    /// <summary>
    /// Hit margin added around the visual bounds, in logical units.
    ///
    /// Artwork may be smaller than the target it answers to, but an expanded
    /// target that overlaps a neighbour's makes z-order decide what the user
    /// pressed. <see cref="TouchLayoutAudit"/> rejects that.
    /// </summary>
    public float HitMarginUnits { get; init; }

    /// <summary>Higher wins when two hit regions still overlap.</summary>
    public int Priority { get; init; }

    /// <summary>
    /// Draw and hit order within the layout, low to high.
    ///
    /// A real layout property rather than incidental list position, because once
    /// duplicate and freely placed instances exist, "which one is in front" is a
    /// question the user can answer and the arrangement has to remember. The router
    /// reads the same number the renderer does, so the control drawn on top is the
    /// control a thumb lands on.
    /// </summary>
    public int ZIndex { get; init; }

    /// <summary>Drawn legend, when the control's label is not derived from a face layout.</summary>
    public string Label { get; init; } = string.Empty;

    /// <summary>Optional symbol in place of <see cref="Label"/>.</summary>
    public TouchControlGlyph? Glyph { get; init; }

    /// <summary>The personality-visible output this control represents.</summary>
    public TouchOutputControl Output { get; init; } = TouchOutputControl.Unspecified;

    /// <summary>Portable visual treatment; the host owns the actual paths and colours.</summary>
    public TouchVisualRole VisualRole { get; init; } = TouchVisualRole.Default;

    /// <summary>
    /// TOTAL clockwise rotation: the catalog entry's authored orientation plus
    /// whatever the user has turned this instance by.
    ///
    /// One number because rendering and hit testing must never disagree about how
    /// far the silhouette is turned, and a renderer that had to remember to add two
    /// fields is a renderer that will eventually add one.
    /// </summary>
    public float VisualRotationDegrees { get; init; }

    /// <summary>
    /// The catalog entry's own orientation, carried alongside the total so the
    /// editor can offer "reset orientation" and can snap to the authored angle
    /// rather than blindly to zero.
    /// </summary>
    public float AuthoredRotationDegrees { get; init; }

    /// <summary>Instances sharing this id are transformed together by an editor.</summary>
    public string? EditGroupId { get; init; }

    /// <summary>Logical-unit offset from the group's normalized anchor.</summary>
    public float GroupOffsetXUnits { get; init; }

    public float GroupOffsetYUnits { get; init; }

    /// <summary>
    /// The user's hold-to-latch choice for this control.
    ///
    /// Tri-state on purpose. <c>null</c> means "whatever the global setting says",
    /// so changing that setting moves every control the user has not had an opinion
    /// about — which is what a global setting is for. <c>true</c>/<c>false</c> are
    /// explicit per-control answers that survive it.
    /// </summary>
    public bool? Latch { get; init; }

    /// <summary>What to call this control in a message or a label.</summary>
    public string DisplayName => TouchControlNaming.NameFor(Action, Label, Id);

    /// <summary>The catalog entry this came from, defaulting to the instance id.</summary>
    public string ResolvedCatalogId =>
        string.IsNullOrEmpty(CatalogId) ? Id : CatalogId;
}

/// <summary>
/// What to call a control in words a person reads.
///
/// <code>
/// a face binding      the letter it is DRAWN with   A, B, X, Y
/// an authored legend  that legend                   ZL, L3, GL, Z
/// anything else       its id, made readable         Stick left, Dpad
/// a second instance   the same name, numbered       B (2)
/// </code>
///
/// Shared, and used by BOTH the editor's labels and the audit's messages, because
/// those are the same claim made twice. Pro Controller 2's face controls carry no
/// authored legend — their letter is resolved at draw time — so naming them from
/// their ids produced "face-north", "face-east" and so on in every sentence the
/// user was shown. Cardinal slots are an internal way to keep a position separate
/// from the bit it sends; nobody has ever pressed a button called Face East.
/// </summary>
public static class TouchControlNaming
{
    /// <summary>
    /// The presentation every drawn diamond uses.
    ///
    /// Fixed rather than chosen: every controller this surface can emulate is a
    /// Nintendo one, and the retired Nintendo/Xbox mode is not coming back.
    /// </summary>
    public const ControllerFaceLayout FaceLayout = ControllerFaceLayout.Nintendo;

    /// <summary>
    /// <paramref name="instanceId"/> contributes only the copy number. Two
    /// instances of the same catalog entry are the same control to a user, so they
    /// get the same name — but a message about one of them still has to say WHICH,
    /// and "B (2)" is the readable form of the <c>b#2</c> the document stores.
    /// </summary>
    public static string NameFor(TouchControlAction? action, string label, string instanceId)
    {
        var hash = instanceId.IndexOf('#');
        var stem = hash >= 0 ? instanceId[..hash] : instanceId;
        var copy = hash >= 0 ? instanceId[(hash + 1)..] : string.Empty;

        var @base = action is TouchControlAction.Face face
            ? ControllerLayoutResolver.FaceLabel(face.Position, FaceLayout)
            : !string.IsNullOrWhiteSpace(label)
                ? label
                : Readable(stem);

        return string.IsNullOrWhiteSpace(copy) ? @base : $"{@base} ({copy})";
    }

    private static string Readable(string stem)
    {
        if (stem.Length == 0)
        {
            return stem;
        }

        var spaced = stem.Replace('-', ' ');
        return char.ToUpperInvariant(spaced[0]) + spaced[1..];
    }
}

/// <summary>
/// A complete on-screen controller, versioned.
///
/// <see cref="SchemaVersion"/> exists from the first release even though nothing
/// migrates yet: the alternative is discovering later that a stored layout blob
/// has no way to say which shape it is, and the only safe response then is to
/// throw every user's configuration away.
/// </summary>
public sealed record TouchLayout(
    string Id,
    int SchemaVersion,
    IReadOnlyList<TouchControlSpec> Controls)
{
    public TouchProfileId? ProfileId { get; init; }

    public string? TemplateId { get; init; }

    public int TemplateRevision { get; init; } = 1;
}

/// <summary>
/// The rectangle a layout is resolved into, plus the platform's unit scale.
///
/// This is the interaction-safe area, NOT the window: system gesture strips,
/// cutouts and caption bars are subtracted by the platform adapter before the
/// layout ever sees a number. A background may still be drawn edge to edge; that
/// is decoration and never changes hit geometry.
///
/// <see cref="UnitScale"/> is pixels per logical unit — the one place the portable
/// layer is told how big a physical thumb is on this display.
/// </summary>
public readonly record struct TouchLayoutRegion(
    float Left,
    float Top,
    float Right,
    float Bottom,
    float UnitScale)
{
    public float Width => Right - Left;

    public float Height => Bottom - Top;

    public float WidthUnits => UnitScale > 0f ? Width / UnitScale : 0f;

    public float HeightUnits => UnitScale > 0f ? Height / UnitScale : 0f;
}

/// <summary>One control placed in real coordinates.</summary>
public sealed class ResolvedTouchControl
{
    private readonly float rotatedExtentX;
    private readonly float rotatedExtentY;

    public ResolvedTouchControl(
        TouchControlSpec spec,
        float centerX,
        float centerY,
        float halfWidth,
        float halfHeight,
        float hitHalfWidth,
        float hitHalfHeight)
    {
        Spec = spec;
        CenterX = centerX;
        CenterY = centerY;
        HalfWidth = halfWidth;
        HalfHeight = halfHeight;
        HitHalfWidth = hitHalfWidth;
        HitHalfHeight = hitHalfHeight;

        // Computed once per resolve, never per contact: geometry changes only when
        // the window or the layout does.
        (rotatedExtentX, rotatedExtentY) = ComputeRotatedExtents();
    }

    public TouchControlSpec Spec { get; }

    public float CenterX { get; }

    public float CenterY { get; }

    /// <summary>Visual half-extent.</summary>
    public float HalfWidth { get; }

    public float HalfHeight { get; }

    /// <summary>Hit half-extent; never smaller than the visual one.</summary>
    public float HitHalfWidth { get; }

    public float HitHalfHeight { get; }

    public string Id => Spec.Id;

    /// <summary>Travel radius for the vector controls; the smaller half-extent keeps it circular.</summary>
    public float TrackingRadius => MathF.Min(HalfWidth, HalfHeight);

    /// <summary>
    /// Axis-aligned half-extents of the answerable region AFTER rotation.
    ///
    /// Not the same as <see cref="HitHalfWidth"/>/<see cref="HitHalfHeight"/> once
    /// a control is turned: those are the control's own frame. Every screen-space
    /// question — is this inside the safe rectangle, can these two possibly
    /// overlap, how far may this be dragged — has to be asked in screen space, and
    /// asking it with the unrotated extents is how a rotated control ends up half
    /// under a system gesture strip.
    /// </summary>
    public float HitExtentX => rotatedExtentX;

    public float HitExtentY => rotatedExtentY;

    /// <summary>
    /// How far the control's axis-aligned box must be turned to describe its
    /// screen-space region — which is NOT always the visual rotation.
    ///
    /// <code>
    /// Rectangle          the box IS the region                     -> turn it
    /// Circle, round      rotation-invariant                        -> do not
    /// Circle, elliptical the box IS the region                     -> turn it
    /// GameCubeContour    the contour is rotated INSIDE an upright box -> do not
    /// </code>
    ///
    /// Here rather than in a renderer because a selection outline, a debug overlay
    /// and any future chrome all have to agree with the router about where a
    /// control actually is.
    /// </summary>
    public float OutlineRotationDegrees => Spec.Shape switch
    {
        TouchControlShape.Rectangle => Spec.VisualRotationDegrees,
        TouchControlShape.Circle =>
            HitHalfWidth == HitHalfHeight ? 0f : Spec.VisualRotationDegrees,
        _ => 0f,
    };

    /// <summary>
    /// Hit test in the control's own frame.
    ///
    /// A rotated control is tested by rotating the POINT backwards rather than by
    /// building a rotated polygon: one inverse transform reuses the same local
    /// shape test the unrotated case uses, so the two can never drift apart, and
    /// nothing is allocated on the contact path.
    /// </summary>
    public bool HitTest(float x, float y) => Contains(x, y, includeMargin: true);

    /// <summary>
    /// The DRAWN shape, without the courtesy touch margin around it.
    ///
    /// Used by the layout audit to tell two genuinely colliding controls from two
    /// whose margins merely meet. Never used for routing a contact — a control
    /// answers to its margin, which is the whole point of having one.
    /// </summary>
    public bool ContainsVisual(float x, float y) => Contains(x, y, includeMargin: false);

    /// <summary>
    /// How central the hit is, <c>0</c> at the centre and <c>1</c> at the edge.
    ///
    /// Used only to break a tie between two controls that both accept the point, so
    /// an overlap resolves to the one the thumb is more plainly on rather than to
    /// whichever happens to be later in the list.
    /// </summary>
    public float NormalizedDistance(float x, float y)
    {
        if (HitHalfWidth <= 0f || HitHalfHeight <= 0f)
        {
            return float.MaxValue;
        }

        var (lx, ly) = LocalPoint(x - CenterX, y - CenterY);
        return MathF.Max(MathF.Abs(lx / HitHalfWidth), MathF.Abs(ly / HitHalfHeight));
    }

    private bool Contains(float x, float y, bool includeMargin)
    {
        var halfW = includeMargin ? HitHalfWidth : HalfWidth;
        var halfH = includeMargin ? HitHalfHeight : HalfHeight;
        if (halfW <= 0f || halfH <= 0f)
        {
            return false;
        }

        var dx = x - CenterX;
        var dy = y - CenterY;

        switch (Spec.Shape)
        {
            case TouchControlShape.Circle:
            {
                // Rotation-invariant when the two half-extents are equal, which is
                // every circular control in the shipped catalog. Rotating the point
                // anyway costs a sin/cos per contact for no change in the answer.
                var (lx, ly) = halfW == halfH ? (dx, dy) : LocalPoint(dx, dy);
                var nx = lx / halfW;
                var ny = ly / halfH;
                return (nx * nx) + (ny * ny) <= 1f;
            }

            case TouchControlShape.Rectangle:
            {
                var (lx, ly) = LocalPoint(dx, dy);
                return MathF.Abs(lx) <= halfW && MathF.Abs(ly) <= halfH;
            }

            default:
                // The contour test does its own inverse rotation from the same
                // total angle, so the point arrives here unrotated. The contour is
                // always the DRAWN one; the margin is the courtesy expansion.
                return TouchGameCubeGeometry.Contains(
                    Spec.VisualRole,
                    dx,
                    dy,
                    HalfWidth * 2f,
                    HalfHeight * 2f,
                    Spec.VisualRotationDegrees,
                    includeMargin ? HitHalfWidth - HalfWidth : 0f);
        }
    }

    /// <summary>A screen-space offset from the centre, expressed in the control's frame.</summary>
    private (float X, float Y) LocalPoint(float dx, float dy)
    {
        var degrees = Spec.VisualRotationDegrees;
        if (degrees == 0f)
        {
            return (dx, dy);
        }

        var radians = -degrees * Math.PI / 180d;
        var cosine = (float)Math.Cos(radians);
        var sine = (float)Math.Sin(radians);
        return ((dx * cosine) - (dy * sine), (dx * sine) + (dy * cosine));
    }

    private (float X, float Y) ComputeRotatedExtents()
    {
        var degrees = Spec.VisualRotationDegrees;
        if (degrees == 0f && Spec.Shape != TouchControlShape.GameCubeContour)
        {
            return (HitHalfWidth, HitHalfHeight);
        }

        var radians = degrees * Math.PI / 180d;
        var cosine = (float)Math.Abs(Math.Cos(radians));
        var sine = (float)Math.Abs(Math.Sin(radians));

        switch (Spec.Shape)
        {
            case TouchControlShape.Rectangle:
                return (
                    (HitHalfWidth * cosine) + (HitHalfHeight * sine),
                    (HitHalfWidth * sine) + (HitHalfHeight * cosine));

            case TouchControlShape.Circle:
            {
                var x = HitHalfWidth * cosine;
                var y = HitHalfHeight * sine;
                var u = HitHalfWidth * sine;
                var v = HitHalfHeight * cosine;
                return (
                    MathF.Sqrt((x * x) + (y * y)),
                    MathF.Sqrt((u * u) + (v * v)));
            }

            default:
            {
                // A bean is inscribed in its box and already rotated by the shared
                // contour helper, so its real extent comes from the contour itself.
                // The touch margin expands it in every direction.
                var margin = HitHalfWidth - HalfWidth;
                var points = TouchGameCubeGeometry.OrientedContour(
                    Spec.VisualRole, HalfWidth * 2f, HalfHeight * 2f, degrees);
                if (points.Count == 0)
                {
                    return (HitHalfWidth, HitHalfHeight);
                }

                var maxX = points.Max(point => MathF.Abs(point.X));
                var maxY = points.Max(point => MathF.Abs(point.Y));
                return (maxX + margin, maxY + margin);
            }
        }
    }
}

/// <summary>
/// A layout placed into a region.
///
/// <see cref="Fits"/> is the layout's own verdict on whether it can be played. A
/// window can genuinely become too small — dragged narrow, a very short landscape
/// at a large font scale — and drawing overlapping controls there would send the
/// console input the user did not choose. The surface is expected to neutralize
/// and say so instead.
/// </summary>
public sealed class ResolvedTouchLayout
{
    private readonly Dictionary<string, ResolvedTouchControl> byId;

    public ResolvedTouchLayout(
        TouchLayout layout,
        TouchLayoutRegion region,
        IReadOnlyList<ResolvedTouchControl> controls,
        float scale,
        bool fits,
        string? problem = null,
        bool regionTooSmall = false,
        IReadOnlyList<TouchLayoutFinding>? findings = null)
    {
        Layout = layout;
        Region = region;
        Controls = controls;
        Scale = scale;
        Fits = fits;
        Problem = problem;
        RegionTooSmall = regionTooSmall;
        Findings = findings ?? [];

        // Built once so the router's per-move owner lookup is not a list scan.
        byId = new Dictionary<string, ResolvedTouchControl>(controls.Count, StringComparer.Ordinal);
        foreach (var control in controls)
        {
            byId[control.Id] = control;
        }

        InvalidControlIds = Findings
            .Where(finding => finding.Blocking)
            .SelectMany(finding => finding.ControlIds)
            .ToHashSet(StringComparer.Ordinal);
    }

    public static readonly ResolvedTouchLayout Empty = new(
        new TouchLayout("empty", TouchLayoutV1.SchemaVersion, []),
        new TouchLayoutRegion(0f, 0f, 0f, 0f, 1f),
        [],
        1f,
        fits: false,
        problem: "No interaction area has been measured yet",
        regionTooSmall: true);

    public TouchLayout Layout { get; }

    public TouchLayoutRegion Region { get; }

    public IReadOnlyList<ResolvedTouchControl> Controls { get; }

    public float Scale { get; }

    public bool Fits { get; }

    public string? Problem { get; }

    /// <summary>
    /// The rectangle itself is below the resolver's minimum.
    ///
    /// Distinct from a merely failing audit because no EDIT can clear it: moving or
    /// shrinking controls does not make the window bigger. A surface that offered
    /// layout editing here would be offering a repair that cannot work.
    /// </summary>
    public bool RegionTooSmall { get; }

    /// <summary>
    /// Everything the audit said about this exact geometry.
    ///
    /// Carried on the resolved layout rather than recomputed by whoever wants it,
    /// so an editor highlighting a broken control and the runtime deciding whether
    /// to play the layout are reading one answer. Recomputing invited the failure
    /// where the canvas says a control fits and the validator refuses it.
    /// </summary>
    public IReadOnlyList<TouchLayoutFinding> Findings { get; }

    /// <summary>
    /// The instances a blocking finding names, for a surface to mark as broken.
    ///
    /// Blocking only: a non-blocking finding is information, and painting a control
    /// red for one would teach the user to ignore the colour.
    /// </summary>
    public IReadOnlySet<string> InvalidControlIds { get; }

    public ResolvedTouchControl? Control(string id) =>
        byId.TryGetValue(id, out var control) ? control : null;

    /// <summary>The shortest true thing to say about <paramref name="id"/>, when it is broken.</summary>
    public string? ProblemFor(string id) => Findings
        .FirstOrDefault(finding => finding.Blocking && finding.ControlIds.Contains(id))
        ?.Message;
}

/// <summary>
/// Normalized layout + real rectangle -> real control geometry.
///
/// The scaling rule is deliberately not "multiply everything by the window width".
/// A twelve-inch tablet does not have larger thumbs, so past a point the controls
/// stop growing and the SPACE between them absorbs the extra room. That keeps
/// every control at the edge where a thumb can reach it and preserves the quiet
/// centre, which is the whole ergonomic argument for this arrangement.
/// </summary>
public static class TouchLayoutResolver
{
    /// <summary>
    /// The window this layout was authored against, in logical units. Scale is
    /// relative to this, so <c>1.0</c> is the shape the numbers were chosen for.
    /// </summary>
    public const float ReferenceWidthUnits = 800f;

    public const float ReferenceHeightUnits = 400f;

    /// <summary>Bounds on that scale; see the type doc for why the top one is low.</summary>
    public const float MinScale = 0.78f;

    public const float MaxScale = 1.25f;

    /// <summary>
    /// Smallest region a controller can be laid out in at all.
    ///
    /// The reference shape at <see cref="MinScale"/>, because that is the point
    /// below which the controls stop shrinking with the window and start closing
    /// the gaps between each other. Below it the audit would fail anyway; refusing
    /// here gives the surface one truthful thing to say instead of a screenful of
    /// overlapping targets.
    /// </summary>
    public const float MinRegionWidthUnits = ReferenceWidthUnits * MinScale;

    public const float MinRegionHeightUnits = ReferenceHeightUnits * MinScale;

    public static ResolvedTouchLayout Resolve(
        TouchLayout layout,
        TouchLayoutRegion region,
        TouchLayoutAuditMode auditMode = TouchLayoutAuditMode.Runtime)
    {
        if (region.UnitScale <= 0f || region.Width <= 0f || region.Height <= 0f)
        {
            return new ResolvedTouchLayout(
                layout, region, [], 1f, fits: false,
                problem: "The interaction area has no usable size",
                regionTooSmall: true);
        }

        var scale = Math.Clamp(
            MathF.Min(
                region.WidthUnits / ReferenceWidthUnits,
                region.HeightUnits / ReferenceHeightUnits),
            MinScale,
            MaxScale);

        var controls = layout.Controls.Select(spec => Place(spec, region, scale)).ToList();

        var tooSmall = region.WidthUnits < MinRegionWidthUnits ||
                       region.HeightUnits < MinRegionHeightUnits;

        var profile = layout.ProfileId is { } profileId
            ? TouchProfileCatalog.Profile(profileId)
            : null;

        var findings = layout.ProfileId is not null && profile is null
            ? [new TouchLayoutFinding($"No touch profile is registered for {layout.ProfileId}", true)]
            : profile is not null
                ? TouchLayoutAudit.Audit(layout, controls, region, profile, auditMode)
                : TouchLayoutAudit.Audit(controls, region);

        var problem = tooSmall
            ? "This window is too small for the on-screen controller"
            : findings.FirstOrDefault(finding => finding.Blocking)?.Message;

        return new ResolvedTouchLayout(
            layout, region, controls, scale,
            fits: problem is null,
            problem: problem,
            regionTooSmall: tooSmall,
            findings: findings);
    }

    private static ResolvedTouchControl Place(
        TouchControlSpec spec, TouchLayoutRegion region, float scale)
    {
        var unit = region.UnitScale * scale;
        var halfWidth = spec.WidthUnits * unit / 2f;
        var halfHeight = spec.HeightUnits * unit / 2f;
        var margin = spec.HitMarginUnits * unit;

        // Do not silently repair an out-of-bounds user override. The audit must see
        // the authored result and block it; otherwise the persisted geometry says
        // one thing while the control is drawn and hit-tested somewhere else.
        var centerX = region.Left + (spec.AnchorX * region.Width) + (spec.GroupOffsetXUnits * unit);
        var centerY = region.Top + (spec.AnchorY * region.Height) + (spec.GroupOffsetYUnits * unit);

        return new ResolvedTouchControl(
            spec, centerX, centerY, halfWidth, halfHeight,
            halfWidth + margin, halfHeight + margin);
    }
}
