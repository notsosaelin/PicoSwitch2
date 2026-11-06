using PicoSwitch.Bridge.Touch;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// What must be subtracted from a surface before it is safe to put a control there.
///
/// Effective pixels, one value per edge, always non-negative. Kept as a value rather than
/// four parameters because the sources are independent — a caption bar at the top, an
/// edge-gesture strip at the sides — and they have to be composed before anything can be
/// resolved.
/// </summary>
public readonly record struct TouchSafeInsets(float Left, float Top, float Right, float Bottom)
{
    public static readonly TouchSafeInsets None = new(0f, 0f, 0f, 0f);

    /// <summary>Negative or non-finite edges become zero; a bad inset must not enlarge the region.</summary>
    public TouchSafeInsets Sanitized() => new(
        Edge(Left), Edge(Top), Edge(Right), Edge(Bottom));

    private static float Edge(float value) =>
        float.IsFinite(value) && value > 0f ? value : 0f;
}

/// <summary>
/// The interaction-safe rectangle, built from what the window can tell us.
///
/// `WINDOWS_PASS.md` §15.5 makes this the platform's job and puts it BEFORE the resolve:
/// every control's position is a function of this rectangle, and so is the audit's verdict
/// on whether the layout can be played at all.
///
/// ## The unit
///
/// <see cref="TouchLayoutRegion.UnitScale"/> is pixels per logical unit, and the layout's
/// unit is the one the templates were authored in — the Android density-independent pixel.
/// On Windows the equivalent comfort unit is the effective pixel: XAML lays out in epx,
/// the user's display-scale setting is already folded into it, and the OS enlarges
/// everything else on the screen through the same mechanism.
///
/// So <see cref="EffectivePixelsPerUnit"/> is 1, and <c>RasterizationScale</c> is
/// deliberately NOT multiplied in. §15.5 lists DPI scale as an input to this rectangle,
/// which is correct for a host that draws in physical pixels; WinUI does not. Folding
/// the rasterization scale in on top of coordinates that already carry it would shrink
/// the controller by exactly the factor the user chose when they asked Windows for larger
/// UI — the opposite of what they asked for. A DPI change still re-resolves, because the
/// surface's size in epx changes when it happens.
///
/// The alternative — converting dp to epx by their nominal baselines (96/160 = 0.6) —
/// was rejected: it puts the smallest layout the audit will pass
/// (<see cref="TouchLayoutAudit.MinTargetUnits"/> = 44) at 26 epx, well under the
/// platform's own 40 epx minimum touch target (§25). Comfort unit to comfort unit is the
/// honest transfer between the two platforms; nominal inches are not.
/// </summary>
public static class TouchRegionBuilder
{
    /// <summary>One layout unit is one effective pixel. See the type doc.</summary>
    public const float EffectivePixelsPerUnit = 1f;

    /// <summary>
    /// The Windows edge-gesture strip, kept clear on a touch device.
    ///
    /// A swipe from the left or right edge opens Task View or the notification centre, so
    /// a stick placed under one of them loses the contact to the shell mid-movement —
    /// which arrives here as a <c>PointerCaptureLost</c> and a cancelled gesture, not as
    /// anything the layout can recover from. Cheaper to not put a control there.
    /// </summary>
    public const float EdgeGestureInsetEpx = 12f;

    /// <summary>
    /// Compose the insets §15.5 names.
    ///
    /// The caption bar only applies when the surface is NOT full-window: in full-window
    /// mode there is no drag region to avoid, and reserving one would leave a visible
    /// band of dead space across the top of a gameplay surface.
    /// </summary>
    public static TouchSafeInsets Insets(
        bool fullWindow, float captionBarEpx, bool touchCapable)
    {
        var edge = touchCapable ? EdgeGestureInsetEpx : 0f;
        var caption = float.IsFinite(captionBarEpx) && captionBarEpx > 0f ? captionBarEpx : 0f;
        var top = fullWindow ? edge : MathF.Max(edge, caption);

        return new TouchSafeInsets(edge, top, edge, edge).Sanitized();
    }

    /// <summary>
    /// The rectangle the layout is resolved into.
    ///
    /// Returns a zero-size region rather than throwing when the surface has no usable
    /// size — during the first measure pass, or when the insets meet in the middle. The
    /// resolver already has one truthful thing to say about that
    /// ("The interaction area has no usable size"), and inventing a second answer here
    /// would give the surface two.
    /// </summary>
    public static TouchLayoutRegion Build(
        double widthEpx, double heightEpx, TouchSafeInsets insets)
    {
        var safe = insets.Sanitized();

        if (!IsUsable(widthEpx) || !IsUsable(heightEpx))
        {
            return Empty;
        }

        var left = safe.Left;
        var top = safe.Top;
        var right = (float)widthEpx - safe.Right;
        var bottom = (float)heightEpx - safe.Bottom;

        // Insets that overlap describe a rectangle with negative width. Report nothing
        // usable rather than a mirrored one: a negative Width would make every anchor
        // resolve to a position left of the region's own left edge.
        return right <= left || bottom <= top
            ? Empty
            : new TouchLayoutRegion(left, top, right, bottom, EffectivePixelsPerUnit);
    }

    private static readonly TouchLayoutRegion Empty =
        new(0f, 0f, 0f, 0f, EffectivePixelsPerUnit);

    private static bool IsUsable(double value) =>
        double.IsFinite(value) && value > 0d;
}
