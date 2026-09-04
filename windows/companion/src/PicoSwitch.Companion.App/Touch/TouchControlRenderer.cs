using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Touch;
using Windows.Foundation;
using Windows.UI;

// Microsoft.UI.Xaml.Shapes.Path is the drawing primitive this file is about; the
// implicit System.IO using would otherwise make the name ambiguous.
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace PicoSwitch.Companion.App.Touch;

/// <summary>What to draw on top of the controls this pass.</summary>
public sealed record TouchRenderOptions
{
    public IReadOnlySet<string> Selection { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    /// <summary>Instances a BLOCKING audit finding names. Advisory findings are not painted.</summary>
    public IReadOnlySet<string> Invalid { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    public IReadOnlyList<TouchGuideLine> Guides { get; init; } = [];

    public IReadOnlyList<TouchGuideLine> Grid { get; init; } = [];

    /// <summary>Editing chrome — selection outlines, guides, the grid — is off during play.</summary>
    public bool Editing { get; init; }

    /// <summary>
    /// Controls a contact is holding right now.
    /// </summary>
    /// <remarks>
    /// The single most important thing this surface can draw. A physical button
    /// tells your thumb it went down; glass tells you nothing at all, so without
    /// this a player cannot distinguish "I missed the button" from "the game
    /// ignored me" — and both feel like the adapter is broken.
    /// </remarks>
    public IReadOnlySet<string> Pressed { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    /// <summary>
    /// Controls held by a LATCH rather than by a finger.
    /// </summary>
    /// <remarks>
    /// Drawn differently from an ordinary press on purpose. A latch outlives the
    /// contact that made it, so "held" and "held by nothing you are touching" are
    /// different facts, and a player who cannot tell them apart has no way to
    /// discover why their character keeps walking.
    /// </remarks>
    public IReadOnlySet<string> Latched { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    /// <summary>Controls one dwell away from latching. Shown so the gesture is discoverable.</summary>
    public IReadOnlySet<string> Armed { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    /// <summary>Analog trigger travel, 0..1, by control id.</summary>
    public IReadOnlyDictionary<string, float> AnalogTriggers { get; init; } =
        new Dictionary<string, float>(StringComparer.Ordinal);

    /// <summary>
    /// Which way each analog trigger's fill grows, by control id.
    /// </summary>
    /// <remarks>
    /// Taken from the engine rather than derived from the layout, because while a
    /// gesture is live this is the axis FROZEN at pointer-down. A renderer
    /// recomputing it every frame would agree with the engine right up until the
    /// moment the fill and the published level could differ, which is the only
    /// moment this drawing has to be right.
    /// </remarks>
    public IReadOnlyDictionary<string, TouchFillDirection> AnalogTriggerFills { get; init; } =
        new Dictionary<string, TouchFillDirection>(StringComparer.Ordinal);

    /// <summary>
    /// Where each stick is pushed, <c>-1..1</c> in screen axes, by control id.
    /// </summary>
    /// <remarks>
    /// Normalized rather than in canvas units, because the renderer owns the knob
    /// size and therefore owns how far the knob may travel. Handing it pixels
    /// would put that arithmetic in the caller, where it would have to be kept in
    /// step with a constant the caller cannot see.
    ///
    /// A stick that does not move under the thumb is the clearest possible signal
    /// that a surface is a picture rather than a controller.
    /// </remarks>
    public IReadOnlyDictionary<string, (double X, double Y)> StickOffsets { get; init; } =
        new Dictionary<string, (double, double)>(StringComparer.Ordinal);

    /// <summary>
    /// Which D-pad directions are lit.
    /// </summary>
    /// <remarks>
    /// One state for the surface rather than a per-control entry in
    /// <see cref="Pressed"/>: a D-pad is ONE control with eight outcomes, and
    /// "the D-pad is held" is not a picture a thumb can navigate by. This is the
    /// published state, so a diagonal shows as a diagonal and a contact inside
    /// the dead zone shows as nothing — which is exactly what the console was
    /// told.
    /// </remarks>
    public DpadState Dpad { get; init; } = DpadState.None;
}

/// <summary>
/// Draws a <see cref="ResolvedTouchLayout"/>.
///
/// ## It decides nothing
///
/// Every position, size, rotation and shape arrives already computed. The renderer picks
/// colours and turns a contour into a path, and that is the whole of its authority — which
/// is what keeps the drawn control and the answerable control the same object. The
/// GameCube silhouettes come from <see cref="TouchGameCubeGeometry"/> for exactly this
/// reason: the beans wrap around A, and a per-platform drawing guess would put the
/// artwork somewhere the router does not agree with.
///
/// ## Why the numbers here are shared with Android and not with the shell
///
/// Every fraction and colour below is the value the Android surface draws with. The
/// on-screen controller is ARTWORK, not shell chrome: it is a picture of a physical
/// object, drawn over an opaque ground it paints itself, and someone with both
/// companions open should be looking at the same controller. Deriving these from the
/// host theme instead — which is what this file used to do — made one product look like
/// two, and more concretely produced a white D-pad and a white stick knob, because the
/// nearest Fluent brush to "the mark inside a control" is the primary TEXT brush. Fluent
/// resources still supply the EDITOR's colours, which are shell semantics (see
/// <see cref="Palette"/>).
///
/// ## Why visuals are reused rather than rebuilt
///
/// A drag repaints on every pointer event (§29: event-driven, never a frame clock), so
/// rebuilding thirty <c>UIElement</c>s per move would allocate its way through a gesture.
/// Visuals are keyed by instance id and updated in place; only a control that appeared or
/// disappeared costs an allocation.
/// </summary>
public sealed class TouchControlRenderer(Canvas canvas)
{
    /// <summary>
    /// Where an analog trigger starts reading as held.
    ///
    /// Half travel, matching the Android surface. Lighting it at the first
    /// millimetre would promise an input the console has not been sent.
    /// </summary>
    internal const float AnalogTriggerFlip = 0.5f;

    /// <summary>Every control's outline, in canvas units. Android's <c>OUTLINE_WIDTH</c>.</summary>
    internal const double OutlineWidth = 2d;

    /// <summary>A rounded control's corner, as a fraction of its half-height.</summary>
    internal const double CornerFraction = 0.45d;

    /// <summary>The knob, as a fraction of the stick's tracking radius.</summary>
    internal const double StickKnobFraction = 0.46d;

    /// <summary>A D-pad arm's length, as a fraction of the tracking radius.</summary>
    internal const double DpadArmFraction = 0.90d;

    /// <summary>A D-pad arm's half-width, as a fraction of the tracking radius.</summary>
    internal const double DpadArmHalfWidth = 0.26d;

    /// <summary>A D-pad arm's rounded tip, as a fraction of the arm's half-width.</summary>
    internal const double DpadCornerFraction = 0.55d;

    /// <summary>
    /// The recess a stick or D-pad sits in, relative to a button's face.
    /// </summary>
    /// <remarks>
    /// A well is a hole, not a control. Drawing it at full strength makes the
    /// biggest, least pressable objects on the surface the loudest ones, and
    /// leaves the knob inside nothing to stand out against.
    /// </remarks>
    internal const double WellAlpha = 0.55d;

    /// <summary>The legend's size before it is shrunk to fit, matching Android's 24sp.</summary>
    internal const double LegendBaseSize = 24d;

    /// <summary>How much of a control a legend may occupy before it is shrunk.</summary>
    internal const double LabelWidthFraction = 0.78d;

    internal const double LabelHeightFraction = 0.68d;

    // Glyph proportions, all fractions of the smaller face dimension, so a utility
    // keeps its mark whatever size the user has dragged it to.
    private const double CaptureDiscFraction = 0.28d;
    private const double CaptureFillAlpha = 0.18d;
    private const double CaptureRimAlpha = 0.82d;
    private const double CaptureStrokeFraction = 0.045d;
    private const double HomeCircleFraction = 0.30d;
    private const double HomeStrokeFraction = 0.05d;
    private const double HomeHouseFraction = 0.18d;

    private readonly Dictionary<string, ControlVisual> visuals = new(StringComparer.Ordinal);
    private readonly List<Line> lines = [];

    public void Draw(ResolvedTouchLayout resolved, TouchRenderOptions options)
    {
        DrawLines(resolved, options);

        var live = new HashSet<string>(StringComparer.Ordinal);

        // Draw order is the layout's own ZIndex, so the control drawn on top is the
        // control a thumb lands on — the router reads the same number.
        foreach (var control in resolved.Controls.OrderBy(control => control.Spec.ZIndex))
        {
            live.Add(control.Id);

            if (!visuals.TryGetValue(control.Id, out var visual))
            {
                visual = ControlVisual.Create(canvas);
                visuals[control.Id] = visual;
            }

            visual.Update(control, options);
        }

        foreach (var stale in visuals.Keys.Where(id => !live.Contains(id)).ToList())
        {
            visuals[stale].Remove(canvas);
            visuals.Remove(stale);
        }
    }

    public void Clear()
    {
        foreach (var visual in visuals.Values)
        {
            visual.Remove(canvas);
        }

        visuals.Clear();

        foreach (var line in lines)
        {
            canvas.Children.Remove(line);
        }

        lines.Clear();
    }

    /// <summary>
    /// The grid and the matched guides, beneath everything else.
    ///
    /// Guides are drawn only while editing: a guide that appears during play is decoration
    /// over a control the user is trying to hit.
    /// </summary>
    private void DrawLines(ResolvedTouchLayout resolved, TouchRenderOptions options)
    {
        var wanted = options.Editing
            ? options.Grid.Select(line => (line, guide: false))
                .Concat(options.Guides.Select(line => (line, guide: true)))
                .ToList()
            : [];

        while (lines.Count < wanted.Count)
        {
            var line = new Line { StrokeThickness = 1 };
            lines.Add(line);
            canvas.Children.Add(line);
        }

        for (var index = 0; index < lines.Count; index++)
        {
            var line = lines[index];
            if (index >= wanted.Count)
            {
                line.Visibility = Visibility.Collapsed;
                continue;
            }

            var (guide, isMatch) = wanted[index];
            var region = resolved.Region;

            line.Visibility = Visibility.Visible;
            line.X1 = guide.Vertical ? guide.Position : region.Left;
            line.X2 = guide.Vertical ? guide.Position : region.Right;
            line.Y1 = guide.Vertical ? region.Top : guide.Position;
            line.Y2 = guide.Vertical ? region.Bottom : guide.Position;
            line.Stroke = isMatch ? Palette.Guide : Palette.Grid;
            line.StrokeThickness = isMatch ? 1.5 : 1;
            Canvas.SetZIndex(line, -1);
        }
    }

    /// <summary>
    /// One control's elements, kept together so they move as one.
    /// </summary>
    /// <remarks>
    /// Four elements, split by PAINT rather than by meaning: WinUI gives one shape
    /// one fill and one stroke, so anything needing a second fill needs a second
    /// element. The roles that need them are mutually exclusive — a D-pad has no
    /// glyph, a glyph control has no lit arms — so the same two extra paths serve
    /// every control and nothing allocates per role.
    /// </remarks>
    private sealed class ControlVisual
    {
        /// <summary>The silhouette: fill plus outline.</summary>
        private Path Body { get; init; } = null!;

        /// <summary>
        /// The mark inside the body that also wants an outline: a D-pad's idle
        /// cross, a stick's knob, a utility's glyph ring, a trigger's travel fill.
        /// </summary>
        private Path Detail { get; init; } = null!;

        /// <summary>A second, unoutlined fill: a D-pad's lit arms, or Home's house.</summary>
        private Path Overlay { get; init; } = null!;

        private TextBlock Legend { get; init; } = null!;

        /// <summary>
        /// What the legend was last fitted for.
        /// </summary>
        /// <remarks>
        /// Fitting means measuring, and measuring text is the one genuinely
        /// expensive thing in this loop. The fitted size depends only on the text
        /// and the box it sits in, neither of which changes while a thumb moves, so
        /// it is recomputed on a layout change and not on a press.
        /// </remarks>
        private (string Text, double Width, double Height) LegendFit { get; set; }

        private double LegendSize { get; set; }

        public static ControlVisual Create(Canvas canvas)
        {
            var visual = new ControlVisual
            {
                Body = new Path(),
                Detail = new Path { IsHitTestVisible = false },
                Overlay = new Path { IsHitTestVisible = false },
                Legend = new TextBlock
                {
                    IsHitTestVisible = false,
                    TextAlignment = TextAlignment.Center,
                    FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                    FontFamily = Palette.TextFont,
                },
            };

            canvas.Children.Add(visual.Body);
            canvas.Children.Add(visual.Detail);
            canvas.Children.Add(visual.Overlay);
            canvas.Children.Add(visual.Legend);
            return visual;
        }

        public void Remove(Canvas canvas)
        {
            canvas.Children.Remove(Body);
            canvas.Children.Remove(Detail);
            canvas.Children.Remove(Overlay);
            canvas.Children.Remove(Legend);
        }

        public void Update(ResolvedTouchControl control, TouchRenderOptions options)
        {
            var selected = options.Editing && options.Selection.Contains(control.Id);
            var invalid = options.Invalid.Contains(control.Id);

            // Gameplay state. Latched OUTRANKS pressed: a latched control is
            // usually also under a finger, and the fact worth showing is the one
            // that will still be true after the finger lifts.
            var latched = options.Latched.Contains(control.Id);
            var pressed = latched || options.Pressed.Contains(control.Id);
            var armed = !latched && options.Armed.Contains(control.Id);
            var trigger = options.AnalogTriggers.TryGetValue(control.Id, out var fill);
            var travel = trigger ? Math.Clamp(fill, 0f, 1f) : 0f;

            // A trigger reads as held from the point it would actually fire, not
            // from the first millimetre of travel: lighting it earlier would
            // promise an input the console has not been sent.
            if (travel >= AnalogTriggerFlip)
            {
                pressed = true;
            }

            var role = control.Spec.VisualRole;
            var well = role is TouchVisualRole.AnalogStick or TouchVisualRole.UnifiedDpad;

            // A well is a recess; the thing IN it is what lights up. Filling the
            // ring on a press would flash the largest object on screen for an input
            // the player made with a knob a third of its size.
            var bodyLit = pressed && !well;

            Body.Data = BodyGeometry(control);
            Body.Fill = invalid ? Palette.InvalidFill
                : bodyLit ? Palette.Pressed
                : well ? Palette.Well
                : Palette.Idle;
            Body.Stroke = invalid ? Palette.InvalidStroke
                : selected ? Palette.Selection
                : latched ? Palette.LatchedOutline
                : bodyLit ? Palette.PressedOutline
                : armed ? Palette.ArmedOutline
                : Palette.IdleOutline;

            // Shape first, colour second. Latched and pressed cannot be a colour
            // distinction alone -- a player mid-game is not comparing swatches --
            // so a latch also thickens the outline.
            Body.StrokeThickness = selected || invalid || latched ? 4
                : bodyLit || armed ? 3
                : OutlineWidth;
            Canvas.SetZIndex(Body, control.Spec.ZIndex);

            UpdateDetail(control, options, role, trigger, travel, pressed, latched, armed);
            UpdateLegend(control, pressed);
        }

        /// <summary>The mark inside the body, and the second fill some marks need.</summary>
        private void UpdateDetail(
            ResolvedTouchControl control,
            TouchRenderOptions options,
            TouchVisualRole role,
            bool trigger,
            float travel,
            bool pressed,
            bool latched,
            bool armed)
        {
            Detail.Clip = null;
            Detail.Stroke = null;
            Detail.StrokeThickness = OutlineWidth;
            Overlay.Data = null;
            Overlay.Visibility = Visibility.Collapsed;

            if (trigger)
            {
                // The travelled portion of the pad, clipped to its own silhouette so
                // a partial pull keeps the control's outline instead of growing a
                // squared-off edge inside it.
                Detail.Data = travel > 0f ? BodyGeometry(control) : null;
                Detail.Fill = Palette.Pressed;
                Detail.Clip = TravelClip(control, options, travel);
            }
            else if (role == TouchVisualRole.UnifiedDpad)
            {
                Detail.Data = DpadCross(control);
                Detail.Fill = Palette.Idle;

                var lit = DpadLit(control, options.Dpad);
                Overlay.Data = lit;
                Overlay.Fill = Palette.Pressed;
                Overlay.Visibility = lit is null ? Visibility.Collapsed : Visibility.Visible;
            }
            else if (role == TouchVisualRole.AnalogStick)
            {
                Detail.Data = Knob(control, options);
                Detail.Fill = pressed ? Palette.Pressed : Palette.Idle;
                Detail.Stroke = latched ? Palette.LatchedOutline
                    : pressed ? Palette.PressedOutline
                    : armed ? Palette.ArmedOutline
                    : Palette.IdleOutline;
            }
            else if (control.Spec.Glyph is { } glyph)
            {
                var ink = pressed ? Palette.PressedLegend : Palette.Legend;
                var available = Math.Min(control.HalfWidth, control.HalfHeight) * 2;

                if (glyph == TouchControlGlyph.Capture)
                {
                    // A broad, softly filled disc rather than a camera pictogram: it
                    // reads as the recessed mark on the physical button, and the
                    // faint fill is what stops it looking like a letter O.
                    Detail.Data = Circle(
                        control.CenterX, control.CenterY, available * CaptureDiscFraction);
                    Detail.Fill = Palette.Translucent(ink, CaptureFillAlpha);
                    Detail.Stroke = Palette.Translucent(ink, CaptureRimAlpha);
                    Detail.StrokeThickness = available * CaptureStrokeFraction;
                }
                else
                {
                    Detail.Data = Circle(
                        control.CenterX, control.CenterY, available * HomeCircleFraction);
                    Detail.Fill = null;
                    Detail.Stroke = ink;
                    Detail.StrokeThickness = available * HomeStrokeFraction;

                    Overlay.Data = House(control, available * HomeHouseFraction);
                    Overlay.Fill = ink;
                    Overlay.Visibility = Visibility.Visible;
                }
            }
            else
            {
                Detail.Data = null;
                Detail.Fill = null;
            }

            Detail.Visibility = Detail.Data is null ? Visibility.Collapsed : Visibility.Visible;
            Canvas.SetZIndex(Detail, control.Spec.ZIndex);
            Canvas.SetZIndex(Overlay, control.Spec.ZIndex);
        }

        /// <summary>
        /// The legend, sized to the control it sits in.
        /// </summary>
        /// <remarks>
        /// A fixed base size that only ever shrinks, which is Android's rule. Sizing
        /// from the control's radius instead — the previous behaviour — gave a wide
        /// shoulder pad and a small round button different type for no reason a
        /// reader can see, and left a long legend free to run into the outline.
        /// </remarks>
        private void UpdateLegend(ResolvedTouchControl control, bool pressed)
        {
            var legend = LegendFor(control.Spec);

            // A glyph IS the control's content; drawing the label as well would put
            // the word "Capture" through the middle of the disc.
            var silent = legend.Length == 0 || control.Spec.Glyph is not null;

            Legend.Visibility = silent ? Visibility.Collapsed : Visibility.Visible;
            Canvas.SetZIndex(Legend, control.Spec.ZIndex);

            if (silent)
            {
                return;
            }

            var width = control.HalfWidth * 2d;
            var height = control.HalfHeight * 2d;
            var fit = (legend, width, height);

            if (LegendFit != fit)
            {
                LegendFit = fit;
                LegendSize = FittedSize(legend, width, height);
            }

            Legend.Text = legend;
            Legend.Foreground = pressed ? Palette.PressedLegend : Palette.Legend;
            Legend.FontSize = LegendSize;
            Legend.LineHeight = LegendSize;
            Legend.Width = width;
            Canvas.SetLeft(Legend, control.CenterX - control.HalfWidth);
            Canvas.SetTop(Legend, control.CenterY - (LegendSize * 0.5));
        }

        /// <summary>
        /// The largest size at or below the base that leaves the control breathing room.
        /// </summary>
        /// <remarks>
        /// One corrective pass, because for a fixed typeface on one unwrapped line
        /// text dimensions scale linearly — the same reason Android measures twice
        /// and stops. Measured rather than estimated from a character count: these
        /// legends run from "-" to "ZR", and an advance-width guess that is wrong by
        /// a little is a legend touching the outline.
        /// </remarks>
        private double FittedSize(string text, double width, double height)
        {
            Legend.Text = text;
            Legend.FontSize = LegendBaseSize;
            Legend.LineHeight = LegendBaseSize;

            // Unconstrained, so an over-wide legend reports its true width instead
            // of wrapping -- which is what makes the shrink possible at all.
            Legend.Width = double.NaN;
            Legend.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));

            return LegendBaseSize * FitScale(
                Legend.DesiredSize.Width, Legend.DesiredSize.Height, width, height);
        }

        /// <summary>
        /// The silhouette, in canvas coordinates.
        ///
        /// Absolute rather than a shape plus a <c>Canvas.Left</c>: a rotated control's
        /// bounding box is not its position, and offsetting one by the other is how
        /// artwork drifts away from the region the router tests.
        /// </summary>
        private static Geometry BodyGeometry(ResolvedTouchControl control)
        {
            var spec = control.Spec;

            if (spec.Shape == TouchControlShape.GameCubeContour)
            {
                return Contour(control);
            }

            if (spec.Shape == TouchControlShape.Rectangle)
            {
                // Rounded, and by the control's own half-height, so a tall pad and a
                // squat one carry the same visual softness rather than the same
                // absolute radius.
                var rectangle = RoundedRect(
                    control.CenterX - control.HalfWidth,
                    control.CenterY - control.HalfHeight,
                    control.HalfWidth * 2,
                    control.HalfHeight * 2,
                    control.HalfHeight * CornerFraction);

                return Rotate(rectangle, control, spec.VisualRotationDegrees);
            }

            var ellipse = new EllipseGeometry
            {
                Center = new Point(control.CenterX, control.CenterY),
                RadiusX = control.HalfWidth,
                RadiusY = control.HalfHeight,
            };

            // A round control is rotation invariant; turning it would only cost a
            // transform that can never be observed.
            return control.HalfWidth == control.HalfHeight
                ? ellipse
                : Rotate(ellipse, control, spec.VisualRotationDegrees);
        }

        /// <summary>
        /// The travelled portion of an analog trigger, as a clip on its own body.
        /// </summary>
        /// <remarks>
        /// A clip rather than a second, shorter geometry: the fill has to be the
        /// control's real silhouette, and building a partial contour would mean
        /// this file re-deriving a shape the resolver already owns.
        ///
        /// Full travel clips nothing at all. The rectangle is axis-aligned while a
        /// control may be rotated, so at 1.0 a computed box could fall just short of
        /// the corners and draw a trigger that is fully pulled as very nearly fully
        /// pulled -- the one reading that must never be ambiguous.
        /// </remarks>
        private static RectangleGeometry? TravelClip(
            ResolvedTouchControl control, TouchRenderOptions options, float travel)
        {
            if (travel >= 1f)
            {
                return null;
            }

            var direction = options.AnalogTriggerFills.TryGetValue(control.Id, out var value)
                ? value
                : TouchFillDirection.Down;

            // Generous on the axis the fill does NOT grow along: a rotated body
            // reaches outside its own half-extents, and clipping it there would
            // narrow the fill for a reason the travel did not ask for.
            var spanX = control.HitExtentX;
            var spanY = control.HitExtentY;
            var left = control.CenterX - spanX;
            var top = control.CenterY - spanY;
            var width = spanX * 2;
            var height = spanY * 2;

            var rect = direction switch
            {
                TouchFillDirection.Up =>
                    new Rect(left, top + (height * (1 - travel)), width, height * travel),
                TouchFillDirection.Left =>
                    new Rect(left + (width * (1 - travel)), top, width * travel, height),
                TouchFillDirection.Right =>
                    new Rect(left, top, width * travel, height),
                _ => new Rect(left, top, width, height * travel),
            };

            return new RectangleGeometry { Rect = rect };
        }

        /// <summary>
        /// The stick's knob, AT ITS CURRENT DEFLECTION.
        /// </summary>
        /// <remarks>
        /// Scaled by the travel the knob actually has — the well's radius less the
        /// knob's own — rather than by the well's radius and then clamped. Both put
        /// full deflection on the rim, but scaling by the larger number and clamping
        /// makes the knob reach the rim early and then sit still through the last
        /// third of the stick's range, which is the part of a turn a player is
        /// actually reading.
        /// </remarks>
        private static Geometry Knob(ResolvedTouchControl control, TouchRenderOptions options)
        {
            var radius = control.TrackingRadius * StickKnobFraction;
            var travel = Math.Max(0d, control.TrackingRadius - radius);

            var (dx, dy) = options.StickOffsets.TryGetValue(control.Id, out var offset)
                ? offset
                : (0d, 0d);

            return Circle(
                control.CenterX + (dx * travel),
                control.CenterY + (dy * travel),
                radius);
        }

        /// <summary>
        /// The D-pad's body: one cross, not four wedges radiating from a hub.
        /// </summary>
        /// <remarks>
        /// The fill rule is the whole point of this method. Two overlapping bars
        /// under WinUI's DEFAULT <c>EvenOdd</c> cancel where they cross, punching a
        /// square hole through the middle of the D-pad — which is what this surface
        /// shipped, and it read as a checkerboard rather than as a control.
        /// <c>Nonzero</c> unions them.
        /// </remarks>
        private static Geometry DpadCross(ResolvedTouchControl control)
        {
            var arm = control.TrackingRadius * DpadArmFraction;
            var half = control.TrackingRadius * DpadArmHalfWidth;
            var corner = half * DpadCornerFraction;

            var group = new GeometryGroup { FillRule = FillRule.Nonzero };
            group.Children.Add(RoundedRect(
                control.CenterX - arm, control.CenterY - half, arm * 2, half * 2, corner));
            group.Children.Add(RoundedRect(
                control.CenterX - half, control.CenterY - arm, half * 2, arm * 2, corner));
            return group;
        }

        /// <summary>
        /// The lit arms, as one region.
        /// </summary>
        /// <remarks>
        /// Each direction is an arm carrying the body's own rounded tip, tapering to
        /// a POINT at the exact centre. Two consequences, both deliberate:
        ///
        /// <list type="bullet">
        /// <item>a lit arm ends where the body ends, so no square corner overhangs
        /// the rounded tip;</item>
        /// <item>adjacent directions share their whole hub edge, so a diagonal is
        /// one continuous region rather than two shapes with a seam between them.</item>
        /// </list>
        ///
        /// Built as explicit figures rather than by intersecting a wedge with the
        /// body, which is what Android does: WinUI has no path booleans, and the
        /// arithmetic to place a rounded tip is smaller than the arithmetic to fake
        /// an intersection.
        /// </remarks>
        private static Geometry? DpadLit(ResolvedTouchControl control, DpadState state)
        {
            if (!state.Up && !state.Down && !state.Left && !state.Right)
            {
                return null;
            }

            var arm = control.TrackingRadius * DpadArmFraction;
            var half = control.TrackingRadius * DpadArmHalfWidth;
            var corner = half * DpadCornerFraction;
            var geometry = new PathGeometry { FillRule = FillRule.Nonzero };
            var cx = (double)control.CenterX;
            var cy = (double)control.CenterY;

            // One arm, described once pointing north, then mapped into each
            // direction's frame. All four maps are rotations, so the arc sweep is
            // the same in every one of them.
            void Arm(Func<double, double, Point> map)
            {
                var figure = new PathFigure
                {
                    IsClosed = true,
                    IsFilled = true,
                    StartPoint = map(-half, -half),
                };

                figure.Segments.Add(new LineSegment { Point = map(-half, -(arm - corner)) });
                figure.Segments.Add(new ArcSegment
                {
                    Point = map(-(half - corner), -arm),
                    Size = new Size(corner, corner),
                    SweepDirection = SweepDirection.Clockwise,
                });
                figure.Segments.Add(new LineSegment { Point = map(half - corner, -arm) });
                figure.Segments.Add(new ArcSegment
                {
                    Point = map(half, -(arm - corner)),
                    Size = new Size(corner, corner),
                    SweepDirection = SweepDirection.Clockwise,
                });
                figure.Segments.Add(new LineSegment { Point = map(half, -half) });
                figure.Segments.Add(new LineSegment { Point = map(0, 0) });

                geometry.Figures.Add(figure);
            }

            if (state.Up)
            {
                Arm((x, y) => new Point(cx + x, cy + y));
            }

            if (state.Down)
            {
                Arm((x, y) => new Point(cx - x, cy - y));
            }

            if (state.Left)
            {
                Arm((x, y) => new Point(cx + y, cy - x));
            }

            if (state.Right)
            {
                Arm((x, y) => new Point(cx - y, cy + x));
            }

            return geometry;
        }

        /// <summary>Home's house, drawn rather than borrowed from a symbol font.</summary>
        /// <remarks>
        /// The same unit polygon the Android surface uses. A font glyph would be
        /// whatever Segoe Fluent Icons happens to draw at that code point — a
        /// different house at a different weight from the one on the other
        /// companion, and on a machine missing the font, a hollow box.
        /// </remarks>
        private static Geometry House(ResolvedTouchControl control, double unit)
        {
            var cx = (double)control.CenterX;
            var cy = (double)control.CenterY;

            Point At(double x, double y) => new(cx + (unit * x), cy + (unit * y));

            var figure = new PathFigure
            {
                IsClosed = true,
                IsFilled = true,
                StartPoint = At(0, -1),
            };

            foreach (var (x, y) in new (double X, double Y)[]
            {
                (-1, -0.12), (-0.72, -0.12), (-0.72, 0.78), (-0.25, 0.78),
                (-0.25, 0.20), (0.25, 0.20), (0.25, 0.78), (0.72, 0.78),
                (0.72, -0.12), (1, -0.12),
            })
            {
                figure.Segments.Add(new LineSegment { Point = At(x, y) });
            }

            var geometry = new PathGeometry();
            geometry.Figures.Add(figure);
            return geometry;
        }

        /// <summary>
        /// A rectangle with rounded corners.
        /// </summary>
        /// <remarks>
        /// Built by hand because WinUI's <see cref="RectangleGeometry"/>, unlike
        /// WPF's, has no corner radius at all -- the rounding on a XAML Rectangle
        /// lives on the SHAPE, not on its geometry, and this renderer needs the
        /// geometry so it can put the same silhouette through a GeometryGroup, a
        /// clip and a rotation.
        ///
        /// The radius is clamped to half the shorter side, so a control the user
        /// has dragged narrower than its own corners degrades to a stadium instead
        /// of folding its arcs back through itself.
        /// </remarks>
        private static PathGeometry RoundedRect(
            double left, double top, double width, double height, double radius)
        {
            var r = Math.Max(0d, Math.Min(radius, Math.Min(width, height) / 2d));
            var right = left + width;
            var bottom = top + height;
            var size = new Size(r, r);

            var figure = new PathFigure
            {
                IsClosed = true,
                IsFilled = true,
                StartPoint = new Point(left + r, top),
            };

            void Corner(Point to)
            {
                figure.Segments.Add(new ArcSegment
                {
                    Point = to,
                    Size = size,
                    SweepDirection = SweepDirection.Clockwise,
                });
            }

            figure.Segments.Add(new LineSegment { Point = new Point(right - r, top) });
            Corner(new Point(right, top + r));
            figure.Segments.Add(new LineSegment { Point = new Point(right, bottom - r) });
            Corner(new Point(right - r, bottom));
            figure.Segments.Add(new LineSegment { Point = new Point(left + r, bottom) });
            Corner(new Point(left, bottom - r));
            figure.Segments.Add(new LineSegment { Point = new Point(left, top + r) });
            Corner(new Point(left + r, top));

            var geometry = new PathGeometry();
            geometry.Figures.Add(figure);
            return geometry;
        }

        private static EllipseGeometry Circle(double x, double y, double radius) => new()
        {
            Center = new Point(x, y),
            RadiusX = radius,
            RadiusY = radius,
        };

        private static Geometry Contour(ResolvedTouchControl control)
        {
            var points = TouchGameCubeGeometry.OrientedContour(
                control.Spec.VisualRole,
                control.HalfWidth * 2,
                control.HalfHeight * 2,
                control.Spec.VisualRotationDegrees);

            var figure = new PathFigure
            {
                IsClosed = true,
                IsFilled = true,
                StartPoint = new Point(
                    control.CenterX + points[0].X, control.CenterY + points[0].Y),
            };

            foreach (var point in points.Skip(1))
            {
                figure.Segments.Add(new LineSegment
                {
                    Point = new Point(control.CenterX + point.X, control.CenterY + point.Y),
                });
            }

            var geometry = new PathGeometry();
            geometry.Figures.Add(figure);
            return geometry;
        }

        private static Geometry Rotate(Geometry geometry, ResolvedTouchControl control, float degrees)
        {
            if (MathF.Abs(degrees) < 0.01f)
            {
                return geometry;
            }

            geometry.Transform = new RotateTransform
            {
                Angle = degrees,
                CenterX = control.CenterX,
                CenterY = control.CenterY,
            };
            return geometry;
        }

        /// <summary>
        /// What is written on the control.
        ///
        /// A face button's letter is NOT in its label: the template stores a POSITION, and
        /// which letter that position carries is a property of the controller being drawn.
        /// Resolving it through <see cref="ControllerLayoutResolver.FaceLabel"/> — the same
        /// call the Android renderer makes, and the same one <see cref="TouchControlNaming"/>
        /// makes for the editor's own text — is what keeps a touch slot sending the letter
        /// it draws (I13). Reading <c>spec.Label</c> alone left every face button blank.
        /// </summary>
        private static string LegendFor(TouchControlSpec spec) =>
            spec.Action is TouchControlAction.Face face
                ? ControllerLayoutResolver.FaceLabel(face.Position, TouchControlNaming.FaceLayout)
                : spec.Label;
    }

    /// <summary>
    /// A downscale factor that leaves deliberate breathing room around a legend.
    /// </summary>
    /// <remarks>
    /// Never above 1: the base size is the visual target, and this only handles
    /// unusually long text, small controls and large accessibility font scales.
    /// Internal rather than private so the ratio itself can be tested without a
    /// WinUI text stack, which is not available to a unit test.
    /// </remarks>
    internal static double FitScale(
        double measuredWidth, double measuredHeight, double availableWidth, double availableHeight)
    {
        var widthRoom = Math.Max(availableWidth * LabelWidthFraction, 1d);
        var heightRoom = Math.Max(availableHeight * LabelHeightFraction, 1d);

        return Math.Min(
            1d,
            Math.Min(
                widthRoom / Math.Max(measuredWidth, 1d),
                heightRoom / Math.Max(measuredHeight, 1d)));
    }

    /// <summary>
    /// The controller's colours.
    /// </summary>
    /// <remarks>
    /// Split deliberately in two.
    ///
    /// The CONTROLLER's own colours are fixed design tokens, the same values the
    /// Android surface resolves from its theme. They are artwork: the surface
    /// paints its own opaque near-black ground on both platforms, so there is no
    /// host background for them to adapt to, and a Fluent brush that follows the
    /// system theme would go pale over a ground that never does. Reading them from
    /// the theme is also what produced a white D-pad and a white stick knob — the
    /// closest Fluent resource to "the mark inside a control" is the primary TEXT
    /// brush, and it is white by design.
    ///
    /// The EDITOR's colours still come from the theme, because selection, guides
    /// and audit errors are shell semantics and should look like the rest of the
    /// app saying the same things.
    /// </remarks>
    private static class Palette
    {
        /// <summary>A control's face. Android's <c>surfaceVariant</c>.</summary>
        public static Brush Idle { get; } = Solid(0xFF, 0x24, 0x2A, 0x35);

        /// <summary>Every control's edge. Android's <c>outline</c>.</summary>
        public static Brush IdleOutline { get; } = Solid(0xFF, 0x87, 0x91, 0x9D);

        /// <summary>A stick or D-pad recess: the face, stepped back.</summary>
        public static Brush Well { get; } = Solid((byte)(0xFF * WellAlpha), 0x24, 0x2A, 0x35);

        /// <summary>A held control. Android's <c>primary</c> under the default accent.</summary>
        public static Brush Pressed { get; } = Solid(0xFF, 0x77, 0xD8, 0xFF);

        public static Brush PressedOutline { get; } = Solid(0xFF, 0xBF, 0xEA, 0xFF);

        /// <summary>The legend on an idle control. Android's <c>onSurface</c>.</summary>
        public static Brush Legend { get; } = Solid(0xFF, 0xE2, 0xE2, 0xE9);

        /// <summary>
        /// The legend over a held control. Contrast against the accent fill, not
        /// against the page: a label that stays legible on a filled button is the
        /// one thing a player looks at when they cannot tell if it registered.
        /// </summary>
        public static Brush PressedLegend { get; } = Solid(0xFF, 0x00, 0x35, 0x48);

        /// <summary>A latch outlives the finger, so it gets its own colour AND a thicker stroke.</summary>
        public static Brush LatchedOutline =>
            Resource("SystemFillColorCautionBrush", Microsoft.UI.Colors.Orange);

        /// <summary>Armed: the dwell landed and a slide would latch. Discoverability, not state.</summary>
        public static Brush ArmedOutline =>
            Resource("SystemFillColorSuccessBrush", Microsoft.UI.Colors.MediumSeaGreen);

        public static Brush Selection =>
            Resource("AccentFillColorDefaultBrush", Microsoft.UI.Colors.DodgerBlue);

        public static Brush Guide =>
            Resource("AccentFillColorSecondaryBrush", Microsoft.UI.Colors.DeepSkyBlue);

        public static Brush Grid { get; } = Solid(0x28, 0x87, 0x91, 0x9D);

        public static Brush InvalidStroke =>
            Resource("SystemFillColorCriticalBrush", Microsoft.UI.Colors.OrangeRed);

        public static Brush InvalidFill =>
            Resource("SystemFillColorCriticalBackgroundBrush", Microsoft.UI.Colors.DarkRed);

        public static FontFamily TextFont { get; } = new("Segoe UI");

        /// <summary>
        /// The same ink at a lower opacity.
        /// </summary>
        /// <remarks>
        /// Allocated per call rather than cached, and only for the two utility
        /// glyphs: a per-alpha cache would be two entries and a dictionary lookup
        /// to save two objects on controls that change at most once a frame.
        /// </remarks>
        public static Brush Translucent(Brush source, double alpha)
        {
            if (source is not SolidColorBrush solid)
            {
                return source;
            }

            var colour = solid.Color;
            return new SolidColorBrush(Color.FromArgb(
                (byte)Math.Clamp(colour.A * alpha, 0, 255), colour.R, colour.G, colour.B));
        }

        private static Brush Solid(byte a, byte r, byte g, byte b) =>
            new SolidColorBrush(Color.FromArgb(a, r, g, b));

        private static Brush Resource(string key, Color fallback) =>
            Application.Current.Resources.TryGetValue(key, out var value) && value is Brush brush
                ? brush
                : new SolidColorBrush(fallback);
    }
}
