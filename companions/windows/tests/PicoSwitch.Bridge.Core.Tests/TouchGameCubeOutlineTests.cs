using PicoSwitch.Bridge.Touch;
using Xunit;

namespace PicoSwitch.Bridge.Core.Tests;

/// <summary>
/// The GameCube beans are DRAWN as a smooth curve through their twenty traced
/// anchors (<see cref="TouchGameCubeGeometry.SmoothOutline"/>), the same on every
/// companion. The first Windows and Linux ports joined the anchors with straight
/// lines and the beans came out as polygons beside Android's (owner,
/// 2026-11-08). The Kotlin TouchGameCubeOutlineTest pins the same numbers.
/// </summary>
public sealed class TouchGameCubeOutlineTests
{
    [Theory]
    [InlineData(TouchVisualRole.GameCubeBeanX)]
    [InlineData(TouchVisualRole.GameCubeBeanY)]
    public void TheOutlinePassesThroughEveryAnchorInOrderAndCloses(TouchVisualRole role)
    {
        var points = TouchGameCubeGeometry.OrientedContour(role, 120, 80, 30);
        var outline = TouchGameCubeGeometry.SmoothOutline(points);

        Assert.Equal(points.Count, outline.Count);
        for (var index = 0; index < points.Count; index++)
        {
            Assert.Equal(points[(index + 1) % points.Count], outline[index].End);
        }
    }

    [Fact]
    public void SegmentsAreCurvesNotTheStraightChordsBetweenAnchors()
    {
        var points = TouchGameCubeGeometry.Contour(TouchVisualRole.GameCubeBeanX);
        var outline = TouchGameCubeGeometry.SmoothOutline(points);

        var bent = 0;
        for (var index = 0; index < outline.Count; index++)
        {
            var start = points[index];
            var segment = outline[index];
            // A control point off the chord is what makes the segment curve.
            var cross = ((segment.End.X - start.X) * (segment.Control1.Y - start.Y)) -
                        ((segment.End.Y - start.Y) * (segment.Control1.X - start.X));
            if (MathF.Abs(cross) > 1e-5f)
            {
                bent++;
            }
        }

        Assert.True(bent >= outline.Count - 2, $"only {bent} of {outline.Count} segments curve");
    }

    /// <summary>Uniform Catmull-Rom, pinned by value; the Kotlin test asserts the same numbers.</summary>
    [Fact]
    public void TheFirstSegmentMatchesTheKotlinArithmetic()
    {
        var outline = TouchGameCubeGeometry.SmoothOutline(
            TouchGameCubeGeometry.Contour(TouchVisualRole.GameCubeBeanX));

        Assert.Equal(0.234033f, outline[0].Control1.X, 4);
        Assert.Equal(0.004633f, outline[0].Control1.Y, 4);
        Assert.Equal(0.160383f, outline[0].Control2.X, 4);
        Assert.Equal(0.025067f, outline[0].Control2.Y, 4);
        Assert.Equal(new TouchVector(0.1126f, 0.0523f), outline[0].End);
    }
}
