namespace PicoSwitch.Bridge.Touch;

/// <summary>A document plus whatever the operation created, so a host can select it.</summary>
public sealed record TouchEditResult(
    TouchLayoutDocument Document,
    /// <summary>Instances the operation brought into existence, in creation order.</summary>
    IReadOnlyList<string>? CreatedList = null,
    /// <summary>Set when the operation could not be performed, for the surface to show.</summary>
    string? Refusal = null)
{
    public IReadOnlyList<string> Created { get; } = CreatedList ?? [];

    public bool Changed => Refusal is null;
}

/// <summary>
/// Every pure operation an editor performs on a layout document.
///
/// ## Why there are no command objects
///
/// Undo/redo is a stack of DOCUMENTS (<see cref="TouchEditorHistory"/>), not a stack
/// of invertible commands, because every operation here is already a pure total
/// function from one document to the next. A revision stack cannot desynchronize from
/// the thing it is undoing, needs no inverse to be written (and kept correct) for each
/// new operation, and makes gesture coalescing a matter of when a revision is pushed
/// rather than of merging command objects.
///
/// ## Selections and groups
///
/// Every entry point takes a SELECTION of instance ids. <see cref="Expand"/> applies
/// group membership once, at the call site, so what a surface highlights and what an
/// edit actually moves are the same set — an editor that quietly moves four things
/// while outlining one is an editor nobody trusts.
///
/// ## Where geometry needs the region
///
/// Operations naturally expressed in what the user can see — drag by this many pixels,
/// scale this cluster about its centre — take the resolved layout, because the answer
/// genuinely depends on the rectangle on screen. The result is still stored in the
/// document's own aspect-independent terms. Purely declarative operations do not need
/// it and do not take it.
/// </summary>
public static class TouchLayoutEditor
{
    /// <summary>Kept as the editor's own names; the values live with the stored limits.</summary>
    public const float MinScale = TouchLayoutLimits.MinScale;

    public const float MaxScale = TouchLayoutLimits.MaxScale;

    /// <summary>How far a duplicate or a newly added control sits from its origin, in logical units.</summary>
    public const float PlacementStepUnits = 28f;

    /// <summary>Rotation lands exactly on a snap target within this many degrees of it.</summary>
    public const float RotationSnapDegrees = 6f;

    /// <summary>Anchors closer than this count as the same spot when placing a new control.</summary>
    private const float NearbyAnchor = 0.06f;

    public static TouchLayoutDocument AuthoredDefault(TouchControllerProfile profile) =>
        TouchLayoutDocument.AuthoredDefault(profile);

    /// <summary>
    /// The instances an operation on this selection actually touches.
    ///
    /// Public because a host has to draw it. With <paramref name="editGroup"/> off, a
    /// selection is literally itself, which is what makes editing one button of a
    /// cluster possible at all.
    /// </summary>
    public static IReadOnlySet<string> Expand(
        TouchLayoutDocument document, IReadOnlySet<string> selection, bool editGroup)
    {
        if (!editGroup)
        {
            return selection
                .Where(id => document.Instance(id) is not null)
                .ToHashSet(StringComparer.Ordinal);
        }

        var result = new HashSet<string>(StringComparer.Ordinal);
        foreach (var id in selection)
        {
            result.UnionWith(document.GroupMembers(id));
        }

        return result;
    }

    // ------------------------------------------------------------------- composition

    /// <summary>
    /// Create a new instance of a catalog entry.
    ///
    /// Placement has two cases, and the distinction matters:
    ///
    /// <code>
    /// the authored spot is free   put it exactly there
    /// something is already there  near fallbackAnchor, stepped clear
    /// </code>
    ///
    /// The first is what "add the control back" should mean — a personality's authored
    /// position for its grips is a considered piece of layout design. The second is what
    /// "give me another one of these" should mean: a duplicate created exactly
    /// underneath its twin looks like nothing happened.
    ///
    /// Group membership follows placement, and only placement: landing at the authored
    /// spot joins the authored cluster, anywhere else is free-standing. Restoring a
    /// deleted control should put it back completely, or "delete then re-add" would
    /// quietly leave the face diamond one member short of a group.
    /// </summary>
    public static TouchEditResult Add(
        TouchLayoutDocument document,
        TouchControllerProfile profile,
        string catalogId,
        float fallbackAnchorX,
        float fallbackAnchorY)
    {
        var entry = profile.CatalogEntry(catalogId);
        if (entry is null)
        {
            return new TouchEditResult(
                document, Refusal: $"This controller has no '{catalogId}'");
        }

        var authored = entry.Geometry;
        var instanceId = AllocateInstanceId(document, catalogId);

        TouchControlInstance instance;
        if (IsFree(document, authored.AnchorX, authored.AnchorY, authored))
        {
            instance = new TouchControlInstance
            {
                InstanceId = instanceId,
                CatalogId = entry.Id,
                AnchorX = authored.AnchorX,
                AnchorY = authored.AnchorY,
                OffsetXUnits = authored.GroupOffsetXUnits,
                OffsetYUnits = authored.GroupOffsetYUnits,
                ZIndex = NextZIndex(document),

                // Only when the group still exists in this document. Recreating a
                // cluster the user has entirely dissolved would resurrect a grouping
                // they deliberately took apart.
                GroupId = entry.EditGroupId is { } group &&
                          document.Controls.Any(control => control.GroupId == group)
                    ? group
                    : null,
            };
        }
        else
        {
            var step = PlacementStepUnits * Occupancy(document, fallbackAnchorX, fallbackAnchorY);
            instance = new TouchControlInstance
            {
                InstanceId = instanceId,
                CatalogId = entry.Id,
                AnchorX = Math.Clamp(
                    fallbackAnchorX, TouchLayoutLimits.MinAnchor, TouchLayoutLimits.MaxAnchor),
                AnchorY = Math.Clamp(
                    fallbackAnchorY, TouchLayoutLimits.MinAnchor, TouchLayoutLimits.MaxAnchor),
                OffsetXUnits = step,
                OffsetYUnits = step,
                ZIndex = NextZIndex(document),
            };
        }

        return new TouchEditResult(
            document with { Controls = [.. document.Controls, instance] },
            CreatedList: [instanceId]);
    }

    /// <summary>
    /// Clone the selected instances.
    ///
    /// Each clone keeps its source's transform, behaviour and group membership and is
    /// offset slightly so it is visibly a second object. Cloning a whole group produces
    /// a whole new group, because copying half a cluster into the original's group would
    /// silently change what the original group means.
    /// </summary>
    public static TouchEditResult Duplicate(
        TouchLayoutDocument document, IReadOnlySet<string> selection, bool editGroup)
    {
        var targets = Expand(document, selection, editGroup);
        if (targets.Count == 0)
        {
            return new TouchEditResult(document, Refusal: "Nothing is selected");
        }

        var next = document;
        var created = new List<string>();
        var groupMapping = new Dictionary<string, string>(StringComparer.Ordinal);

        foreach (var source in document.Controls.Where(c => targets.Contains(c.InstanceId)))
        {
            var instanceId = AllocateInstanceId(next, source.CatalogId);

            string? group = null;
            if (source.GroupId is { } original)
            {
                if (!groupMapping.TryGetValue(original, out group))
                {
                    group = AllocateGroupId(next);
                    groupMapping[original] = group;
                }
            }

            var clone = source with
            {
                InstanceId = instanceId,
                OffsetXUnits = source.OffsetXUnits + PlacementStepUnits,
                OffsetYUnits = source.OffsetYUnits + PlacementStepUnits,
                ZIndex = NextZIndex(next),
                GroupId = group,
            };

            next = next with { Controls = [.. next.Controls, clone] };
            created.Add(instanceId);
        }

        return new TouchEditResult(next, created);
    }

    /// <summary>
    /// Remove instances from the layout.
    ///
    /// Genuinely removed, not hidden: an absent instance does not exist, and Add Control
    /// is how one comes back. The whole selection goes in one operation so a single undo
    /// restores it as one thing.
    /// </summary>
    public static TouchEditResult Delete(
        TouchLayoutDocument document, IReadOnlySet<string> selection, bool editGroup)
    {
        var targets = Expand(document, selection, editGroup);
        if (targets.Count == 0)
        {
            return new TouchEditResult(document, Refusal: "Nothing is selected");
        }

        return new TouchEditResult(document with
        {
            Controls = document.Controls
                .Where(control => !targets.Contains(control.InstanceId))
                .ToList(),
        });
    }

    // ------------------------------------------------------------------------ groups

    /// <summary>
    /// Make the selection one group.
    ///
    /// Pure membership: no geometry changes at all, which is what makes grouping and
    /// ungrouping exactly reversible on any window shape. Members already in other
    /// groups are moved into this one — an instance belongs to at most one group,
    /// structurally, so there is no nested-group state to represent.
    /// </summary>
    public static TouchEditResult Group(
        TouchLayoutDocument document, IReadOnlySet<string> selection)
    {
        var targets = Expand(document, selection, editGroup: false);
        if (targets.Count < 2)
        {
            return new TouchEditResult(
                document, Refusal: "Select two or more controls to group them");
        }

        var groupId = AllocateGroupId(document);
        return new TouchEditResult(document with
        {
            Controls = document.Controls
                .Select(control => targets.Contains(control.InstanceId)
                    ? control with { GroupId = groupId }
                    : control)
                .ToList(),
        });
    }

    /// <summary>Drop group membership. Visually lossless, for the same reason Group is.</summary>
    public static TouchEditResult Ungroup(
        TouchLayoutDocument document, IReadOnlySet<string> selection)
    {
        var targets = Expand(document, selection, editGroup: true);
        if (!targets.Any(id => document.Instance(id)?.GroupId is not null))
        {
            return new TouchEditResult(
                document, Refusal: "Nothing in the selection is grouped");
        }

        return new TouchEditResult(document with
        {
            Controls = document.Controls
                .Select(control => targets.Contains(control.InstanceId)
                    ? control with { GroupId = null }
                    : control)
                .ToList(),
        });
    }

    // --------------------------------------------------------------------- transforms

    /// <summary>
    /// Drag the selection by a screen-space delta.
    ///
    /// One clamp for the WHOLE selection rather than per member: clamping each member
    /// after the move would compress a cluster against an edge and silently destroy the
    /// relative spacing that makes it a cluster.
    /// </summary>
    public static TouchLayoutDocument Move(
        TouchLayoutDocument document,
        ResolvedTouchLayout resolved,
        IReadOnlySet<string> selection,
        float deltaX,
        float deltaY,
        bool editGroup)
    {
        if (!float.IsFinite(deltaX) || !float.IsFinite(deltaY))
        {
            return document;
        }

        var targets = Expand(document, selection, editGroup);
        if (targets.Count == 0)
        {
            return document;
        }

        var region = resolved.Region;
        if (region.Width <= 0f || region.Height <= 0f)
        {
            return document;
        }

        var placed = targets.Select(resolved.Control).OfType<ResolvedTouchControl>().ToList();
        if (placed.Count == 0)
        {
            return document;
        }

        var allowedX = Math.Clamp(
            deltaX,
            placed.Max(c => region.Left + c.HitExtentX - c.CenterX),
            placed.Min(c => region.Right - c.HitExtentX - c.CenterX));
        var allowedY = Math.Clamp(
            deltaY,
            placed.Max(c => region.Top + c.HitExtentY - c.CenterY),
            placed.Min(c => region.Bottom - c.HitExtentY - c.CenterY));

        var normalizedX = allowedX / region.Width;
        var normalizedY = allowedY / region.Height;

        return Update(document, targets, instance => instance with
        {
            AnchorX = Math.Clamp(instance.AnchorX + normalizedX,
                TouchLayoutLimits.MinAnchor, TouchLayoutLimits.MaxAnchor),
            AnchorY = Math.Clamp(instance.AnchorY + normalizedY,
                TouchLayoutLimits.MinAnchor, TouchLayoutLimits.MaxAnchor),
        });
    }

    /// <summary>
    /// Put the selection at an absolute normalized anchor.
    ///
    /// The precise-entry counterpart to <see cref="Move"/>: no region, no clamping
    /// against neighbours, just "this control's anchor is now here". The audit still has
    /// the final word on whether the result is playable.
    /// </summary>
    public static TouchLayoutDocument Place(
        TouchLayoutDocument document,
        IReadOnlySet<string> selection,
        float anchorX,
        float anchorY)
    {
        if (!float.IsFinite(anchorX) || !float.IsFinite(anchorY))
        {
            return document;
        }

        return Update(document, Expand(document, selection, editGroup: false), instance =>
            instance with
            {
                AnchorX = Math.Clamp(
                    anchorX, TouchLayoutLimits.MinAnchor, TouchLayoutLimits.MaxAnchor),
                AnchorY = Math.Clamp(
                    anchorY, TouchLayoutLimits.MinAnchor, TouchLayoutLimits.MaxAnchor),
            });
    }

    /// <summary>
    /// Multiply the selection's size, about the selection's own centroid.
    ///
    /// Relative rather than absolute because a pinch has no absolute value to report,
    /// and because a multi-selection has no single current size: one absolute scale
    /// would flatten sizes the user deliberately made different on the very first pinch.
    ///
    /// A single control simply grows in place — its centroid is itself — while a
    /// cluster's members also move apart. The displacement is written into the
    /// aspect-independent offset, so the scaled cluster stays rigid on every other window
    /// shape.
    /// </summary>
    public static TouchLayoutDocument ScaleBy(
        TouchLayoutDocument document,
        ResolvedTouchLayout resolved,
        IReadOnlySet<string> selection,
        float factor,
        bool editGroup)
    {
        if (!float.IsFinite(factor) || factor <= 0f)
        {
            return document;
        }

        var targets = Expand(document, selection, editGroup);
        if (targets.Count == 0)
        {
            return document;
        }

        var unit = resolved.Region.UnitScale * resolved.Scale;
        var centroid = Centroid(resolved, targets);

        return Update(document, targets, instance =>
        {
            var applied = Math.Clamp(instance.Scale * factor, MinScale, MaxScale);

            // The applied factor may be clipped by the size limits; move the member by
            // what actually happened, never by what was asked for, or a cluster at its
            // size limit slowly tears itself apart.
            var effective = instance.Scale > 0f ? applied / instance.Scale : 1f;
            var placed = resolved.Control(instance.InstanceId);

            if (placed is null || centroid is not { } centre || unit <= 0f)
            {
                return instance with { Scale = applied };
            }

            return instance with
            {
                Scale = applied,
                OffsetXUnits = instance.OffsetXUnits +
                    ((placed.CenterX - centre.X) * (effective - 1f) / unit),
                OffsetYUnits = instance.OffsetYUnits +
                    ((placed.CenterY - centre.Y) * (effective - 1f) / unit),
            };
        });
    }

    /// <summary>Set an absolute size multiplier; used by numeric entry and the size buttons.</summary>
    public static TouchLayoutDocument SetScale(
        TouchLayoutDocument document,
        IReadOnlySet<string> selection,
        float scale,
        bool editGroup)
    {
        if (!float.IsFinite(scale))
        {
            return document;
        }

        var applied = Math.Clamp(scale, MinScale, MaxScale);
        return Update(document, Expand(document, selection, editGroup),
            instance => instance with { Scale = applied });
    }

    /// <summary>
    /// Turn the selection about its centroid.
    ///
    /// Both halves, together: each member's own orientation gains the angle and its
    /// position rotates about the shared centre, so a grouped cluster behaves like one
    /// turned object rather than like several independently spinning ones.
    ///
    /// Purely presentational. Nothing here can change a binding, a D-pad direction, or an
    /// analog trigger's travel axis.
    /// </summary>
    public static TouchLayoutDocument RotateBy(
        TouchLayoutDocument document,
        ResolvedTouchLayout resolved,
        IReadOnlySet<string> selection,
        float degrees,
        bool editGroup)
    {
        if (!float.IsFinite(degrees) || degrees == 0f)
        {
            return document;
        }

        var targets = Expand(document, selection, editGroup);
        if (targets.Count == 0)
        {
            return document;
        }

        var unit = resolved.Region.UnitScale * resolved.Scale;
        var centroid = Centroid(resolved, targets);
        var radians = degrees * Math.PI / 180d;
        var cosine = (float)Math.Cos(radians);
        var sine = (float)Math.Sin(radians);

        return Update(document, targets, instance =>
        {
            var turned = instance with
            {
                RotationDegrees = TouchLayoutLimits.NormalizeRotation(
                    instance.RotationDegrees + degrees),
            };

            var placed = resolved.Control(instance.InstanceId);
            if (placed is null || centroid is not { } centre || unit <= 0f)
            {
                return turned;
            }

            var dx = placed.CenterX - centre.X;
            var dy = placed.CenterY - centre.Y;
            return turned with
            {
                OffsetXUnits = instance.OffsetXUnits +
                    (((dx * cosine) - (dy * sine) - dx) / unit),
                OffsetYUnits = instance.OffsetYUnits +
                    (((dx * sine) + (dy * cosine) - dy) / unit),
            };
        });
    }

    /// <summary>
    /// Set one control's own orientation, relative to its authored one.
    ///
    /// Deliberately NOT a group operation: "every member of this cluster is now at 30
    /// degrees" is a different and much less useful statement than "turn this cluster by
    /// 30 degrees", and offering the first through the same control would make the second
    /// unreachable.
    /// </summary>
    public static TouchLayoutDocument SetRotation(
        TouchLayoutDocument document, IReadOnlySet<string> selection, float degrees)
    {
        if (!float.IsFinite(degrees))
        {
            return document;
        }

        var applied = TouchLayoutLimits.NormalizeRotation(degrees);
        return Update(document, Expand(document, selection, editGroup: false),
            instance => instance with { RotationDegrees = applied });
    }

    /// <summary>Put the selection back to the orientation the catalog authored for it.</summary>
    public static TouchLayoutDocument ResetRotation(
        TouchLayoutDocument document, IReadOnlySet<string> selection, bool editGroup) =>
        Update(document, Expand(document, selection, editGroup),
            instance => instance with { RotationDegrees = 0f });

    /// <summary>
    /// The nearest angle a live rotation should settle on, or the angle itself.
    ///
    /// Magnetic rather than discrete: the user keeps every angle in between and only the
    /// useful ones — the authored orientation and its quarter turns — pull. Expressed
    /// against the instance's own rotation, which is already relative to the authored
    /// angle, so "0" IS the authored orientation and the arithmetic needs no special case
    /// for a bean that ships at 10.7 degrees.
    /// </summary>
    public static float SnapRotation(float degrees)
    {
        if (!float.IsFinite(degrees))
        {
            return 0f;
        }

        var normalized = TouchLayoutLimits.NormalizeRotation(degrees);
        var quarter = MathF.Round(normalized / 90f, MidpointRounding.AwayFromZero) * 90f;
        return MathF.Abs(normalized - quarter) <= RotationSnapDegrees
            ? TouchLayoutLimits.NormalizeRotation(quarter)
            : normalized;
    }

    /// <summary>
    /// The rotation delta to actually apply, so that the primary control lands exactly on
    /// a snap target whenever the gesture brings it near one.
    ///
    /// Computed from ONE reference control and returned as a delta for the whole
    /// selection, for the same reason movement snapping works that way.
    /// </summary>
    /// <param name="degrees">This frame's raw angle change. Applied as-is when there is no reference.</param>
    /// <param name="intentDegrees">
    /// Where the GESTURE has turned to: the total raw angle it has described since it
    /// began, with <paramref name="degrees"/> already included and every snap applied so
    /// far ignored.
    ///
    /// A separate number from the stored angle, and that separation is the whole point.
    /// Deriving the target from stored + degrees instead means a control sitting inside a
    /// snap zone can never leave it: each frame proposes a fraction of a degree, the
    /// magnet pulls it back to the same target, the stored angle never moves, and the
    /// next frame asks the identical question. Rotation then only escapes if one single
    /// frame carries more than the snap radius — which is why turning a control used to
    /// need a whole-hand flick instead of a wrist.
    /// </param>
    public static float SnappedRotationDelta(
        TouchLayoutDocument document, string? primaryId, float degrees, float intentDegrees)
    {
        if (!float.IsFinite(degrees) || !float.IsFinite(intentDegrees))
        {
            return 0f;
        }

        if (primaryId is null || document.Instance(primaryId) is not { } primary)
        {
            return degrees;
        }

        var target = SnapRotation(intentDegrees);

        // Back through normalization so a snap across the +/-180 seam is the short way
        // round rather than a 359-degree spin.
        return TouchLayoutLimits.NormalizeRotation(target - primary.RotationDegrees);
    }

    // ----------------------------------------------------------------------- z-order

    public static TouchLayoutDocument BringToFront(
        TouchLayoutDocument document, IReadOnlySet<string> selection, bool editGroup) =>
        Restack(document, Expand(document, selection, editGroup), toFront: true);

    public static TouchLayoutDocument SendToBack(
        TouchLayoutDocument document, IReadOnlySet<string> selection, bool editGroup) =>
        Restack(document, Expand(document, selection, editGroup), toFront: false);

    public static TouchLayoutDocument BringForward(
        TouchLayoutDocument document, IReadOnlySet<string> selection, bool editGroup) =>
        NudgeStack(document, Expand(document, selection, editGroup), forward: true);

    public static TouchLayoutDocument SendBackward(
        TouchLayoutDocument document, IReadOnlySet<string> selection, bool editGroup) =>
        NudgeStack(document, Expand(document, selection, editGroup), forward: false);

    // ---------------------------------------------------------------------- behaviour

    /// <summary>
    /// Choose whether these controls answer to the hold gestures.
    ///
    /// <c>null</c> means "follow the global setting" and DROPS the stored answer rather
    /// than freezing whatever the setting happens to say today.
    ///
    /// Per instance, not per binding. Two A buttons may hold differently, and that is a
    /// coherent thing to want — one to lean on, one to tap.
    /// </summary>
    public static TouchLayoutDocument SetLatch(
        TouchLayoutDocument document,
        TouchControllerProfile profile,
        IReadOnlySet<string> selection,
        bool? latch,
        bool editGroup) =>
        Update(document, Expand(document, selection, editGroup), instance =>
        {
            var entry = profile.CatalogEntry(instance.CatalogId);
            return entry?.Interaction.SupportsLatch() == true
                ? instance with { Latch = latch }
                : instance;
        });

    // -------------------------------------------------------------------------- reset

    /// <summary>
    /// Restore the authored transform of the selected instances.
    ///
    /// Only instances whose id is still a catalog id can be reset — those are the ones
    /// the authored default has an opinion about. A duplicate the user created has no
    /// authored position, so it keeps the one it has; resetting it to the original's
    /// place would silently stack the two.
    /// </summary>
    public static TouchLayoutDocument Reset(
        TouchLayoutDocument document,
        TouchControllerProfile profile,
        IReadOnlySet<string> selection,
        bool editGroup)
    {
        var authored = TouchLayoutDocument.AuthoredDefault(profile);
        return Update(document, Expand(document, selection, editGroup), instance =>
            authored.Instance(instance.InstanceId) is { } original
                ? original with { ZIndex = instance.ZIndex }
                : instance);
    }

    /// <summary>Replace the whole layout with a fresh copy of the shipped one.</summary>
    public static TouchLayoutDocument ResetAll(TouchControllerProfile profile) =>
        TouchLayoutDocument.AuthoredDefault(profile);

    // ---------------------------------------------------------------------- internals

    private static TouchLayoutDocument Update(
        TouchLayoutDocument document,
        IReadOnlySet<string> targets,
        Func<TouchControlInstance, TouchControlInstance> transform)
    {
        if (targets.Count == 0)
        {
            return document;
        }

        return document with
        {
            Controls = document.Controls
                .Select(control => targets.Contains(control.InstanceId)
                    ? transform(control)
                    : control)
                .ToList(),
        };
    }

    /// <summary>Centre of the selection's placed controls, in screen coordinates.</summary>
    private static TouchVector? Centroid(
        ResolvedTouchLayout resolved, IReadOnlySet<string> targets)
    {
        var placed = targets.Select(resolved.Control).OfType<ResolvedTouchControl>().ToList();
        if (placed.Count == 0)
        {
            return null;
        }

        return new TouchVector(
            (float)(placed.Sum(c => (double)c.CenterX) / placed.Count),
            (float)(placed.Sum(c => (double)c.CenterY) / placed.Count));
    }

    /// <summary>
    /// A readable, deterministic instance id.
    ///
    /// <c>dpad</c>, then <c>dpad#2</c>, <c>dpad#3</c>. Derived from the document rather
    /// than randomly generated so every operation here stays a pure function and the
    /// migration fixtures mean something; readable so a stored document can be understood
    /// by a person looking at it.
    /// </summary>
    internal static string AllocateInstanceId(TouchLayoutDocument document, string catalogId)
    {
        var taken = document.Controls
            .Select(control => control.InstanceId)
            .ToHashSet(StringComparer.Ordinal);

        if (!taken.Contains(catalogId))
        {
            return catalogId;
        }

        var suffix = 2;
        while (taken.Contains($"{catalogId}#{suffix}"))
        {
            suffix++;
        }

        return $"{catalogId}#{suffix}";
    }

    internal static string AllocateGroupId(TouchLayoutDocument document)
    {
        var taken = document.Controls
            .Select(control => control.GroupId)
            .OfType<string>()
            .ToHashSet(StringComparer.Ordinal);

        var suffix = 1;
        while (taken.Contains($"group-{suffix}"))
        {
            suffix++;
        }

        return $"group-{suffix}";
    }

    private static int NextZIndex(TouchLayoutDocument document) =>
        (document.Controls.Count == 0 ? -1 : document.Controls.Max(c => c.ZIndex)) + 1;

    /// <summary>
    /// How crowded a placement point already is, in units of one nudge.
    ///
    /// Counts what is near the target so the second, third and fourth copy of a control
    /// step further out instead of piling up on each other.
    /// </summary>
    private static float Occupancy(
        TouchLayoutDocument document, float anchorX, float anchorY) =>
        document.Controls.Count(control => Near(control, anchorX, anchorY, 0f, 0f));

    /// <summary>True when nothing already sits at this approximate place.</summary>
    private static bool IsFree(
        TouchLayoutDocument document, float anchorX, float anchorY, TouchControlGeometry geometry) =>
        !document.Controls.Any(control => Near(
            control, anchorX, anchorY, geometry.GroupOffsetXUnits, geometry.GroupOffsetYUnits));

    /// <summary>
    /// Whether an instance's approximate centre coincides with a target place.
    ///
    /// Anchor AND offset, converted to one comparable normalized point through the
    /// authoring reference shape. An anchor-only comparison would call every member of
    /// the face diamond "the same place", because they share one.
    /// </summary>
    private static bool Near(
        TouchControlInstance instance,
        float anchorX,
        float anchorY,
        float offsetXUnits,
        float offsetYUnits)
    {
        var dx = (instance.AnchorX - anchorX) +
            ((instance.OffsetXUnits - offsetXUnits) / TouchLayoutResolver.ReferenceWidthUnits);
        var dy = (instance.AnchorY - anchorY) +
            ((instance.OffsetYUnits - offsetYUnits) / TouchLayoutResolver.ReferenceHeightUnits);
        return MathF.Abs(dx) < NearbyAnchor && MathF.Abs(dy) < NearbyAnchor;
    }

    private static TouchLayoutDocument Restack(
        TouchLayoutDocument document, IReadOnlySet<string> targets, bool toFront)
    {
        if (targets.Count == 0)
        {
            return document;
        }

        var ordered = document.Controls.OrderBy(control => control.ZIndex).ToList();
        var moved = ordered.Where(c => targets.Contains(c.InstanceId)).ToList();
        var rest = ordered.Where(c => !targets.Contains(c.InstanceId)).ToList();
        var sequence = toFront ? [.. rest, .. moved] : new List<TouchControlInstance>([.. moved, .. rest]);
        return document with { Controls = Renumber(document, sequence) };
    }

    private static TouchLayoutDocument NudgeStack(
        TouchLayoutDocument document, IReadOnlySet<string> targets, bool forward)
    {
        if (targets.Count == 0)
        {
            return document;
        }

        var ordered = document.Controls.OrderBy(control => control.ZIndex).ToList();

        // Walk from the end the move is heading toward, so a contiguous run of selected
        // controls slides as a block instead of collapsing into itself.
        var indices = forward
            ? Enumerable.Range(0, ordered.Count).Reverse()
            : Enumerable.Range(0, ordered.Count);

        foreach (var index in indices)
        {
            if (!targets.Contains(ordered[index].InstanceId))
            {
                continue;
            }

            var swap = forward ? index + 1 : index - 1;
            if (swap < 0 || swap >= ordered.Count || targets.Contains(ordered[swap].InstanceId))
            {
                continue;
            }

            (ordered[index], ordered[swap]) = (ordered[swap], ordered[index]);
        }

        return document with { Controls = Renumber(document, ordered) };
    }

    /// <summary>Reassign a dense z sequence and restore the document's own control order.</summary>
    private static List<TouchControlInstance> Renumber(
        TouchLayoutDocument document, List<TouchControlInstance> sequence)
    {
        var z = sequence
            .Select((instance, index) => (instance.InstanceId, index))
            .ToDictionary(pair => pair.InstanceId, pair => pair.index, StringComparer.Ordinal);

        return document.Controls
            .Select(control => z.TryGetValue(control.InstanceId, out var index)
                ? control with { ZIndex = index }
                : control)
            .ToList();
    }
}

/// <summary>
/// Undo/redo for one editor session.
///
/// A bounded stack of document revisions with a label each. Push once per completed
/// gesture rather than once per pointer frame — the working document is authoritative
/// during a drag and only the endpoints are worth remembering.
///
/// Deliberately session-scoped: the layout itself is persisted normally, and carrying a
/// command history across process death would be a second thing that can be corrupt for
/// no benefit a user would notice.
/// </summary>
public sealed class TouchEditorHistory(
    TouchLayoutDocument initial,
    int limit = TouchEditorHistory.DefaultLimit)
{
    /// <summary>Long enough for a real editing session, short enough to stay bounded.</summary>
    public const int DefaultLimit = 64;

    private readonly List<(TouchLayoutDocument Document, string Label)> past = [];
    private readonly List<(TouchLayoutDocument Document, string Label)> future = [];

    public TouchLayoutDocument Current { get; private set; } = initial;

    public bool CanUndo => past.Count > 0;

    public bool CanRedo => future.Count > 0;

    /// <summary>What undo would take back, for a menu item that can name itself.</summary>
    public string? UndoLabel => past.Count > 0 ? past[^1].Label : null;

    /// <summary>
    /// Adopt a new revision.
    ///
    /// A no-op change pushes nothing, so a gesture that ended where it started does not
    /// leave an undo step that appears to do nothing.
    /// </summary>
    public void Push(TouchLayoutDocument next, string label)
    {
        if (next == Current)
        {
            return;
        }

        past.Add((Current, label));
        while (past.Count > limit)
        {
            past.RemoveAt(0);
        }

        future.Clear();
        Current = next;
    }

    public TouchLayoutDocument? Undo()
    {
        if (past.Count == 0)
        {
            return null;
        }

        var revision = past[^1];
        past.RemoveAt(past.Count - 1);
        future.Add((Current, revision.Label));
        Current = revision.Document;
        return Current;
    }

    public TouchLayoutDocument? Redo()
    {
        if (future.Count == 0)
        {
            return null;
        }

        var revision = future[^1];
        future.RemoveAt(future.Count - 1);
        past.Add((Current, revision.Label));
        Current = revision.Document;
        return Current;
    }

    /// <summary>Adopt a document from outside the editor, discarding the history it invalidates.</summary>
    public void Reset(TouchLayoutDocument document)
    {
        past.Clear();
        future.Clear();
        Current = document;
    }
}
