namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// Platform-neutral GameCube face-button contours.
///
/// These are twenty equal-arc samples of Dolphin's <c>gcpad_x.png</c> and
/// <c>gcpad_y.png</c> alpha &gt;= 128 boundaries, normalized to each
/// non-transparent silhouette. A host turns them into drawing paths; shared input
/// routing uses the same geometry so interlocking artwork never disagrees with
/// touch.
///
/// The numbers are identical to the Kotlin table by construction — they are a
/// sampled asset, not a derivation, and two independently "tidied" copies would
/// make the drawn shape and the answerable region diverge in a way only a user's
/// thumb would notice.
/// </summary>
public static class TouchGameCubeGeometry
{
    public static IReadOnlyList<TouchVector> Contour(TouchVisualRole role) => role switch
    {
        TouchVisualRole.GameCubeBeanX => XContour,
        TouchVisualRole.GameCubeBeanY => YContour,
        _ => throw new ArgumentOutOfRangeException(
            nameof(role), role, "Not a GameCube contour role"),
    };

    /// <summary>Points relative to the control centre after clockwise screen-space rotation.</summary>
    public static IReadOnlyList<TouchVector> OrientedContour(
        TouchVisualRole role, float width, float height, float rotationDegrees)
    {
        var radians = rotationDegrees * Math.PI / 180d;
        var cosine = (float)Math.Cos(radians);
        var sine = (float)Math.Sin(radians);

        var source = Contour(role);
        var points = new TouchVector[source.Count];
        for (var index = 0; index < source.Count; index++)
        {
            var x = (source[index].X - 0.5f) * width;
            var y = (source[index].Y - 0.5f) * height;
            points[index] = new TouchVector(
                (x * cosine) - (y * sine),
                (x * sine) + (y * cosine));
        }

        return points;
    }

    /// <summary>Contour hit test with an optional rounded expansion for touch comfort.</summary>
    public static bool Contains(
        TouchVisualRole role,
        float x,
        float y,
        float width,
        float height,
        float rotationDegrees,
        float margin)
    {
        var radians = -rotationDegrees * Math.PI / 180d;
        var cosine = (float)Math.Cos(radians);
        var sine = (float)Math.Sin(radians);
        var localX = (x * cosine) - (y * sine);
        var localY = (x * sine) + (y * cosine);

        var source = Contour(role);
        var points = new TouchVector[source.Count];
        for (var index = 0; index < source.Count; index++)
        {
            points[index] = new TouchVector(
                (source[index].X - 0.5f) * width,
                (source[index].Y - 0.5f) * height);
        }

        if (InsidePolygon(localX, localY, points))
        {
            return true;
        }

        if (margin <= 0f)
        {
            return false;
        }

        var marginSquared = margin * margin;
        for (var index = 0; index < points.Length; index++)
        {
            var distance = DistanceSquaredToSegment(
                localX, localY, points[index], points[(index + 1) % points.Length]);
            if (distance <= marginSquared)
            {
                return true;
            }
        }

        return false;
    }

    private static bool InsidePolygon(float x, float y, IReadOnlyList<TouchVector> points)
    {
        var inside = false;
        var previous = points[^1];
        foreach (var current in points)
        {
            var crosses = current.Y > y != previous.Y > y &&
                x < ((previous.X - current.X) * (y - current.Y) / (previous.Y - current.Y)) + current.X;
            if (crosses)
            {
                inside = !inside;
            }

            previous = current;
        }

        return inside;
    }

    private static float DistanceSquaredToSegment(
        float x, float y, TouchVector start, TouchVector end)
    {
        var dx = end.X - start.X;
        var dy = end.Y - start.Y;
        var lengthSquared = (dx * dx) + (dy * dy);
        var t = lengthSquared > 0f
            ? Math.Clamp((((x - start.X) * dx) + ((y - start.Y) * dy)) / lengthSquared, 0f, 1f)
            : 0f;
        var offsetX = x - (start.X + (t * dx));
        var offsetY = y - (start.Y + (t * dy));
        return (offsetX * offsetX) + (offsetY * offsetY);
    }

    private static readonly TouchVector[] XContour =
    [
        new(0.2994f, 0.0000f), new(0.1126f, 0.0523f),
        new(0.0127f, 0.1634f), new(0.0301f, 0.2891f),
        new(0.1274f, 0.4010f), new(0.2038f, 0.5181f),
        new(0.2357f, 0.6467f), new(0.2484f, 0.7801f),
        new(0.2924f, 0.9055f), new(0.4533f, 0.9850f),
        new(0.6606f, 0.9932f), new(0.8418f, 0.9327f),
        new(0.9447f, 0.8223f), new(0.9936f, 0.6981f),
        new(1.0000f, 0.5631f), new(0.9682f, 0.4346f),
        new(0.9050f, 0.3140f), new(0.8111f, 0.2013f),
        new(0.6767f, 0.0990f), new(0.5048f, 0.0245f),
    ];

    private static readonly TouchVector[] YContour =
    [
        new(0.5922f, 0.0000f), new(0.4625f, 0.0309f),
        new(0.3407f, 0.0915f), new(0.2261f, 0.1789f),
        new(0.1217f, 0.3046f), new(0.0409f, 0.4670f),
        new(0.0039f, 0.6576f), new(0.0314f, 0.8493f),
        new(0.1310f, 0.9781f), new(0.2614f, 0.9939f),
        new(0.3743f, 0.8998f), new(0.4867f, 0.8042f),
        new(0.6101f, 0.7500f), new(0.7399f, 0.7195f),
        new(0.8711f, 0.6943f), new(0.9672f, 0.5632f),
        new(0.9991f, 0.3705f), new(0.9543f, 0.1850f),
        new(0.8517f, 0.0610f), new(0.7268f, 0.0122f),
    ];
}
