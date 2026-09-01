namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// Bounds every stored instance transform has to satisfy.
///
/// Here rather than on the editor because the CODEC needs them too: a document
/// arriving from storage or an import must pass exactly the checks an editor
/// operation would have enforced, or a hand-edited file becomes a way to construct
/// geometry the editor refuses to make.
/// </summary>
public static class TouchLayoutLimits
{
    public const float MinScale = 0.55f;

    public const float MaxScale = 1.75f;

    /// <summary>
    /// How far a control's own anchor may sit outside the interaction rectangle.
    ///
    /// Zero: the anchor is the control's group origin and the audit already refuses
    /// a control whose answerable region leaves the safe area, so a document that
    /// stored an out-of-range anchor could only ever describe a layout that refuses
    /// to load.
    /// </summary>
    public const float MinAnchor = 0f;

    public const float MaxAnchor = 1f;

    /// <summary>
    /// The largest offset a grouped member may carry from its anchor, in logical
    /// units.
    ///
    /// Generous — a full reference width either way — because a cluster is allowed
    /// to be as wide as the controller. It exists to reject nonsense (a document
    /// claiming 10^9 units) rather than to constrain composition.
    /// </summary>
    public const float MaxOffsetUnits = TouchLayoutResolver.ReferenceWidthUnits;

    /// <summary>Normalize a rotation to <c>[-180, 180)</c>, the one representation stored.</summary>
    public static float NormalizeRotation(float degrees)
    {
        if (!float.IsFinite(degrees))
        {
            return 0f;
        }

        var value = degrees % 360f;
        if (value >= 180f)
        {
            value -= 360f;
        }

        if (value < -180f)
        {
            value += 360f;
        }

        // -0f compares equal to 0f but serializes differently; pin it.
        return value == 0f ? 0f : value;
    }
}

/// <summary>
/// One independently identifiable on-screen object.
///
/// ## Instance identity is not logical button identity
///
/// <see cref="InstanceId"/> is the object; <see cref="CatalogId"/> is what kind of
/// object it is. Two instances may name the same catalog entry, and therefore the
/// same logical binding, and remain completely separate things: separate hit
/// region, separate transform, separate latch, separate contact. That separation is
/// the whole point of the instance model and the one invariant most likely to be
/// accidentally undone by a future "simplification" that keys something by binding.
///
/// ## Position
///
/// <code>
/// centre = region.Left + AnchorX * region.Width + OffsetXUnits * unit
/// </code>
///
/// Two terms because they behave differently under a change of window shape.
/// <see cref="AnchorX"/>/<see cref="AnchorY"/> are NORMALIZED, so they follow the
/// rectangle and keep a control at the edge a thumb can reach.
/// <see cref="OffsetXUnits"/>/<see cref="OffsetYUnits"/> are in LOGICAL UNITS, so
/// they are immune to aspect-ratio distortion — which is what keeps a square button
/// diamond square on a display the layout was not authored for.
///
/// Group transforms exploit exactly that: grouping and ungrouping are pure
/// <see cref="GroupId"/> changes and never touch geometry, while a group scale or
/// rotation writes the displacement into the offsets, where it stays rigid.
///
/// ## Rotation
///
/// <see cref="RotationDegrees"/> is the USER's rotation, relative to the catalog
/// entry's authored orientation. Stored that way so "reset orientation" means the
/// authored value rather than a blind zero, and so re-authoring a control's art
/// direction moves every existing instance with it.
///
/// It is VISUAL AND HIT geometry only. Rotating a control never changes what it
/// sends, never re-labels a D-pad direction, and never turns an analog trigger's
/// position-derived travel axis.
/// </summary>
public sealed record TouchControlInstance
{
    public required string InstanceId { get; init; }

    public required string CatalogId { get; init; }

    /// <summary>Group origin, or the control's own centre when it has no offset.</summary>
    public required float AnchorX { get; init; }

    public required float AnchorY { get; init; }

    /// <summary>Aspect-independent displacement from the anchor, in logical units.</summary>
    public float OffsetXUnits { get; init; }

    public float OffsetYUnits { get; init; }

    public float Scale { get; init; } = 1f;

    /// <summary>Clockwise, relative to the catalog entry's authored orientation.</summary>
    public float RotationDegrees { get; init; }

    /// <summary>Higher draws and hit-tests in front.</summary>
    public int ZIndex { get; init; }

    /// <summary>
    /// At most one group; membership is a property OF the instance, so an instance
    /// cannot be in two groups and a group cannot dangle.
    /// </summary>
    public string? GroupId { get; init; }

    /// <summary><c>null</c> follows the global hold-to-latch setting.</summary>
    public bool? Latch { get; init; }
}

/// <summary>
/// A complete user layout: a scene of instances, not a patch over a stencil.
///
/// ## Default layout membership is not personality capability
///
/// A control absent from <see cref="Controls"/> is not on screen. It is still
/// perfectly available — the personality's catalog says what may be instantiated,
/// and Add Control reads that. The two questions are separate and must stay
/// separate; conflating them is what the pre-2.0 "hidden template control" model
/// did, and it is why an optional control could not be genuinely deleted.
/// </summary>
public sealed record TouchLayoutDocument
{
    /// <summary>
    /// Instance-based documents.
    ///
    /// Version 1 was the sparse override map over an immutable template. Nothing
    /// writes it any more; <see cref="TouchLayoutMigration"/> converts one, and the
    /// legacy decoder exists only to feed that conversion.
    /// </summary>
    public const int CurrentSchemaVersion = 2;

    private Dictionary<string, TouchControlInstance>? index;

    public int SchemaVersion { get; init; } = CurrentSchemaVersion;

    public required TouchProfileId ProfileId { get; init; }

    public required string TemplateId { get; init; }

    public required int BasedOnRevision { get; init; }

    public IReadOnlyList<TouchControlInstance> Controls { get; init; } = [];

    private Dictionary<string, TouchControlInstance> Index =>
        index ??= Controls
            .GroupBy(control => control.InstanceId, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);

    public TouchControlInstance? Instance(string instanceId) =>
        Index.TryGetValue(instanceId, out var instance) ? instance : null;

    /// <summary>Instance ids in each group, in document order. Derived; never stored twice.</summary>
    public IReadOnlyDictionary<string, IReadOnlyList<string>> Groups =>
        Controls
            .Where(control => control.GroupId is not null)
            .GroupBy(control => control.GroupId!, StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => (IReadOnlyList<string>)group.Select(c => c.InstanceId).ToList(),
                StringComparer.Ordinal);

    /// <summary>Members of the group <paramref name="instanceId"/> belongs to, or just itself.</summary>
    public IReadOnlySet<string> GroupMembers(string instanceId)
    {
        var instance = Instance(instanceId);
        if (instance?.GroupId is not { } group)
        {
            return instance is null
                ? new HashSet<string>(StringComparer.Ordinal)
                : new HashSet<string>(StringComparer.Ordinal) { instance.InstanceId };
        }

        return Controls
            .Where(control => control.GroupId == group)
            .Select(control => control.InstanceId)
            .ToHashSet(StringComparer.Ordinal);
    }

    /// <summary>
    /// The shipped starting point for <paramref name="profile"/>, as an ordinary
    /// document.
    ///
    /// A normal value with no special status at runtime — Reset Layout is literally
    /// "replace the document with a fresh copy of this one", which is why the
    /// factory layout can never be damaged by an edit.
    ///
    /// Instance ids are the catalog ids. Deterministic, readable in a stored
    /// document, and stable across app versions, which is what makes the migration
    /// and the golden fixtures meaningful.
    /// </summary>
    public static TouchLayoutDocument AuthoredDefault(TouchControllerProfile profile)
    {
        var template = profile.DefaultTemplate;
        var placed = template.Controls.Where(control => control.InDefaultLayout).ToList();

        return new TouchLayoutDocument
        {
            ProfileId = profile.Id,
            TemplateId = template.Id,
            BasedOnRevision = template.TemplateRevision,
            Controls = placed.Select((control, index) => new TouchControlInstance
            {
                InstanceId = control.Id,
                CatalogId = control.Id,
                AnchorX = control.Geometry.AnchorX,
                AnchorY = control.Geometry.AnchorY,
                OffsetXUnits = control.Geometry.GroupOffsetXUnits,
                OffsetYUnits = control.Geometry.GroupOffsetYUnits,
                ZIndex = index,
                GroupId = control.EditGroupId,
            }).ToList(),
        };
    }
}

/// <summary>One thing wrong with a stored document, and whether it cost an instance.</summary>
public sealed record TouchDocumentFinding(string Message, bool Dropped);

/// <summary>
/// A document after validation, plus what had to be changed to make it usable.
///
/// Repair rather than refusal wherever a repair is unambiguous: one corrupt instance
/// must not cost the user a whole layout. <see cref="Degraded"/> is true whenever
/// anything was dropped, so a surface can say so instead of silently presenting a
/// layout that is missing a control.
/// </summary>
public sealed record TouchDocumentValidation(
    TouchLayoutDocument Document,
    IReadOnlyList<TouchDocumentFinding>? FindingList = null)
{
    public IReadOnlyList<TouchDocumentFinding> Findings { get; } = FindingList ?? [];

    public bool Degraded => Findings.Any(finding => finding.Dropped);

    public string? Problem => Findings.Count > 0 ? Findings[0].Message : null;
}

/// <summary>
/// Structural validation and repair for a stored or imported document.
///
/// Everything here is about the DOCUMENT being well formed — identity, catalog
/// references, numeric sanity. Whether the resulting arrangement is playable is a
/// separate question answered by <see cref="TouchLayoutAudit"/> against resolved
/// geometry, and the two must not be merged: a layout can be perfectly well formed
/// and still have two controls on top of each other.
/// </summary>
public static class TouchLayoutDocumentValidator
{
    public static TouchDocumentValidation Validate(
        TouchLayoutDocument document, TouchControllerProfile profile)
    {
        if (document.ProfileId != profile.Id)
        {
            return new TouchDocumentValidation(
                TouchLayoutDocument.AuthoredDefault(profile),
                [new TouchDocumentFinding("That layout belongs to another controller", true)]);
        }

        var findings = new List<TouchDocumentFinding>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var repaired = new List<TouchControlInstance>();

        foreach (var instance in document.Controls)
        {
            if (string.IsNullOrWhiteSpace(instance.InstanceId))
            {
                findings.Add(new TouchDocumentFinding("A control has no identity", true));
                continue;
            }

            if (!seen.Add(instance.InstanceId))
            {
                findings.Add(new TouchDocumentFinding(
                    $"Control '{instance.InstanceId}' appears more than once", true));
                continue;
            }

            var entry = profile.CatalogEntry(instance.CatalogId);
            if (entry is null)
            {
                // The catalog no longer has this control — an app downgrade, or a
                // personality that dropped it. Only this instance is lost; the rest of
                // the document, and every OTHER instance id, is intact.
                findings.Add(new TouchDocumentFinding(
                    $"This controller no longer has a '{instance.CatalogId}' control", true));
                continue;
            }

            var clean = Repair(instance, entry);
            if (clean is null)
            {
                findings.Add(new TouchDocumentFinding(
                    $"Control '{instance.InstanceId}' has an impossible position", true));
                continue;
            }

            repaired.Add(clean);
        }

        return new TouchDocumentValidation(document with { Controls = repaired }, findings);
    }

    /// <summary>
    /// Bring one instance inside the stored limits, or reject it.
    ///
    /// Values that are merely out of range are clamped: a document written by a build
    /// with different limits is still describing something the user made. A value
    /// that is not a number at all is rejected, because there is no defensible
    /// position to clamp it to and drawing at NaN takes the whole layout with it.
    /// </summary>
    private static TouchControlInstance? Repair(
        TouchControlInstance instance, TouchTemplateControl entry)
    {
        var finite = float.IsFinite(instance.AnchorX) && float.IsFinite(instance.AnchorY) &&
            float.IsFinite(instance.OffsetXUnits) && float.IsFinite(instance.OffsetYUnits) &&
            float.IsFinite(instance.Scale) && float.IsFinite(instance.RotationDegrees);
        if (!finite)
        {
            return null;
        }

        return instance with
        {
            AnchorX = Math.Clamp(instance.AnchorX, TouchLayoutLimits.MinAnchor, TouchLayoutLimits.MaxAnchor),
            AnchorY = Math.Clamp(instance.AnchorY, TouchLayoutLimits.MinAnchor, TouchLayoutLimits.MaxAnchor),
            OffsetXUnits = Math.Clamp(
                instance.OffsetXUnits, -TouchLayoutLimits.MaxOffsetUnits, TouchLayoutLimits.MaxOffsetUnits),
            OffsetYUnits = Math.Clamp(
                instance.OffsetYUnits, -TouchLayoutLimits.MaxOffsetUnits, TouchLayoutLimits.MaxOffsetUnits),
            Scale = Math.Clamp(instance.Scale, TouchLayoutLimits.MinScale, TouchLayoutLimits.MaxScale),
            RotationDegrees = TouchLayoutLimits.NormalizeRotation(instance.RotationDegrees),
            GroupId = string.IsNullOrWhiteSpace(instance.GroupId) ? null : instance.GroupId,

            // A control that cannot hold must not carry a stored opinion about
            // holding: it would be a setting the editor never shows and nothing ever
            // reads.
            Latch = entry.Interaction.SupportsLatch() ? instance.Latch : null,
        };
    }
}

/// <summary>
/// Version 1 sparse override -> version 2 instance document.
///
/// Deterministic and one-way. Given the same template and the same stored override
/// this always produces the same document, including the same instance ids, which
/// is what makes the golden migration fixtures worth having.
///
/// <code>
/// legacy control hidden      -> no instance at all; Add Control can bring it back
/// legacy control visible     -> one instance, id = the template control id
/// anchor override            -> the instance's anchor
/// scale override             -> the instance's scale
/// groupOffsetScale override  -> baked into the instance's offset
/// latch override             -> the instance's latch
/// template editGroupId       -> the instance's group
/// </code>
///
/// Overrides naming a control the template no longer has are dropped, exactly as the
/// version 1 composer already ignored them.
/// </summary>
public static class TouchLayoutMigration
{
    public static TouchLayoutDocument FromOverride(
        TouchControllerProfile profile, TouchLayoutOverride @override)
    {
        var template = profile.DefaultTemplate;
        if (@override.ProfileId != profile.Id || @override.TemplateId != template.Id)
        {
            return TouchLayoutDocument.AuthoredDefault(profile);
        }

        var controls = new List<TouchControlInstance>();
        for (var index = 0; index < template.Controls.Count; index++)
        {
            var control = template.Controls[index];
            @override.Controls.TryGetValue(control.Id, out var change);

            // The version 1 visibility rule, unchanged: absent means the template's
            // own answer, and for an optional control that answer is "not placed".
            var visible = change?.Visible ?? control.InDefaultLayout;
            if (!visible)
            {
                continue;
            }

            var geometry = control.Geometry;
            var offsetScale = change?.GroupOffsetScale ?? 1f;
            controls.Add(new TouchControlInstance
            {
                InstanceId = control.Id,
                CatalogId = control.Id,
                AnchorX = change?.AnchorX ?? geometry.AnchorX,
                AnchorY = change?.AnchorY ?? geometry.AnchorY,
                OffsetXUnits = geometry.GroupOffsetXUnits * offsetScale,
                OffsetYUnits = geometry.GroupOffsetYUnits * offsetScale,
                Scale = change?.Scale ?? 1f,
                ZIndex = index,
                GroupId = control.EditGroupId,
                Latch = change?.Latch,
            });
        }

        return new TouchLayoutDocument
        {
            ProfileId = profile.Id,
            TemplateId = template.Id,
            BasedOnRevision = Math.Min(@override.BasedOnRevision, template.TemplateRevision),
            Controls = controls,
        };
    }
}
