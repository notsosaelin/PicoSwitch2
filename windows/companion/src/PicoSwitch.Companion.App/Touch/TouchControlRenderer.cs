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
/// ## Why visuals are reused rather than rebuilt
///
/// A drag repaints on every pointer event (§29: event-driven, never a frame clock), so
/// rebuilding thirty <c>UIElement</c>s per move would allocate its way through a gesture.
/// Visuals are keyed by instance id and updated in place; only a control that appeared or
/// disappeared costs an allocation.
/// </summary>
public sealed class TouchControlRenderer(Canvas canvas)
{
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

    /// <summary>One control's elements, kept together so they move as one.</summary>
    private sealed class ControlVisual
    {
        private Path Body { get; init; } = null!;

        private Path Detail { get; init; } = null!;

        private TextBlock Legend { get; init; } = null!;

        public static ControlVisual Create(Canvas canvas)
        {
            var visual = new ControlVisual
            {
                Body = new Path { StrokeThickness = 2 },
                Detail = new Path { StrokeThickness = 2, IsHitTestVisible = false },
                Legend = new TextBlock
                {
                    IsHitTestVisible = false,
                    TextAlignment = TextAlignment.Center,
                    FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                },
            };

            canvas.Children.Add(visual.Body);
            canvas.Children.Add(visual.Detail);
            canvas.Children.Add(visual.Legend);
            return visual;
        }

        public void Remove(Canvas canvas)
        {
            canvas.Children.Remove(Body);
            canvas.Children.Remove(Detail);
            canvas.Children.Remove(Legend);
        }

        public void Update(ResolvedTouchControl control, TouchRenderOptions options)
        {
            var selected = options.Editing && options.Selection.Contains(control.Id);
            var invalid = options.Invalid.Contains(control.Id);

            Body.Data = BodyGeometry(control);
            Body.Fill = invalid ? Palette.InvalidFill : Palette.Fill(control.Spec.VisualRole);
            Body.Stroke = invalid
                ? Palette.InvalidStroke
                : selected ? Palette.Selection : Palette.Stroke;
            Body.StrokeThickness = selected || invalid ? 3 : 1.5;
            Canvas.SetZIndex(Body, control.Spec.ZIndex);

            Detail.Data = DetailGeometry(control);
            Detail.Visibility = Detail.Data is null ? Visibility.Collapsed : Visibility.Visible;
            Detail.Fill = Palette.Detail;
            Detail.Stroke = null;
            Canvas.SetZIndex(Detail, control.Spec.ZIndex);

            var legend = LegendFor(control.Spec);
            Legend.Visibility = legend.Text.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
            Legend.Text = legend.Text;
            Legend.FontFamily = legend.Symbol ? Palette.SymbolFont : Palette.TextFont;
            Legend.Foreground = Palette.Legend;
            Legend.FontSize = Math.Clamp(control.TrackingRadius * 0.62, 10d, 26d);
            Legend.Width = control.HalfWidth * 2;
            Canvas.SetLeft(Legend, control.CenterX - control.HalfWidth);
            Canvas.SetTop(Legend, control.CenterY - (Legend.FontSize * 0.72));
            Canvas.SetZIndex(Legend, control.Spec.ZIndex);
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
                var rectangle = new RectangleGeometry
                {
                    Rect = new Rect(
                        control.CenterX - control.HalfWidth,
                        control.CenterY - control.HalfHeight,
                        control.HalfWidth * 2,
                        control.HalfHeight * 2),
                };

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
        /// The mark inside the body: a D-pad's cross, a stick's knob.
        ///
        /// Null for a plain button, whose legend is its whole content.
        /// </summary>
        private static Geometry? DetailGeometry(ResolvedTouchControl control) =>
            control.Spec.VisualRole switch
            {
                TouchVisualRole.UnifiedDpad => Cross(control),
                TouchVisualRole.AnalogStick => new EllipseGeometry
                {
                    Center = new Point(control.CenterX, control.CenterY),
                    RadiusX = control.TrackingRadius * 0.46,
                    RadiusY = control.TrackingRadius * 0.46,
                },
                _ => null,
            };

        private static Geometry Cross(ResolvedTouchControl control)
        {
            var arm = control.TrackingRadius * 0.86;
            var half = control.TrackingRadius * 0.29;

            var group = new GeometryGroup();
            group.Children.Add(new RectangleGeometry
            {
                Rect = new Rect(
                    control.CenterX - arm, control.CenterY - half, arm * 2, half * 2),
            });
            group.Children.Add(new RectangleGeometry
            {
                Rect = new Rect(
                    control.CenterX - half, control.CenterY - arm, half * 2, arm * 2),
            });
            return group;
        }

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
        private static (string Text, bool Symbol) LegendFor(TouchControlSpec spec) => spec.Glyph switch
        {
            // Segoe Fluent Icons: a square for Capture, a home for Home. The portable
            // layer names the ROLE and never a resource id, so this table is the host's
            // whole contribution.
            TouchControlGlyph.Capture => ("", true),
            TouchControlGlyph.Home => ("", true),
            _ => spec.Action is TouchControlAction.Face face
                ? (ControllerLayoutResolver.FaceLabel(face.Position, TouchControlNaming.FaceLayout),
                   false)
                : (spec.Label, false),
        };
    }

    /// <summary>
    /// Colours.
    ///
    /// Read from the theme dictionary where a suitable brush exists, so the surface follows
    /// light, dark and high contrast without a second palette to keep in step; the literal
    /// fallbacks exist only for the case where a key is missing, which would otherwise be a
    /// null brush and an invisible control.
    /// </summary>
    private static class Palette
    {
        public static Brush Stroke => Resource("ControlStrokeColorDefaultBrush", Microsoft.UI.Colors.Gray);

        public static Brush Detail => Resource("TextFillColorPrimaryBrush", Microsoft.UI.Colors.White);

        public static Brush Legend => Resource("TextFillColorPrimaryBrush", Microsoft.UI.Colors.White);

        public static Brush Selection => Resource("AccentFillColorDefaultBrush", Microsoft.UI.Colors.DodgerBlue);

        public static Brush Guide => Resource("AccentFillColorSecondaryBrush", Microsoft.UI.Colors.DeepSkyBlue);

        public static Brush Grid => Resource("ControlStrokeColorSecondaryBrush", Microsoft.UI.Colors.DimGray);

        public static Brush InvalidStroke =>
            Resource("SystemFillColorCriticalBrush", Microsoft.UI.Colors.OrangeRed);

        public static Brush InvalidFill =>
            Resource("SystemFillColorCriticalBackgroundBrush", Microsoft.UI.Colors.DarkRed);

        public static FontFamily SymbolFont { get; } = new("Segoe Fluent Icons");

        public static FontFamily TextFont { get; } = new("Segoe UI");

        public static Brush Fill(TouchVisualRole role) => role switch
        {
            TouchVisualRole.AnalogStick or TouchVisualRole.UnifiedDpad =>
                Resource("ControlAltFillColorQuarternaryBrush", Microsoft.UI.Colors.DimGray),
            TouchVisualRole.Utility =>
                Resource("ControlFillColorSecondaryBrush", Microsoft.UI.Colors.Gray),
            _ => Resource("ControlFillColorDefaultBrush", Microsoft.UI.Colors.SlateGray),
        };

        private static Brush Resource(string key, Color fallback) =>
            Application.Current.Resources.TryGetValue(key, out var value) && value is Brush brush
                ? brush
                : new SolidColorBrush(fallback);
    }
}
