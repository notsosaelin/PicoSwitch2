using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// One thing wrong with a layout. <see cref="Blocking"/> findings make a layout
/// unplayable.
///
/// <see cref="ControlIds"/> names the instances the finding is ABOUT, when it is
/// about particular ones. It exists so an editor can point at the offending control
/// instead of printing a sentence: the same audit run that decides whether the
/// layout may be played decides which controls are drawn as broken, so the two
/// cannot disagree.
/// </summary>
public sealed record TouchLayoutFinding(
    string Message,
    bool Blocking,
    IReadOnlySet<string>? ControlIdSet = null)
{
    public IReadOnlySet<string> ControlIds { get; } =
        ControlIdSet ?? new HashSet<string>(StringComparer.Ordinal);
}

public enum TouchLayoutAuditMode
{
    /// <summary>Repository-owned defaults: every declared output is mandatory.</summary>
    ShippedTemplate,

    /// <summary>A user's draft may hide outputs, but unsafe geometry still blocks Save.</summary>
    UserDraft,

    /// <summary>Runtime effective layout; hidden outputs are allowed and reported as warnings.</summary>
    Runtime,
}

/// <summary>
/// Mechanical layout validation.
///
/// A declarative layout can be checked instead of merely looked at, and it should
/// be: the failures that matter here — a hidden hit region overlapping its
/// neighbour, a target below the size a thumb can reliably find, a control that
/// drifted outside the safe rectangle — are precisely the ones a screenshot does not
/// show. The audit runs on real resolved geometry, so it covers every window shape
/// the layout is ever asked to fit rather than the one that was rendered.
/// </summary>
public static class TouchLayoutAudit
{
    /// <summary>
    /// Smallest interactive target, in logical units.
    ///
    /// The platform accessibility guidance for an interactive target is 48; a
    /// gameplay control may show smaller artwork but must not answer to a smaller
    /// region than this.
    /// </summary>
    public const float MinTargetUnits = 44f;

    /// <summary>Half a pixel; guards against float placement noise, not against real overlap.</summary>
    private const float Tolerance = 0.5f;

    private const float ContourProbeStep = 0.5f;

    /// <summary>
    /// Every already-logical action a usable controller has to expose.
    ///
    /// L2/R2 are deliberately absent: they are reached through
    /// <see cref="TouchControlAction.Trigger"/>, which publishes the digital bit AND
    /// the analog value together, and are checked as triggers below.
    /// </summary>
    private static readonly HashSet<ControllerButton> RequiredLogical =
    [
        ControllerButton.L1, ControllerButton.R1,
        ControllerButton.Select, ControllerButton.Start,
        ControllerButton.LeftStick, ControllerButton.RightStick,
        ControllerButton.Home, ControllerButton.Capture, ControllerButton.C,
    ];

    public static IReadOnlyList<TouchLayoutFinding> Audit(
        IReadOnlyList<ResolvedTouchControl> controls, TouchLayoutRegion region)
    {
        var findings = AuditGeometry(controls, region).ToList();

        // Legacy completeness check retained for callers that have not selected a
        // personality. Profile-backed layouts use the exhaustive overload instead of
        // pretending every controller is a Pro2.
        var actions = controls.Select(control => control.Spec.Action).ToList();
        var logical = actions.OfType<TouchControlAction.Logical>()
            .Select(action => action.Button)
            .ToHashSet();

        foreach (var button in RequiredLogical.Where(button => !logical.Contains(button)))
        {
            findings.Add(new TouchLayoutFinding($"Layout has no control for {button}", false));
        }

        foreach (var position in Enum.GetValues<FaceButtonPosition>())
        {
            if (!actions.Any(action =>
                    action is TouchControlAction.Face face && face.Position == position))
            {
                findings.Add(new TouchLayoutFinding($"Layout has no {position} face control", false));
            }
        }

        foreach (var side in Enum.GetValues<ControlSide>())
        {
            if (!actions.Any(action => action is TouchControlAction.Stick stick && stick.Side == side))
            {
                findings.Add(new TouchLayoutFinding($"Layout has no {side} stick", false));
            }

            if (!actions.Any(action =>
                    action is TouchControlAction.Trigger trigger && trigger.Side == side))
            {
                findings.Add(new TouchLayoutFinding($"Layout has no {side} trigger", false));
            }
        }

        if (!actions.Any(action => action is TouchControlAction.Directions))
        {
            findings.Add(new TouchLayoutFinding("Layout has no D-pad", false));
        }

        return findings;
    }

    public static IReadOnlyList<TouchLayoutFinding> Audit(
        TouchLayout layout,
        IReadOnlyList<ResolvedTouchControl> controls,
        TouchLayoutRegion region,
        TouchControllerProfile profile,
        TouchLayoutAuditMode mode)
    {
        var findings = AuditGeometry(controls, region).ToList();
        var template = profile.DefaultTemplate;

        if (layout.ProfileId != profile.Id)
        {
            findings.Add(new TouchLayoutFinding(
                $"Layout profile does not match {profile.DisplayName}", true));
        }

        if (layout.TemplateId != template.Id ||
            layout.SchemaVersion != template.SchemaVersion ||
            layout.TemplateRevision != template.TemplateRevision)
        {
            findings.Add(new TouchLayoutFinding("Layout template metadata is inconsistent", true));
        }

        foreach (var control in layout.Controls)
        {
            var finite = float.IsFinite(control.AnchorX) && float.IsFinite(control.AnchorY) &&
                float.IsFinite(control.WidthUnits) && float.IsFinite(control.HeightUnits) &&
                float.IsFinite(control.HitMarginUnits) &&
                float.IsFinite(control.VisualRotationDegrees) &&
                float.IsFinite(control.GroupOffsetXUnits) &&
                float.IsFinite(control.GroupOffsetYUnits);

            if (string.IsNullOrWhiteSpace(control.Id) || !finite ||
                control.AnchorX is < 0f or > 1f || control.AnchorY is < 0f or > 1f ||
                control.WidthUnits <= 0f || control.HeightUnits <= 0f ||
                control.HitMarginUnits < 0f)
            {
                findings.Add(new TouchLayoutFinding(
                    $"Control '{control.Id}' has invalid authored geometry", true));
            }

            if (control.Output == TouchOutputControl.Unspecified ||
                !profile.Outputs.Contains(control.Output))
            {
                findings.Add(new TouchLayoutFinding(
                    $"Control '{control.Id}' exposes {control.Output}, which is absent from " +
                    $"{profile.DisplayName}",
                    true));
                continue;
            }

            if (!profile.Bindings.TryGetValue(control.Output, out var binding))
            {
                findings.Add(new TouchLayoutFinding(
                    $"{control.Output} has no fixed bridge binding", true));
            }
            else if (!binding.Equals(control.Action))
            {
                findings.Add(new TouchLayoutFinding(
                    $"Control '{control.Id}' changed the fixed {control.Output} binding", true));
            }
        }

        // Duplicate outputs are LEGAL from Editor 2.0 onward. Two A buttons are two
        // instances contributing to one binding, and the engine aggregates them;
        // refusing the layout here would refuse the feature. What must still hold is
        // that each instance has its own identity and its own unambiguous hit region,
        // and AuditGeometry checks both.
        var present = layout.Controls.Select(control => control.Output).ToHashSet();

        // Outputs the shipped default deliberately does not place are not missing
        // when they are absent — that IS the authored starting point. Supporting a
        // control and placing one are separate claims. Everything else stays exactly
        // as strict, so a genuinely dropped control is still blocking.
        var optional = template.Controls
            .Where(control => !control.InDefaultLayout)
            .Select(control => control.Output)
            .ToHashSet();

        foreach (var missing in profile.Outputs
                     .Where(output => !present.Contains(output) && !optional.Contains(output)))
        {
            findings.Add(new TouchLayoutFinding(
                $"{profile.DisplayName} control {missing} is hidden or missing",
                mode == TouchLayoutAuditMode.ShippedTemplate));
        }

        return findings;
    }

    private static List<TouchLayoutFinding> AuditGeometry(
        IReadOnlyList<ResolvedTouchControl> controls, TouchLayoutRegion region)
    {
        var findings = new List<TouchLayoutFinding>();
        var unit = region.UnitScale > 0f ? region.UnitScale : 1f;

        foreach (var duplicate in controls
                     .GroupBy(control => control.Id, StringComparer.Ordinal)
                     .Where(group => group.Count() > 1)
                     .Select(group => group.Key))
        {
            findings.Add(new TouchLayoutFinding(
                $"Duplicate control id '{duplicate}'", true,
                new HashSet<string>(StringComparer.Ordinal) { duplicate }));
        }

        foreach (var control in controls)
        {
            var name = control.Spec.DisplayName;
            var shortestUnits =
                MathF.Min(control.HitHalfWidth, control.HitHalfHeight) * 2f / unit;
            if (shortestUnits < MinTargetUnits)
            {
                findings.Add(new TouchLayoutFinding(
                    $"{name} answers to only {(int)shortestUnits} units", true,
                    new HashSet<string>(StringComparer.Ordinal) { control.Id }));
            }

            // Both the artwork and the complete answerable target must remain in the
            // safe rectangle. Checking only the visual half-extent would let an
            // invisible hit margin sit under a system gesture strip, and checking the
            // UNROTATED extent would let a turned control's corner do the same.
            var outside =
                control.CenterX - control.HitExtentX < region.Left - Tolerance ||
                control.CenterX + control.HitExtentX > region.Right + Tolerance ||
                control.CenterY - control.HitExtentY < region.Top - Tolerance ||
                control.CenterY + control.HitExtentY > region.Bottom + Tolerance;

            if (outside)
            {
                findings.Add(new TouchLayoutFinding(
                    $"{name} is outside the interaction area", true,
                    new HashSet<string>(StringComparer.Ordinal) { control.Id }));
            }
        }

        for (var i = 0; i < controls.Count; i++)
        {
            for (var j = i + 1; j < controls.Count; j++)
            {
                var a = controls[i];
                var b = controls[j];

                // Two instances of the SAME output may overlap freely. Whichever one a
                // contact lands on produces the same thing, so there is no ambiguity to
                // report — and stacking duplicates deliberately is a reasonable way to
                // build a larger target out of two controls.
                if (a.Spec.Output != TouchOutputControl.Unspecified &&
                    a.Spec.Output == b.Spec.Output)
                {
                    continue;
                }

                switch (Overlap(a, b))
                {
                    case TouchOverlap.None:
                        break;

                    // The DRAWN shapes collide. Blocking: the router would have to let
                    // z-order decide what the user pressed, and a control that answers
                    // unpredictably is worse than a layout that refuses to load.
                    case TouchOverlap.Artwork:
                        findings.Add(new TouchLayoutFinding(
                            $"{a.Spec.DisplayName} and {b.Spec.DisplayName} overlap", true,
                            new HashSet<string>(StringComparer.Ordinal) { a.Id, b.Id }));
                        break;

                    // Only the courtesy margins meet. Reported, never blocking: both
                    // controls remain reliably pressable by aiming at what is drawn, and a
                    // margin is an invitation rather than a claim on space. The shipped
                    // GameCube layout has exactly one of these, between `z` and the `Y` bean.
                    default:
                        findings.Add(new TouchLayoutFinding(
                            $"{a.Spec.DisplayName} and {b.Spec.DisplayName} have touching hit margins",
                            false,
                            new HashSet<string>(StringComparer.Ordinal) { a.Id, b.Id }));
                        break;
                }
            }
        }

        return findings;
    }

    private enum TouchOverlap
    {
        None,
        Margin,
        Artwork,
    }

    /// <summary>
    /// How badly two controls collide.
    ///
    /// The distinction that matters is the DRAWN shape against the courtesy margin
    /// around it. A user aims at what they can see: if the artwork is clear, both
    /// controls are reliably pressable and only the invisible expansions are
    /// ambiguous. If the artwork itself overlaps, one of the two cannot be pressed on
    /// purpose at all.
    /// </summary>
    private static TouchOverlap Overlap(ResolvedTouchControl a, ResolvedTouchControl b)
    {
        if (!Overlaps(a, b, visualOnly: false))
        {
            return TouchOverlap.None;
        }

        return Overlaps(a, b, visualOnly: true) ? TouchOverlap.Artwork : TouchOverlap.Margin;
    }

    /// <summary>Broad-phase boxes followed by the real hit shape where the boxes alone are ambiguous.</summary>
    private static bool Overlaps(ResolvedTouchControl a, ResolvedTouchControl b, bool visualOnly)
    {
        // Margins are the difference between the two passes, so the broad phase has to
        // shrink by them too or the visual pass would probe a box its own shapes
        // cannot reach.
        var marginA = a.HitHalfWidth - a.HalfWidth;
        var marginB = b.HitHalfWidth - b.HalfWidth;
        var extentAx = visualOnly ? a.HitExtentX - marginA : a.HitExtentX;
        var extentAy = visualOnly ? a.HitExtentY - marginA : a.HitExtentY;
        var extentBx = visualOnly ? b.HitExtentX - marginB : b.HitExtentX;
        var extentBy = visualOnly ? b.HitExtentY - marginB : b.HitExtentY;

        var dx = MathF.Abs(a.CenterX - b.CenterX);
        var dy = MathF.Abs(a.CenterY - b.CenterY);

        // Screen-space extents, so a rotated control's real footprint is what is
        // compared. The unrotated half-extents describe the control's own frame and
        // would miss a corner that has turned into a neighbour.
        var boxesOverlap = dx < extentAx + extentBx - Tolerance &&
                           dy < extentAy + extentBy - Tolerance;
        if (!boxesOverlap)
        {
            return false;
        }

        var halfAw = visualOnly ? a.HalfWidth : a.HitHalfWidth;
        var halfAh = visualOnly ? a.HalfHeight : a.HitHalfHeight;
        var halfBw = visualOnly ? b.HalfWidth : b.HitHalfWidth;
        var halfBh = visualOnly ? b.HalfHeight : b.HitHalfHeight;

        // Circular controls can have diagonally intersecting bounding boxes while
        // their actual answerable regions remain disjoint. Use the same shape the
        // input router uses before declaring the layout ambiguous.
        if (a.Spec.Shape == TouchControlShape.Circle &&
            b.Spec.Shape == TouchControlShape.Circle &&
            MathF.Abs(halfAw - halfAh) <= Tolerance &&
            MathF.Abs(halfBw - halfBh) <= Tolerance)
        {
            var minimumDistance = halfAw + halfBw - Tolerance;
            return (dx * dx) + (dy * dy) < minimumDistance * minimumDistance;
        }

        var needsExactProbe =
            a.Spec.Shape == TouchControlShape.GameCubeContour ||
            b.Spec.Shape == TouchControlShape.GameCubeContour ||
            // A rotated rectangle's screen-space box is larger than the shape, so two
            // turned controls can have intersecting boxes and disjoint regions exactly
            // as a bean and its neighbour do.
            a.Spec.VisualRotationDegrees != 0f ||
            b.Spec.VisualRotationDegrees != 0f;

        if (!needsExactProbe)
        {
            return true;
        }

        // A GameCube bean deliberately wraps around A: their boxes overlap in the
        // bean's empty concavity even though their answerable regions do not. Probe
        // only that small box intersection through the same hit tests used by input.
        var left = MathF.Max(a.CenterX - extentAx, b.CenterX - extentBx);
        var right = MathF.Min(a.CenterX + extentAx, b.CenterX + extentBx);
        var top = MathF.Max(a.CenterY - extentAy, b.CenterY - extentBy);
        var bottom = MathF.Min(a.CenterY + extentAy, b.CenterY + extentBy);
        var columns = Math.Max(1, (int)MathF.Ceiling((right - left) / ContourProbeStep));
        var rows = Math.Max(1, (int)MathF.Ceiling((bottom - top) / ContourProbeStep));

        for (var row = 0; row < rows; row++)
        {
            var y = top + ((row + 0.5f) * (bottom - top) / rows);
            for (var column = 0; column < columns; column++)
            {
                var x = left + ((column + 0.5f) * (right - left) / columns);
                var hitA = visualOnly ? a.ContainsVisual(x, y) : a.HitTest(x, y);
                if (!hitA)
                {
                    continue;
                }

                var hitB = visualOnly ? b.ContainsVisual(x, y) : b.HitTest(x, y);
                if (hitB)
                {
                    return true;
                }
            }
        }

        return false;
    }
}
