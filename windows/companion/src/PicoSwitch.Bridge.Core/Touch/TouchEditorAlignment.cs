namespace PicoSwitch.Bridge.Touch;

/// <summary>A movement in the interaction region's pixel space.</summary>
public readonly record struct TouchEditorDelta(float X, float Y)
{
    public static readonly TouchEditorDelta Zero = new(0f, 0f);
}

/// <summary>Why a guide is being drawn; hosts may style the kinds differently.</summary>
public enum TouchGuideKind
{
    /// <summary>The interaction area's horizontal or vertical mid-line.</summary>
    RegionCenter,

    /// <summary>Another control's centre on the same axis.</summary>
    ControlAlignment,

    /// <summary>The innermost line the selection's own extent can reach.</summary>
    SafeEdge,

    /// <summary>A grid line, when the grid is enabled.</summary>
    Grid,
}

/// <summary>
/// One infinite line the host draws across the interaction area.
///
/// <see cref="Vertical"/> describes the LINE, not the axis it constrains: a vertical
/// line lives at a constant x.
/// </summary>
public readonly record struct TouchGuideLine(bool Vertical, float Position, TouchGuideKind Kind);

/// <summary>What the editor's alignment assistance is currently doing.</summary>
public sealed record TouchAlignmentSettings
{
    public static readonly TouchAlignmentSettings Off = new();

    /// <summary>Draw the grid. Independent of <see cref="Snap"/>: a visible grid is an aid, not a rule.</summary>
    public bool Grid { get; init; }

    /// <summary>Pull a moved selection onto nearby guides.</summary>
    public bool Snap { get; init; }
}

/// <summary>
/// Optional alignment assistance for a direct-manipulation editor.
///
/// Two properties are deliberate and both come from the editor design:
///
/// - Guides ASSIST. They never restrict placement: with snapping on, a movement larger
///   than <see cref="SnapToleranceUnits"/> always wins over the nearest guide, so no
///   position becomes unreachable.
/// - The grid is drawn in the interaction region, not the window. Anchoring it to the
///   window would put grid lines under the gesture strips and cutouts that the layout
///   itself already refuses to place controls in, and "aligned to the grid" would stop
///   meaning "aligned with the other controls".
///
/// All of this is pure and operates on already-resolved pixel geometry, so it is
/// testable at every aspect ratio without a device and identical on every host.
/// </summary>
public static class TouchEditorAlignment
{
    /// <summary>
    /// Grid pitch in logical units.
    ///
    /// Half the smallest accessible target: fine enough to place a control
    /// deliberately, coarse enough that the drawn grid stays a background reference
    /// rather than a texture over the controls the user is trying to judge.
    /// </summary>
    public const float GridStepUnits = 22f;

    /// <summary>How near a guide has to be, in logical units, before it pulls.</summary>
    public const float SnapToleranceUnits = 7f;

    /// <summary>How near a guide has to be before the host draws it as matched.</summary>
    public const float MatchToleranceUnits = 1.5f;

    /// <summary>Bound on grid generation; a degenerate unit scale must not spin here.</summary>
    private const int MaxGridLines = 512;

    /// <summary>
    /// Adjust a proposed movement so the selection lands on a nearby guide.
    ///
    /// The correction is computed from ONE reference control — <paramref name="primaryId"/>
    /// — and then applied to the whole movement, so a multi-control selection keeps its
    /// internal spacing exactly. Snapping the members individually would align each of
    /// them to a different guide and pull the cluster apart.
    ///
    /// Deltas are incremental, one per pointer event, which gives the sticky behaviour a
    /// snap is supposed to have: once on a guide, small movements keep resolving back
    /// onto it until one is large enough to escape.
    /// </summary>
    public static TouchEditorDelta Snap(
        ResolvedTouchLayout layout,
        IReadOnlySet<string> selection,
        string primaryId,
        TouchEditorDelta delta,
        TouchAlignmentSettings settings)
    {
        if (!settings.Snap)
        {
            return delta;
        }

        if (!float.IsFinite(delta.X) || !float.IsFinite(delta.Y))
        {
            return TouchEditorDelta.Zero;
        }

        var primary = layout.Control(primaryId);
        if (primary is null)
        {
            return delta;
        }

        var region = layout.Region;
        if (region.UnitScale <= 0f)
        {
            return delta;
        }

        var tolerance = SnapToleranceUnits * region.UnitScale;
        var targetX = primary.CenterX + delta.X;
        var targetY = primary.CenterY + delta.Y;

        var snappedX = Nearest(
            Candidates(layout, selection, region, settings, vertical: true, primary),
            targetX, tolerance);
        var snappedY = Nearest(
            Candidates(layout, selection, region, settings, vertical: false, primary),
            targetY, tolerance);

        return new TouchEditorDelta(
            snappedX is { } x ? delta.X + (x - targetX) : delta.X,
            snappedY is { } y ? delta.Y + (y - targetY) : delta.Y);
    }

    /// <summary>
    /// The guides the current placement is actually sitting on.
    ///
    /// Returned for DRAWING only. A guide that appears when nothing is aligned is noise,
    /// and a guide that never appears when something is aligned makes the user check
    /// alignment by eye — which is the problem the feature exists to remove.
    /// </summary>
    public static IReadOnlyList<TouchGuideLine> MatchedGuides(
        ResolvedTouchLayout layout,
        IReadOnlySet<string> selection,
        string? primaryId,
        TouchAlignmentSettings settings)
    {
        var primary = primaryId is null ? null : layout.Control(primaryId);
        if (primary is null || layout.Region.UnitScale <= 0f)
        {
            return [];
        }

        var region = layout.Region;
        var tolerance = MatchToleranceUnits * region.UnitScale;
        var lines = new List<TouchGuideLine>();

        lines.AddRange(Candidates(layout, selection, region, settings, true, primary)
            .Where(line => MathF.Abs(line.Position - primary.CenterX) <= tolerance));
        lines.AddRange(Candidates(layout, selection, region, settings, false, primary)
            .Where(line => MathF.Abs(line.Position - primary.CenterY) <= tolerance));

        // Two guides of different kinds can coincide (a control that happens to sit on
        // the centre line). Draw the more specific one only.
        return lines
            .GroupBy(line => (line.Vertical, (int)MathF.Round(line.Position)))
            .Select(group => group.MinBy(line => (int)line.Kind))
            .ToList();
    }

    /// <summary>
    /// The grid lines to draw, inside the interaction region.
    ///
    /// Anchored to the region's centre rather than its left edge so the grid is
    /// symmetric: the centre column and row always exist, and a layout that is
    /// mirror-symmetric stays symmetric when both halves are snapped.
    /// </summary>
    public static IReadOnlyList<TouchGuideLine> GridLines(
        TouchLayoutRegion region, TouchAlignmentSettings settings)
    {
        if (!settings.Grid || region.UnitScale <= 0f)
        {
            return [];
        }

        var step = GridStepUnits * region.UnitScale;
        if (step <= 0f || region.Width <= 0f || region.Height <= 0f)
        {
            return [];
        }

        var lines = new List<TouchGuideLine>();
        foreach (var position in GridPositions(region.Left, region.Right, step))
        {
            lines.Add(new TouchGuideLine(true, position, TouchGuideKind.Grid));
        }

        foreach (var position in GridPositions(region.Top, region.Bottom, step))
        {
            lines.Add(new TouchGuideLine(false, position, TouchGuideKind.Grid));
        }

        return lines;
    }

    private static List<float> GridPositions(float start, float end, float step)
    {
        var center = (start + end) / 2f;
        var first = center - (MathF.Floor((center - start) / step) * step);
        var positions = new List<float>();
        var value = first;
        var guard = 0;

        while (value <= end && guard < MaxGridLines)
        {
            positions.Add(value);
            value += step;
            guard++;
        }

        return positions;
    }

    private static List<TouchGuideLine> Candidates(
        ResolvedTouchLayout layout,
        IReadOnlySet<string> selection,
        TouchLayoutRegion region,
        TouchAlignmentSettings settings,
        bool vertical,
        ResolvedTouchControl primary)
    {
        var lines = new List<TouchGuideLine>();
        var center = vertical
            ? (region.Left + region.Right) / 2f
            : (region.Top + region.Bottom) / 2f;
        lines.Add(new TouchGuideLine(vertical, center, TouchGuideKind.RegionCenter));

        // The innermost position the primary control's own answerable extent can occupy.
        // Offering the raw region edge would propose a placement the audit then blocks.
        var half = vertical ? primary.HitHalfWidth : primary.HitHalfHeight;
        var low = (vertical ? region.Left : region.Top) + half;
        var high = (vertical ? region.Right : region.Bottom) - half;
        if (low <= high)
        {
            lines.Add(new TouchGuideLine(vertical, low, TouchGuideKind.SafeEdge));
            lines.Add(new TouchGuideLine(vertical, high, TouchGuideKind.SafeEdge));
        }

        foreach (var control in layout.Controls)
        {
            if (selection.Contains(control.Id))
            {
                continue;
            }

            lines.Add(new TouchGuideLine(
                vertical,
                vertical ? control.CenterX : control.CenterY,
                TouchGuideKind.ControlAlignment));
        }

        if (settings.Grid)
        {
            lines.AddRange(GridLines(region, settings).Where(line => line.Vertical == vertical));
        }

        return lines;
    }

    private static float? Nearest(
        List<TouchGuideLine> candidates, float target, float tolerance)
    {
        TouchGuideLine? best = null;
        var bestDistance = float.MaxValue;

        foreach (var candidate in candidates)
        {
            var distance = MathF.Abs(candidate.Position - target);
            if (distance > tolerance)
            {
                continue;
            }

            // Nearest first. Kind breaks a tie only when two guides coincide, where
            // sitting on the centre line is the more meaningful statement to make than
            // sitting on whichever grid line happens to be there too.
            if (best is null || distance < bestDistance ||
                (distance == bestDistance && (int)candidate.Kind < (int)best.Value.Kind))
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best?.Position;
    }
}
