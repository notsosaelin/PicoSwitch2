namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// The result of turning a user document into a runtime layout.
///
/// <see cref="Customized"/> answers "is this the shipped arrangement?" and nothing
/// more; <see cref="Warning"/> is the one thing a surface should tell the user when
/// a stored document could not be honoured exactly.
/// </summary>
public sealed record TouchCompositionResult(
    TouchLayout Layout,
    bool Customized,
    string? Warning = null,
    /// <summary>True when validation had to drop an instance to make the document usable.</summary>
    bool Degraded = false);

/// <summary>
/// Personality catalog + user document -> the layout the runtime plays.
///
/// <code>
/// TouchControllerProfile      what this controller can produce
///        + catalog entry      what such a control looks like
/// TouchLayoutDocument         which instances exist and where
///        |
///        v
/// TouchLayout                 a flat list of placed instances
///        |  TouchLayoutResolver
///        v
/// ResolvedTouchLayout         real coordinates, audited
/// </code>
///
/// The composer is the ONLY place a catalog entry and an instance transform are
/// combined, so the rules for how a user's scale, rotation and offset apply to
/// authored geometry exist exactly once.
/// </summary>
public static class TouchLayoutComposer
{
    public static TouchCompositionResult Compose(
        TouchControllerProfile profile, TouchLayoutDocument? document = null)
    {
        var template = profile.DefaultTemplate;
        var authored = TouchLayoutDocument.AuthoredDefault(profile);
        var mismatch = document is null ? null : Mismatch(document, profile, template);
        var accepted = document is not null && mismatch is null ? document : authored;
        var validation = TouchLayoutDocumentValidator.Validate(accepted, profile);
        var effective = validation.Document;

        var controls = effective.Controls
            .OrderBy(instance => instance.ZIndex)
            .Select(instance => Place(profile, instance))
            .OfType<TouchControlSpec>()
            .ToList();

        return new TouchCompositionResult(
            new TouchLayout(template.Id, template.SchemaVersion, controls)
            {
                ProfileId = profile.Id,
                TemplateId = template.Id,
                TemplateRevision = template.TemplateRevision,
            },
            Customized: !effective.Controls.SequenceEqual(authored.Controls),
            Warning: mismatch ?? validation.Problem,
            Degraded: validation.Degraded);
    }

    /// <summary>
    /// Why this document cannot be used with this profile, if it cannot.
    ///
    /// A revision NEWER than the shipped catalog is refused rather than
    /// best-efforted: it was written against controls this build may not have, and
    /// quietly dropping them would present a layout that is missing pieces without
    /// saying so.
    /// </summary>
    private static string? Mismatch(
        TouchLayoutDocument document,
        TouchControllerProfile profile,
        TouchLayoutTemplate template)
    {
        if (document.SchemaVersion != TouchLayoutDocument.CurrentSchemaVersion)
        {
            return "Stored layout schema is not supported";
        }

        if (document.ProfileId != profile.Id)
        {
            return "Stored layout belongs to another controller";
        }

        if (document.TemplateId != template.Id)
        {
            return "Stored layout belongs to another template";
        }

        return document.BasedOnRevision > template.TemplateRevision
            ? "Stored layout was written for a newer template revision"
            : null;
    }

    /// <summary>
    /// One instance placed against its catalog entry.
    ///
    /// Returns null only when the personality has no binding for the entry's output,
    /// which the profile's own outputs/bindings agreement makes unreachable; it is
    /// handled rather than asserted so a future catalog edit costs one control
    /// instead of the whole layout.
    /// </summary>
    private static TouchControlSpec? Place(
        TouchControllerProfile profile, TouchControlInstance instance)
    {
        var entry = profile.CatalogEntry(instance.CatalogId);
        if (entry is null || !profile.Bindings.TryGetValue(entry.Output, out var action))
        {
            return null;
        }

        var geometry = entry.Geometry;
        var scale = instance.Scale;

        return new TouchControlSpec
        {
            Id = instance.InstanceId,
            CatalogId = entry.Id,
            Kind = entry.Interaction,
            Action = action,
            AnchorX = instance.AnchorX,
            AnchorY = instance.AnchorY,
            WidthUnits = geometry.WidthUnits * scale,
            HeightUnits = geometry.HeightUnits * scale,
            Shape = geometry.Shape,
            HitMarginUnits = geometry.HitMarginUnits * scale,
            Priority = geometry.Priority,
            ZIndex = instance.ZIndex,
            Label = entry.Visual.Label,
            Glyph = entry.Visual.Glyph,
            Output = entry.Output,
            VisualRole = entry.Visual.Role,

            // Authored art direction plus the user's own turn. One total, so the
            // renderer and the hit tester cannot read different angles.
            VisualRotationDegrees = TouchLayoutLimits.NormalizeRotation(
                entry.Visual.RotationDegrees + instance.RotationDegrees),
            AuthoredRotationDegrees = entry.Visual.RotationDegrees,
            EditGroupId = instance.GroupId,

            // Already absolute: a group scale writes the scaled displacement into the
            // instance, so nothing multiplies it again here.
            GroupOffsetXUnits = instance.OffsetXUnits,
            GroupOffsetYUnits = instance.OffsetYUnits,
            Latch = instance.Latch,
        };
    }
}
