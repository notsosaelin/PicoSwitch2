using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Media;
using Avalonia.Styling;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Touch;

// Avalonia.Controls.Shapes.Path is the drawing primitive this file is about; the
// implicit System.IO using would otherwise make the name ambiguous.
using Path = Avalonia.Controls.Shapes.Path;

namespace PicoSwitch.Companion.Linux.App.Touch;

/// <summary>What to draw on top of the controls this pass.</summary>
/// <remarks>
/// The same record as the WinUI renderer's
/// (<c>companions/windows/src/PicoSwitch.Companion.App/Touch/TouchControlRenderer.cs</c>),
/// field for field. Every field is asked of the shared engine, never derived
/// from pointer events here; that file explains why each one exists.
/// </remarks>
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

    /// <summary>Controls a contact is holding right now. Glass gives a thumb no click; this is the click.</summary>
    public IReadOnlySet<string> Pressed { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    /// <summary>Controls held by a LATCH rather than by a finger, drawn differently on purpose.</summary>
    public IReadOnlySet<string> Latched { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    /// <summary>Controls one dwell away from latching. Shown so the gesture is discoverable.</summary>
    public IReadOnlySet<string> Armed { get; init; } =
        new HashSet<string>(StringComparer.Ordinal);

    /// <summary>Analog trigger travel, 0..1, by control id.</summary>
    public IReadOnlyDictionary<string, float> AnalogTriggers { get; init; } =
        new Dictionary<string, float>(StringComparer.Ordinal);

    /// <summary>Which way each analog trigger's fill grows: the axis the engine froze at pointer-down.</summary>
    public IReadOnlyDictionary<string, TouchFillDirection> AnalogTriggerFills { get; init; } =
        new Dictionary<string, TouchFillDirection>(StringComparer.Ordinal);

    /// <summary>Where each stick is pushed, <c>-1..1</c> in screen axes, by control id.</summary>
    public IReadOnlyDictionary<string, (double X, double Y)> StickOffsets { get; init; } =
        new Dictionary<string, (double, double)>(StringComparer.Ordinal);

    /// <summary>Which D-pad directions are lit: the published state, so a diagonal shows as one.</summary>
    public DpadState Dpad { get; init; } = DpadState.None;
}

/// <summary>
/// Draws a <see cref="ResolvedTouchLayout"/> on an Avalonia canvas.
///
/// A port of the WinUI renderer, which is itself drawn to the Android surface's
/// numbers: the on-screen controller is ARTWORK, a picture of a physical object
/// over an opaque ground it paints itself, so someone with two companions open
/// should be looking at the same controller. Every fraction and colour below is
/// the WinUI file's value; change them there and here together.
///
/// It decides nothing. Every position, size, rotation and shape arrives already
/// computed by the shared resolver, which is what keeps the drawn control and
/// the control a touch is routed to the same object.
///
/// Visuals are keyed by instance id and updated in place, so a drag or a press
/// repaints without allocating a new element per control.
/// </summary>
public sealed class TouchControlRenderer(Canvas canvas)
{
    /// <summary>Where an analog trigger starts reading as held: half travel, as on Android.</summary>
    internal const float AnalogTriggerFlip = 0.5f;

    /// <summary>Every control's outline. Android's <c>OUTLINE_WIDTH</c>.</summary>
    internal const double OutlineWidth = 2d;

    /// <summary>A rounded control's corner, as a fraction of its half-height.</summary>
    internal const double CornerFraction = 0.45d;

    /// <summary>The knob, as a fraction of the stick's tracking radius.</summary>
    internal const double StickKnobFraction = 0.46d;

    internal const double DpadArmFraction = 0.90d;

    internal const double DpadArmHalfWidth = 0.26d;

    internal const double DpadCornerFraction = 0.55d;

    /// <summary>A stick or D-pad recess, relative to a button's face. A well is a hole, not a control.</summary>
    internal const double WellAlpha = 0.55d;

    /// <summary>The legend's size before it is shrunk to fit, matching Android's 24sp.</summary>
    internal const double LegendBaseSize = 24d;

    internal const double LabelWidthFraction = 0.78d;

    internal const double LabelHeightFraction = 0.68d;

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

    /// <summary>The grid and the matched guides, beneath everything else, and only while editing.</summary>
    private void DrawLines(ResolvedTouchLayout resolved, TouchRenderOptions options)
    {
        var wanted = options.Editing
            ? options.Grid.Select(line => (line, guide: false))
                .Concat(options.Guides.Select(line => (line, guide: true)))
                .ToList()
            : [];

        while (lines.Count < wanted.Count)
        {
            var line = new Line { StrokeThickness = 1, IsHitTestVisible = false, ZIndex = -1 };
            lines.Add(line);
            canvas.Children.Add(line);
        }

        for (var index = 0; index < lines.Count; index++)
        {
            var line = lines[index];
            if (index >= wanted.Count)
            {
                line.IsVisible = false;
                continue;
            }

            var (guide, isMatch) = wanted[index];
            var region = resolved.Region;

            line.IsVisible = true;
            line.StartPoint = guide.Vertical
                ? new Point(guide.Position, region.Top)
                : new Point(region.Left, guide.Position);
            line.EndPoint = guide.Vertical
                ? new Point(guide.Position, region.Bottom)
                : new Point(region.Right, guide.Position);
            line.Stroke = isMatch ? Palette.Guide : Palette.Grid;
            line.StrokeThickness = isMatch ? 1.5 : 1;
        }
    }

    /// <summary>
    /// One control's elements, kept together so they move as one: the body
    /// (fill plus outline), a detail mark that also wants an outline, a second
    /// unoutlined fill, and the legend. Split by paint, as in the WinUI file,
    /// because one shape has one fill and one stroke.
    /// </summary>
    private sealed class ControlVisual
    {
        private Path Body { get; init; } = null!;

        private Path Detail { get; init; } = null!;

        private Path Overlay { get; init; } = null!;

        private TextBlock Legend { get; init; } = null!;

        /// <summary>What the legend was last fitted for. Measuring text is the expensive part of a repaint.</summary>
        private (string Text, double Width, double Height) LegendFit { get; set; }

        private double LegendSize { get; set; }

        /// <summary>Half the legend's MEASURED height, which is what centres it (not half the font size).</summary>
        private double LegendHalfHeight { get; set; }

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
                    FontWeight = FontWeight.SemiBold,
                },
            };

            // The surface handles every pointer itself; a control's body must not
            // swallow a press before the canvas sees it.
            visual.Body.IsHitTestVisible = false;

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

            // Latched OUTRANKS pressed: the fact worth showing is the one that will
            // still be true after the finger lifts.
            var latched = options.Latched.Contains(control.Id);
            var pressed = latched || options.Pressed.Contains(control.Id);
            var armed = !latched && options.Armed.Contains(control.Id);
            var trigger = options.AnalogTriggers.TryGetValue(control.Id, out var fill);
            var travel = trigger ? Math.Clamp(fill, 0f, 1f) : 0f;

            if (travel >= AnalogTriggerFlip)
            {
                pressed = true;
            }

            var role = control.Spec.VisualRole;
            var well = role is TouchVisualRole.AnalogStick or TouchVisualRole.UnifiedDpad;

            // A well is a recess; the thing IN it is what lights up.
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

            // Latched and pressed cannot be a colour distinction alone, so a latch
            // also thickens the outline.
            Body.StrokeThickness = selected || invalid || latched ? 4
                : bodyLit || armed ? 3
                : OutlineWidth;
            Body.ZIndex = control.Spec.ZIndex;

            UpdateDetail(control, options, role, trigger, travel, pressed, latched, armed);
            UpdateLegend(control, pressed);
        }

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
            Overlay.IsVisible = false;

            if (trigger)
            {
                // The travelled portion, clipped to the control's own silhouette.
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
                Overlay.IsVisible = lit is not null;
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
                    // A broad, softly filled disc: the recessed mark on the physical
                    // button, and the faint fill stops it reading as a letter O.
                    Detail.Data = Circle(control.CenterX, control.CenterY, available * CaptureDiscFraction);
                    Detail.Fill = Palette.Translucent(ink, CaptureFillAlpha);
                    Detail.Stroke = Palette.Translucent(ink, CaptureRimAlpha);
                    Detail.StrokeThickness = available * CaptureStrokeFraction;
                }
                else
                {
                    Detail.Data = Circle(control.CenterX, control.CenterY, available * HomeCircleFraction);
                    Detail.Fill = null;
                    Detail.Stroke = ink;
                    Detail.StrokeThickness = available * HomeStrokeFraction;

                    Overlay.Data = House(control, available * HomeHouseFraction);
                    Overlay.Fill = ink;
                    Overlay.IsVisible = true;
                }
            }
            else
            {
                Detail.Data = null;
                Detail.Fill = null;
            }

            Detail.IsVisible = Detail.Data is not null;
            Detail.ZIndex = control.Spec.ZIndex;
            Overlay.ZIndex = control.Spec.ZIndex;
        }

        /// <summary>The legend: a fixed base size that only ever shrinks, which is Android's rule.</summary>
        private void UpdateLegend(ResolvedTouchControl control, bool pressed)
        {
            var legend = LegendFor(control.Spec);

            // A glyph IS the control's content.
            var silent = legend.Length == 0 || control.Spec.Glyph is not null;

            Legend.IsVisible = !silent;
            Legend.ZIndex = control.Spec.ZIndex;

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
                FitLegend(legend, width, height);
            }

            Legend.Text = legend;
            Legend.Foreground = pressed ? Palette.PressedLegend : Palette.Legend;
            Legend.FontSize = LegendSize;
            Legend.Width = width;

            Canvas.SetLeft(Legend, control.CenterX - control.HalfWidth);
            Canvas.SetTop(Legend, control.CenterY - LegendHalfHeight);
        }

        /// <summary>The largest size at or below the base that leaves breathing room, and the box it centres in.</summary>
        private void FitLegend(string text, double width, double height)
        {
            Size Measure(double size)
            {
                Legend.Text = text;
                Legend.FontSize = size;

                // Unconstrained, so an over-wide legend reports its true width
                // instead of wrapping.
                Legend.Width = double.NaN;
                Legend.Measure(Size.Infinity);
                return Legend.DesiredSize;
            }

            var natural = Measure(LegendBaseSize);
            var scale = FitScale(natural.Width, natural.Height, width, height);

            LegendSize = LegendBaseSize * scale;

            // One corrective pass: unwrapped single-line text scales linearly, and
            // the second measurement is what the legend is centred on.
            LegendHalfHeight = (scale < 1d ? Measure(LegendSize).Height : natural.Height) / 2d;
        }

        /// <summary>The silhouette, in canvas coordinates (absolute, so a rotated control cannot drift).</summary>
        private static Geometry BodyGeometry(ResolvedTouchControl control)
        {
            var spec = control.Spec;

            if (spec.Shape == TouchControlShape.GameCubeContour)
            {
                return Contour(control);
            }

            if (spec.Shape == TouchControlShape.Rectangle)
            {
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

            // A round control is rotation invariant.
            return control.HalfWidth == control.HalfHeight
                ? ellipse
                : Rotate(ellipse, control, spec.VisualRotationDegrees);
        }

        /// <summary>
        /// The travelled portion of an analog trigger, as a clip on its own body.
        /// Full travel clips nothing, so a fully pulled trigger can never read as
        /// very nearly pulled.
        /// </summary>
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

        /// <summary>The stick's knob at its current deflection, scaled by the travel the knob actually has.</summary>
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
        /// The D-pad's body: one cross. The <c>NonZero</c> fill rule is the whole
        /// point: two overlapping bars under even-odd cancel where they cross and
        /// punch a square hole through the middle (the WinUI surface shipped that).
        /// </summary>
        private static Geometry DpadCross(ResolvedTouchControl control)
        {
            var arm = control.TrackingRadius * DpadArmFraction;
            var half = control.TrackingRadius * DpadArmHalfWidth;
            var corner = half * DpadCornerFraction;

            var group = new GeometryGroup { FillRule = FillRule.NonZero };
            group.Children.Add(RoundedRect(
                control.CenterX - arm, control.CenterY - half, arm * 2, half * 2, corner));
            group.Children.Add(RoundedRect(
                control.CenterX - half, control.CenterY - arm, half * 2, arm * 2, corner));
            return group;
        }

        /// <summary>
        /// The lit arms, as one region: each arm carries the body's rounded tip
        /// and tapers to a point at the centre, so adjacent directions share an
        /// edge and a diagonal is one continuous region.
        /// </summary>
        private static Geometry? DpadLit(ResolvedTouchControl control, DpadState state)
        {
            if (!state.Up && !state.Down && !state.Left && !state.Right)
            {
                return null;
            }

            var arm = control.TrackingRadius * DpadArmFraction;
            var half = control.TrackingRadius * DpadArmHalfWidth;
            var corner = half * DpadCornerFraction;
            var geometry = new PathGeometry { FillRule = FillRule.NonZero, Figures = [] };
            var cx = (double)control.CenterX;
            var cy = (double)control.CenterY;

            // One arm, described pointing north, then rotated into each direction.
            void Arm(Func<double, double, Point> map)
            {
                var figure = Figure(map(-half, -half));
                figure.Segments!.Add(new LineSegment { Point = map(-half, -(arm - corner)) });
                figure.Segments.Add(Arc(map(-(half - corner), -arm), corner));
                figure.Segments.Add(new LineSegment { Point = map(half - corner, -arm) });
                figure.Segments.Add(Arc(map(half, -(arm - corner)), corner));
                figure.Segments.Add(new LineSegment { Point = map(half, -half) });
                figure.Segments.Add(new LineSegment { Point = map(0, 0) });
                geometry.Figures!.Add(figure);
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

        /// <summary>Home's house, drawn from the same unit polygon the Android and WinUI surfaces use.</summary>
        private static Geometry House(ResolvedTouchControl control, double unit)
        {
            var cx = (double)control.CenterX;
            var cy = (double)control.CenterY;

            Point At(double x, double y) => new(cx + (unit * x), cy + (unit * y));

            var figure = Figure(At(0, -1));
            foreach (var (x, y) in new (double X, double Y)[]
            {
                (-1, -0.12), (-0.72, -0.12), (-0.72, 0.78), (-0.25, 0.78),
                (-0.25, 0.20), (0.25, 0.20), (0.25, 0.78), (0.72, 0.78),
                (0.72, -0.12), (1, -0.12),
            })
            {
                figure.Segments!.Add(new LineSegment { Point = At(x, y) });
            }

            return new PathGeometry { Figures = [figure] };
        }

        /// <summary>
        /// A rectangle with rounded corners, as a path so the same silhouette can
        /// go through a group, a clip and a rotation. The radius is clamped to half
        /// the shorter side, so a narrow control degrades to a stadium.
        /// </summary>
        private static PathGeometry RoundedRect(
            double left, double top, double width, double height, double radius)
        {
            var r = Math.Max(0d, Math.Min(radius, Math.Min(width, height) / 2d));
            var right = left + width;
            var bottom = top + height;

            var figure = Figure(new Point(left + r, top));
            figure.Segments!.Add(new LineSegment { Point = new Point(right - r, top) });
            figure.Segments.Add(Arc(new Point(right, top + r), r));
            figure.Segments.Add(new LineSegment { Point = new Point(right, bottom - r) });
            figure.Segments.Add(Arc(new Point(right - r, bottom), r));
            figure.Segments.Add(new LineSegment { Point = new Point(left + r, bottom) });
            figure.Segments.Add(Arc(new Point(left, bottom - r), r));
            figure.Segments.Add(new LineSegment { Point = new Point(left, top + r) });
            figure.Segments.Add(Arc(new Point(left + r, top), r));

            return new PathGeometry { Figures = [figure] };
        }

        private static PathFigure Figure(Point start) => new()
        {
            IsClosed = true,
            IsFilled = true,
            StartPoint = start,
            Segments = [],
        };

        private static ArcSegment Arc(Point to, double radius) => new()
        {
            Point = to,
            Size = new Size(radius, radius),
            SweepDirection = SweepDirection.Clockwise,
        };

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

            var figure = Figure(new Point(control.CenterX + points[0].X, control.CenterY + points[0].Y));
            foreach (var point in points.Skip(1))
            {
                figure.Segments!.Add(new LineSegment
                {
                    Point = new Point(control.CenterX + point.X, control.CenterY + point.Y),
                });
            }

            return new PathGeometry { Figures = [figure] };
        }

        private static Geometry Rotate(Geometry geometry, ResolvedTouchControl control, float degrees)
        {
            if (MathF.Abs(degrees) < 0.01f)
            {
                return geometry;
            }

            // Rotation about the control's centre: translate it to the origin,
            // rotate, translate back. The same transform the WinUI renderer
            // expresses as a RotateTransform with a centre.
            geometry.Transform = new MatrixTransform(
                Matrix.CreateTranslation(-control.CenterX, -control.CenterY)
                * Matrix.CreateRotation(degrees * Math.PI / 180d)
                * Matrix.CreateTranslation(control.CenterX, control.CenterY));
            return geometry;
        }

        /// <summary>
        /// What is written on the control. A face button's letter comes from the
        /// controller being drawn (<see cref="ControllerLayoutResolver.FaceLabel"/>),
        /// never from the template, so a touch slot sends the letter it shows.
        /// </summary>
        private static string LegendFor(TouchControlSpec spec) =>
            spec.Action is TouchControlAction.Face face
                ? ControllerLayoutResolver.FaceLabel(face.Position, TouchControlNaming.FaceLayout)
                : spec.Label;
    }

    /// <summary>A downscale factor that leaves breathing room around a legend. Never above 1.</summary>
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
    /// The controller's colours: fixed artwork tokens, the WinUI file's values,
    /// which are the Android theme's. The surface paints its own near-black ground
    /// in both app themes, so these never follow the theme. The editor's colours
    /// (selection, guides, audit errors) are shell semantics and come from the
    /// app's own accent and tone colours.
    /// </summary>
    private static class Palette
    {
        public static IBrush Idle { get; } = Solid(0xFF, 0x24, 0x2A, 0x35);

        public static IBrush IdleOutline { get; } = Solid(0xFF, 0x87, 0x91, 0x9D);

        public static IBrush Well { get; } = Solid((byte)(0xFF * WellAlpha), 0x24, 0x2A, 0x35);

        public static IBrush Pressed { get; } = Solid(0xFF, 0x77, 0xD8, 0xFF);

        public static IBrush PressedOutline { get; } = Solid(0xFF, 0xBF, 0xEA, 0xFF);

        public static IBrush Legend { get; } = Solid(0xFF, 0xE2, 0xE2, 0xE9);

        /// <summary>Contrast against the accent fill, not against the page.</summary>
        public static IBrush PressedLegend { get; } = Solid(0xFF, 0x00, 0x35, 0x48);

        /// <summary>The app's Busy tone (<c>Ui.ToneBrush</c>): a latch outlives the finger.</summary>
        public static IBrush LatchedOutline { get; } = Solid(0xFF, 0xD9, 0x8E, 0x04);

        /// <summary>The app's Good tone: armed, one slide from latching.</summary>
        public static IBrush ArmedOutline { get; } = Solid(0xFF, 0x2E, 0xA0, 0x43);

        public static IBrush Selection => Resource(Shell.Tokens.Accent, Color.FromRgb(0x1E, 0x90, 0xFF));

        public static IBrush Guide => Resource(Shell.Tokens.AccentText, Color.FromRgb(0x00, 0xBF, 0xFF));

        public static IBrush Grid { get; } = Solid(0x28, 0x87, 0x91, 0x9D);

        /// <summary>The app's Bad tone.</summary>
        public static IBrush InvalidStroke { get; } = Solid(0xFF, 0xD1, 0x34, 0x38);

        public static IBrush InvalidFill { get; } = Solid(0xFF, 0x5C, 0x16, 0x18);

        public static IBrush Translucent(IBrush source, double alpha)
        {
            if (source is not ISolidColorBrush solid)
            {
                return source;
            }

            var colour = solid.Color;
            return new SolidColorBrush(Color.FromArgb(
                (byte)Math.Clamp(colour.A * alpha, 0, 255), colour.R, colour.G, colour.B));
        }

        private static IBrush Solid(byte a, byte r, byte g, byte b) =>
            new SolidColorBrush(Color.FromArgb(a, r, g, b));

        /// <summary>The shell's token, resolved against the dark variant: the surface is always dark.</summary>
        private static IBrush Resource(string key, Color fallback) =>
            Application.Current is { } app &&
            app.TryGetResource(key, ThemeVariant.Dark, out var value) &&
            value is IBrush brush
                ? brush
                : new SolidColorBrush(fallback);
    }
}
