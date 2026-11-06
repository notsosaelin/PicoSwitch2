namespace PicoSwitch.Bridge.Touch;

/// <summary>Which safe edge the editor toolbar can dock to.</summary>
public enum TouchToolbarEdge
{
    Top,
    Bottom,
    Left,
    Right,
}

public static class TouchToolbarEdges
{
    public static string Key(this TouchToolbarEdge value) => value switch
    {
        TouchToolbarEdge.Top => "top",
        TouchToolbarEdge.Bottom => "bottom",
        TouchToolbarEdge.Left => "left",
        _ => "right",
    };

    public static string Title(this TouchToolbarEdge value) => value switch
    {
        TouchToolbarEdge.Top => "Top",
        TouchToolbarEdge.Bottom => "Bottom",
        TouchToolbarEdge.Left => "Left",
        _ => "Right",
    };

    /// <summary>Docked left or right, the toolbar lays its buttons out in a column.</summary>
    public static bool Vertical(this TouchToolbarEdge value) =>
        value is TouchToolbarEdge.Left or TouchToolbarEdge.Right;

    public static TouchToolbarEdge? FromKey(string? value) =>
        Enum.GetValues<TouchToolbarEdge>()
            .Cast<TouchToolbarEdge?>()
            .FirstOrDefault(edge => edge!.Value.Key() == value);
}

/// <summary>
/// Where the editor's toolbar sits.
///
/// Docked and floating are genuinely different states rather than one position with a
/// flag: a docked toolbar follows its edge when the window changes shape, and a floating
/// one keeps the place the user put it. Collapsing them would mean choosing which of
/// those two behaviours to get wrong.
/// </summary>
public abstract record TouchToolbarPlacement
{
    private TouchToolbarPlacement()
    {
    }

    public static readonly TouchToolbarPlacement Default = new Docked(TouchToolbarEdge.Bottom);

    public sealed record Docked(TouchToolbarEdge Edge) : TouchToolbarPlacement;

    /// <summary>
    /// Free position, normalized within the interaction-safe region.
    ///
    /// Normalized rather than in pixels so the toolbar lands in the same PLACE on a
    /// re-open at a different size, and clamped on the way out so it can never come back
    /// somewhere the user cannot reach.
    /// </summary>
    public sealed record Floating(float X, float Y) : TouchToolbarPlacement;
}

/// <summary>
/// The geometry behind a draggable, dockable toolbar.
///
/// Pure, and in the shared module, because every rule here is a rule about REACHABILITY
/// — which edge is close enough to dock to, whether a remembered position still lies
/// inside the window — and a reachability rule that lives in one platform's UI layer is a
/// rule the next platform gets wrong.
/// </summary>
public static class TouchToolbarLayout
{
    /// <summary>
    /// How close a dragged toolbar must come to a safe edge to offer docking, as a
    /// fraction of the toolbar's own shorter side.
    ///
    /// Derived from the toolbar rather than a raw pixel constant: the zone has to be about
    /// one control wide on every density, and a magic number would be a different physical
    /// distance on each device.
    /// </summary>
    public const float SnapZoneFactor = 1.0f;

    /// <summary>
    /// Which edge a toolbar at this position is offering to dock to, if any.
    ///
    /// The coordinates are the toolbar's top-left in the region's own space. The nearest
    /// edge inside the zone wins.
    /// </summary>
    public static TouchToolbarEdge? DockCandidate(
        float x, float y, float toolbarWidth, float toolbarHeight, TouchLayoutRegion region)
    {
        if (region.Width <= 0f || region.Height <= 0f)
        {
            return null;
        }

        var zone = MathF.Min(toolbarWidth, toolbarHeight) * SnapZoneFactor;
        if (zone <= 0f)
        {
            return null;
        }

        (TouchToolbarEdge Edge, float Gap)[] gaps =
        [
            (TouchToolbarEdge.Left, x - region.Left),
            (TouchToolbarEdge.Right, region.Right - (x + toolbarWidth)),
            (TouchToolbarEdge.Top, y - region.Top),
            (TouchToolbarEdge.Bottom, region.Bottom - (y + toolbarHeight)),
        ];

        var inside = gaps.Where(entry => entry.Gap <= zone).ToList();
        if (inside.Count == 0)
        {
            return null;
        }

        // MinBy keeps the first on a tie, which fixes the order rather than leaving it to
        // whichever comparison ran first.
        return inside.MinBy(entry => entry.Gap).Edge;
    }

    /// <summary>
    /// Bring a placement back inside the region, keeping its kind.
    ///
    /// A dock is always valid — the edge exists whatever the window does — so only a
    /// floating position can need repair. Called after every geometry change, because a
    /// toolbar the user cannot reach is a toolbar with no Done button.
    /// </summary>
    public static TouchToolbarPlacement Clamp(
        TouchToolbarPlacement placement,
        float toolbarWidth,
        float toolbarHeight,
        TouchLayoutRegion region)
    {
        if (placement is not TouchToolbarPlacement.Floating floating)
        {
            return placement;
        }

        // A value that is not a number has no position to clamp TO, and letting one
        // through puts the toolbar at NaN, which draws nowhere and cannot be dragged back.
        if (!float.IsFinite(floating.X) || !float.IsFinite(floating.Y))
        {
            return TouchToolbarPlacement.Default;
        }

        if (region.Width <= 0f || region.Height <= 0f)
        {
            return placement;
        }

        var maxX = MathF.Max(0f, region.Width - toolbarWidth);
        var maxY = MathF.Max(0f, region.Height - toolbarHeight);
        var x = Math.Clamp(floating.X * region.Width, 0f, maxX);
        var y = Math.Clamp(floating.Y * region.Height, 0f, maxY);

        return new TouchToolbarPlacement.Floating(x / region.Width, y / region.Height);
    }

    /// <summary>
    /// Where the toolbar's top-left goes, for ANY placement.
    ///
    /// Docked positions are computed here rather than left to a host's own "align to the
    /// edge of the screen", and that is the whole point: the edge a toolbar docks to is
    /// the INTERACTION-SAFE edge, not the physical one. A host aligning to the window
    /// would put a docked toolbar under the system gesture strip or behind a cutout — the
    /// same mistake the layout resolver exists to prevent for controls.
    /// </summary>
    public static (float X, float Y) TopLeft(
        TouchToolbarPlacement placement,
        float toolbarWidth,
        float toolbarHeight,
        TouchLayoutRegion region)
    {
        var safe = Clamp(placement, toolbarWidth, toolbarHeight, region);

        (float X, float Y) raw;
        if (safe is TouchToolbarPlacement.Floating floating)
        {
            raw = (
                region.Left + (floating.X * region.Width),
                region.Top + (floating.Y * region.Height));
        }
        else
        {
            var edge = ((TouchToolbarPlacement.Docked)safe).Edge;
            var centredX = region.Left + ((region.Width - toolbarWidth) / 2f);
            var centredY = region.Top + ((region.Height - toolbarHeight) / 2f);

            raw = edge switch
            {
                TouchToolbarEdge.Top => (centredX, region.Top),
                TouchToolbarEdge.Bottom => (centredX, region.Bottom - toolbarHeight),
                TouchToolbarEdge.Left => (region.Left, centredY),
                _ => (region.Right - toolbarWidth, centredY),
            };
        }

        return Reachable(raw.X, raw.Y, toolbarWidth, toolbarHeight, region);
    }

    /// <summary>
    /// THE invariant: the toolbar's leading corner is always inside the region.
    ///
    /// Every other rule above assumes the toolbar fits. When it does not — a window
    /// dragged small, a large font scale, an inset that grew — the arithmetic that centres
    /// or right-aligns it produces a coordinate outside the safe rectangle, and a
    /// right-docked toolbar whose leading edge has gone off-screen has taken the drag
    /// handle with it. There is then no gesture that brings it back and no Done button to
    /// leave by.
    ///
    /// So the top-left is coerced last, unconditionally. When the toolbar is genuinely
    /// wider than the window it overflows the FAR edge, where wrapping has already put the
    /// least-critical buttons, and the handle stays where a finger can reach it.
    /// </summary>
    private static (float X, float Y) Reachable(
        float x, float y, float toolbarWidth, float toolbarHeight, TouchLayoutRegion region)
    {
        if (region.Width <= 0f || region.Height <= 0f)
        {
            return (x, y);
        }

        var maxX = MathF.Max(region.Left, region.Right - toolbarWidth);
        var maxY = MathF.Max(region.Top, region.Bottom - toolbarHeight);
        return (Math.Clamp(x, region.Left, maxX), Math.Clamp(y, region.Top, maxY));
    }

    /// <summary>
    /// Turn a dragged pixel position into the placement a release would produce.
    ///
    /// One function so the docking PREVIEW and the docking RESULT cannot disagree: the
    /// surface highlights whatever this returns while the drag is live and commits the
    /// same value when the finger lifts.
    /// </summary>
    public static TouchToolbarPlacement PlacementFor(
        float x, float y, float toolbarWidth, float toolbarHeight, TouchLayoutRegion region)
    {
        if (DockCandidate(x, y, toolbarWidth, toolbarHeight, region) is { } edge)
        {
            return new TouchToolbarPlacement.Docked(edge);
        }

        var normalized = new TouchToolbarPlacement.Floating(
            region.Width > 0f ? (x - region.Left) / region.Width : 0f,
            region.Height > 0f ? (y - region.Top) / region.Height : 0f);

        return Clamp(normalized, toolbarWidth, toolbarHeight, region);
    }
}
