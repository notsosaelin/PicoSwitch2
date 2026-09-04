using System.Text.RegularExpressions;
using Xunit;

namespace PicoSwitch.Companion.Windows.Tests;

/// <summary>
/// The on-screen controller looks the same on both companions.
///
/// ## Why this is a source test
///
/// The renderer lives in the WinUI app assembly, which no unit test can load: its
/// types want a UI thread and a packaged host. The existing
/// <c>LayeringGuardTests</c> already answers questions about that project by
/// reading its source, and the same trick works here — every value this file
/// checks is a compile-time constant, so reading it out of the text is reading
/// the real number and not a paraphrase of it.
///
/// ## Why it is worth pinning at all
///
/// The two surfaces are the same product. They were not: Windows derived its
/// colours and proportions from Fluent theme resources while Android drew from
/// its own tokens, and the result was a controller with square-ish pads, no
/// visible outlines, a WHITE D-pad and a WHITE stick knob — the nearest Fluent
/// brush to "the mark inside a control" being the primary TEXT brush. Nothing
/// failed; it just looked like a different application.
///
/// Each expectation below cites the Android constant it mirrors, so a deliberate
/// change on either side is a two-file edit rather than a silent divergence.
/// </summary>
public sealed class TouchVisualParityTests
{
    private static readonly string Renderer =
        Read("src/PicoSwitch.Companion.App/Touch/TouchControlRenderer.cs");

    private static readonly string SurfaceMarkup =
        Read("src/PicoSwitch.Companion.App/Touch/TouchGamepadView.xaml");

    private static readonly string AppMarkup =
        Read("src/PicoSwitch.Companion.App/App.xaml");

    private static readonly string Shell =
        Read("src/PicoSwitch.Companion.App/MainWindow.xaml.cs");

    /// <summary>The one place the controller's ground is defined.</summary>
    private const string GroundKey = "TouchSurfaceGroundBrush";

    private static string Read(string relative)
    {
        var cursor = new DirectoryInfo(AppContext.BaseDirectory);
        while (cursor is not null)
        {
            var root = Path.Combine(cursor.FullName, "windows", "companion");
            var candidate = Path.Combine(
                root, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }

            cursor = cursor.Parent;
        }

        throw new FileNotFoundException(
            $"Cannot find {relative} under any windows/companion above {AppContext.BaseDirectory}");
    }

    /// <summary>
    /// Every proportion the two renderers share, with its Android source.
    /// </summary>
    /// <remarks>
    /// Android's live in <c>TouchControlRenderer.kt</c>; the names differ because
    /// each language's convention does, and renaming one to match the other would
    /// only make both look foreign in their own file.
    /// </remarks>
    public static TheoryData<string, double, string> SharedMetrics() => new()
    {
        { "OutlineWidth", 2d, "OUTLINE_WIDTH" },
        { "StickKnobFraction", 0.46d, "TOUCH_STICK_KNOB_FRACTION" },
        { "DpadArmFraction", 0.90d, "ARM_FRACTION" },
        { "DpadArmHalfWidth", 0.26d, "ARM_HALF_WIDTH" },
        { "WellAlpha", 0.55d, "WELL_ALPHA" },
        { "LabelWidthFraction", 0.78d, "LABEL_WIDTH_FRACTION" },
        { "LabelHeightFraction", 0.68d, "LABEL_HEIGHT_FRACTION" },
        { "CaptureDiscFraction", 0.28d, "CAPTURE_DISC_RADIUS_FRACTION" },
        { "CaptureFillAlpha", 0.18d, "CAPTURE_DISC_FILL_ALPHA" },
        { "CaptureRimAlpha", 0.82d, "CAPTURE_DISC_RIM_ALPHA" },
        { "CaptureStrokeFraction", 0.045d, "CAPTURE_DISC_STROKE_FRACTION" },
        { "HomeCircleFraction", 0.30d, "HOME_CIRCLE_RADIUS_FRACTION" },
        { "HomeStrokeFraction", 0.05d, "HOME_CIRCLE_STROKE_FRACTION" },
        { "HomeHouseFraction", 0.18d, "HOME_HOUSE_UNIT_FRACTION" },
    };

    [Theory]
    [MemberData(nameof(SharedMetrics))]
    public void AProportionMatchesTheAndroidSurface(string name, double expected, string android)
    {
        var match = Regex.Match(
            Renderer,
            $@"const\s+(?:double|float)\s+{Regex.Escape(name)}\s*=\s*(-?[0-9.]+)[dfDF]?\s*;");

        Assert.True(match.Success, $"{name} is not declared in the Windows renderer");
        Assert.Equal(
            expected,
            double.Parse(match.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture),
            5);

        // Named only so the failure message points at the file to change with it.
        Assert.False(string.IsNullOrEmpty(android));
    }

    [Fact]
    public void TheControllerDoesNotDrawItselfWithTheTextBrush()
    {
        // The exact regression. Palette.Detail and Palette.Legend both resolved
        // TextFillColorPrimaryBrush, so the D-pad cross and the stick knob were
        // painted with the colour Fluent reserves for body text — which is white
        // in the dark theme, and made the two largest controls on the surface look
        // like cut-outs rather than like parts of a controller.
        Assert.DoesNotContain("TextFillColorPrimaryBrush", Renderer, StringComparison.Ordinal);
    }

    [Fact]
    public void TheControlFaceIsAFixedTokenAndNotAThemeBrush()
    {
        // Fluent's control fills follow the host theme; this surface paints its
        // own opaque near-black ground in BOTH themes, so a face that went pale in
        // light mode would be a control the player cannot see.
        foreach (var forbidden in new[]
                 {
                     "ControlFillColorDefaultBrush",
                     "ControlFillColorSecondaryBrush",
                     "ControlAltFillColorQuarternaryBrush",
                     "ControlStrokeColorDefaultBrush",
                 })
        {
            Assert.DoesNotContain(forbidden, Renderer, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void TheDpadCrossIsUnionedAndNotPunchedThrough()
    {
        // WinUI's GeometryGroup defaults to EvenOdd, under which the two bars of a
        // cross CANCEL where they overlap and leave a square hole through the
        // middle of the D-pad. That shipped, and it read as a checkerboard rather
        // than as a control.
        Assert.Contains("FillRule.Nonzero", Renderer, StringComparison.Ordinal);
    }

    [Fact]
    public void TheGroundIsDefinedOnceAndIsOpaque()
    {
        // Android paints Color.Black unconditionally. A theme-following ground on
        // Windows would put fixed dark artwork on white in light mode; a
        // translucent one would show the companion's own rail and cards through
        // the controller, which is the failure the markup's own comment records.
        var match = Regex.Match(
            AppMarkup,
            $@"<SolidColorBrush x:Key=""{GroundKey}"" Color=""(#[0-9A-Fa-f]{{8}})""");

        Assert.True(match.Success, $"App.xaml does not define {GroundKey} as an opaque literal");
        Assert.StartsWith("#FF", match.Groups[1].Value, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void TheSurfaceAndTheCaptionStripPaintTheSameGround()
    {
        // The white bar. The title bar is extended into the client area, so the
        // strip at the top of the window IS the top of the controller. The Touch
        // Gamepad drops the Mica backdrop -- an opaque controller over a
        // translucent material is not a thing -- and an unpainted strip then falls
        // through to the window's default brush, which is white in the light
        // theme. It showed at every windowed size and vanished only under the
        // FullScreen presenter, which collapses the strip.
        //
        // Both must reference the SAME key. Matching literals in two files is how
        // the two ends drift and the band comes back a shade off instead of
        // white.
        Assert.Contains(
            $@"Background=""{{StaticResource {GroundKey}}}""",
            SurfaceMarkup,
            StringComparison.Ordinal);
        Assert.Contains(GroundKey, Shell, StringComparison.Ordinal);
        Assert.Contains("AppTitleBar.Background", Shell, StringComparison.Ordinal);
    }

    [Fact]
    public void TheWholeWindowCarriesTheGroundAndNotOneBand()
    {
        // Painting only the caption strip was the first fix, and it was not
        // enough: a fixed 32-epx band covers the reserved caption area at one
        // display scale and not at another, which is why the white line survived
        // on a 1920x1200 at 125% while never appearing on a 4K panel at 200%.
        //
        // The root has no height to get wrong, and any row added to that Grid
        // later inherits the ground rather than becoming the next white band.
        Assert.Contains("WindowRoot.Background", Shell, StringComparison.Ordinal);
        Assert.Contains(
            "<Grid x:Name=\"WindowRoot\">",
            Read("src/PicoSwitch.Companion.App/MainWindow.xaml"),
            StringComparison.Ordinal);
    }

    [Fact]
    public void TheCaptionButtonsAreToldAboutTheGroundToo()
    {
        // The caption buttons are drawn by the SYSTEM, not by XAML, so the
        // strip's own background does not reach them: without this there is a
        // light plate behind minimise/maximise/close at the right-hand end of an
        // otherwise black band.
        Assert.Contains("ButtonBackgroundColor", Shell, StringComparison.Ordinal);
        Assert.Contains("ButtonForegroundColor", Shell, StringComparison.Ordinal);
    }

    [Fact]
    public void TheUtilityGlyphsAreDrawnRatherThanTypeset()
    {
        // Segoe Fluent Icons draws a camera and a house that are not the ones the
        // Android surface draws, at a weight this file does not control, and on a
        // machine without the font it draws a hollow box. Both glyphs are geometry
        // here for the same reason the GameCube silhouettes are.
        // The FontFamily specifically, not the words: the file names the font in a
        // comment explaining why it is not used, and a bare substring search would
        // fail on its own rationale.
        Assert.DoesNotContain(
            "new(\"Segoe Fluent Icons\")", Renderer, StringComparison.Ordinal);
        Assert.Contains("TouchControlGlyph.Capture", Renderer, StringComparison.Ordinal);
        Assert.Contains("PathFigure", Renderer, StringComparison.Ordinal);
    }
}
