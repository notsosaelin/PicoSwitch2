using System.Text.Json;
using System.Text.Json.Nodes;

namespace PicoSwitch.Bridge.Touch;

public abstract record TouchProfileLibraryDecodeResult
{
    private TouchProfileLibraryDecodeResult()
    {
    }

    public sealed record Valid(
        TouchProfileLibrary Value,
        /// <summary>True when a schema-1 document was migrated on the way in.</summary>
        bool Migrated = false) : TouchProfileLibraryDecodeResult;

    public sealed record Invalid(string Problem) : TouchProfileLibraryDecodeResult;
}

public abstract record TouchProfileDecodeResult
{
    private TouchProfileDecodeResult()
    {
    }

    public sealed record Valid(TouchLayoutProfile Value) : TouchProfileDecodeResult;

    public sealed record Invalid(string Problem) : TouchProfileDecodeResult;
}

/// <summary>
/// The persisted shape of a personality's profile set, and of a single exported profile.
///
/// <code>
/// library document                    exported profile document
/// {                                   {
///   schemaVersion: 2, personality,      schemaVersion: 2, kind, personality,
///   selectedProfileId,                  name, templateId, templateRevision,
///   profiles: [ profile, ... ]          controls: [ instance, ... ]
/// }                                   }
/// </code>
///
/// ## Two schema versions, one direction
///
/// Version 1 wrote each profile's <c>controls</c> as an OBJECT keyed by template control
/// id — the sparse override model. Version 2 writes an ARRAY of independently identified
/// instances. A version 1 document is still read, and is migrated through
/// <see cref="TouchLayoutMigration"/> as it is decoded; nothing writes version 1 again.
/// The distinction is visible in the JSON itself, which is what makes a half-migrated
/// document impossible rather than merely unlikely.
///
/// The factory profile never appears in either document. It is synthesized from the
/// shipped template, which is what makes it impossible for stored data to overwrite,
/// rename or delete it.
/// </summary>
public static class TouchProfileLibraryJsonCodec
{
    public const int CurrentSchemaVersion = 2;

    /// <summary>The retired sparse-override document, still readable for migration.</summary>
    public const int LegacySchemaVersion = 1;

    /// <summary>Marks an exported single profile so an unrelated JSON file is refused early.</summary>
    public const string ExportKind = "picoswitch.touch.profile";

    /// <summary>Placeholder identity for an import; the receiving library allocates the real one.</summary>
    private const string ImportedId = "imported";

    public static string Encode(TouchProfileLibrary library)
    {
        var profiles = new JsonArray();
        foreach (var profile in library.UserProfiles)
        {
            profiles.Add(EncodeProfileBody(profile));
        }

        return new JsonObject
        {
            ["schemaVersion"] = CurrentSchemaVersion,
            ["personality"] = library.Personality.Key(),
            ["selectedProfileId"] = library.SelectedProfileId,
            ["profiles"] = profiles,
        }.ToJsonString();
    }

    public static TouchProfileLibraryDecodeResult Decode(string raw, TouchProfileId personality)
    {
        try
        {
            return DecodeLibrary(raw, personality);
        }
        catch (Exception)
        {
            return new TouchProfileLibraryDecodeResult.Invalid(
                "Layout profiles contain malformed JSON values");
        }
    }

    /// <summary>A single profile as a standalone, shareable document.</summary>
    public static string EncodeExport(TouchLayoutProfile profile)
    {
        var root = EncodeProfileBody(profile);
        root["schemaVersion"] = CurrentSchemaVersion;
        root["kind"] = ExportKind;
        root["personality"] = profile.Personality.Key();
        return root.ToJsonString();
    }

    public static TouchProfileDecodeResult DecodeExport(string raw)
    {
        try
        {
            return DecodeExportDocument(raw);
        }
        catch (Exception)
        {
            return new TouchProfileDecodeResult.Invalid(
                "That layout file contains malformed JSON values");
        }
    }

    private static TouchProfileLibraryDecodeResult DecodeLibrary(
        string raw, TouchProfileId personality)
    {
        JsonObject? root;
        try
        {
            root = JsonNode.Parse(raw) as JsonObject;
        }
        catch (JsonException)
        {
            root = null;
        }

        if (root is null)
        {
            return new TouchProfileLibraryDecodeResult.Invalid("Layout profiles are not valid JSON");
        }

        if (TouchLayoutOverrideJsonCodec.Int(root, "schemaVersion") is not { } schema)
        {
            return new TouchProfileLibraryDecodeResult.Invalid(
                "Layout profiles have no schema version");
        }

        if (schema > CurrentSchemaVersion)
        {
            return new TouchProfileLibraryDecodeResult.Invalid(
                "Layout profiles were written by a newer app");
        }

        if (schema < LegacySchemaVersion)
        {
            return new TouchProfileLibraryDecodeResult.Invalid(
                $"Layout profile schema {schema} has no sequential migration");
        }

        if (TouchProfileIds.FromKey(TouchLayoutOverrideJsonCodec.Str(root, "personality"))
            is not { } stored)
        {
            return new TouchProfileLibraryDecodeResult.Invalid(
                "Layout profiles name an unknown controller");
        }

        if (stored != personality)
        {
            return new TouchProfileLibraryDecodeResult.Invalid(
                "Layout profiles belong to another controller");
        }

        if (root["profiles"] is not JsonArray array)
        {
            return new TouchProfileLibraryDecodeResult.Invalid(
                "Layout profiles have no profile list");
        }

        if (array.Count > TouchProfileLibrary.MaxUserProfiles)
        {
            return new TouchProfileLibraryDecodeResult.Invalid(
                "Layout profiles exceed the supported count");
        }

        var profiles = new List<TouchLayoutProfile>();
        var ids = new HashSet<string>(StringComparer.Ordinal);

        foreach (var element in array)
        {
            if (element is not JsonObject entry)
            {
                return new TouchProfileLibraryDecodeResult.Invalid(
                    "A layout profile is not an object");
            }

            switch (DecodeProfileBody(entry, personality, schema))
            {
                case TouchProfileDecodeResult.Invalid invalid:
                    return new TouchProfileLibraryDecodeResult.Invalid(invalid.Problem);

                case TouchProfileDecodeResult.Valid valid:
                    if (!ids.Add(valid.Value.Id))
                    {
                        return new TouchProfileLibraryDecodeResult.Invalid(
                            "Layout profiles contain a duplicate profile id");
                    }

                    profiles.Add(valid.Value);
                    break;
            }
        }

        // A selection naming a profile that is not here resolves to the factory default
        // rather than failing the whole document: the layouts themselves are still
        // perfectly usable.
        var requested = TouchLayoutOverrideJsonCodec.Str(root, "selectedProfileId");
        var selected = requested is not null &&
            (requested == TouchProfileLibrary.FactoryProfileId ||
             profiles.Any(profile => profile.Id == requested))
            ? requested
            : TouchProfileLibrary.FactoryProfileId;

        return new TouchProfileLibraryDecodeResult.Valid(
            new TouchProfileLibrary(personality)
            {
                UserProfiles = profiles,
                SelectedProfileId = selected,
            },
            Migrated: schema != CurrentSchemaVersion);
    }

    private static TouchProfileDecodeResult DecodeExportDocument(string raw)
    {
        JsonObject? root;
        try
        {
            root = JsonNode.Parse(raw) as JsonObject;
        }
        catch (JsonException)
        {
            root = null;
        }

        if (root is null)
        {
            return new TouchProfileDecodeResult.Invalid("That layout file is not valid JSON");
        }

        if (TouchLayoutOverrideJsonCodec.Int(root, "schemaVersion") is not { } schema)
        {
            return new TouchProfileDecodeResult.Invalid("That layout file has no schema version");
        }

        if (schema > CurrentSchemaVersion)
        {
            return new TouchProfileDecodeResult.Invalid(
                "That layout file was written by a newer app");
        }

        if (schema < LegacySchemaVersion)
        {
            return new TouchProfileDecodeResult.Invalid(
                $"Layout file schema {schema} has no sequential migration");
        }

        if (TouchLayoutOverrideJsonCodec.Str(root, "kind") != ExportKind)
        {
            return new TouchProfileDecodeResult.Invalid(
                "That file is not an exported touch layout");
        }

        if (TouchProfileIds.FromKey(TouchLayoutOverrideJsonCodec.Str(root, "personality"))
            is not { } personality)
        {
            return new TouchProfileDecodeResult.Invalid(
                "That layout file names an unknown controller");
        }

        return DecodeProfileBody(root, personality, schema);
    }

    private static JsonObject EncodeProfileBody(TouchLayoutProfile profile)
    {
        var root = new JsonObject
        {
            ["id"] = profile.Id,
            ["name"] = profile.Name,
            ["templateId"] = profile.TemplateId,
            ["templateRevision"] = profile.TemplateRevision,
            ["createdAtEpochMs"] = profile.Metadata.CreatedAtEpochMs,
            ["updatedAtEpochMs"] = profile.Metadata.UpdatedAtEpochMs,
        };

        if (profile.Metadata.GameKey is { } gameKey)
        {
            root["gameKey"] = gameKey;
        }

        root["controls"] = EncodeInstances(profile.Document.Controls);
        return root;
    }

    /// <summary>
    /// Instances, in document order, with defaulted fields omitted.
    ///
    /// Omitting defaults keeps a stored layout readable and small: a control the user
    /// only moved writes an anchor and nothing else. The decoder supplies exactly the
    /// same defaults, so a round trip is lossless.
    /// </summary>
    internal static JsonArray EncodeInstances(IReadOnlyList<TouchControlInstance> controls)
    {
        var array = new JsonArray();

        foreach (var instance in controls)
        {
            var entry = new JsonObject
            {
                ["instanceId"] = instance.InstanceId,
                ["catalogId"] = instance.CatalogId,
                ["anchorX"] = instance.AnchorX,
                ["anchorY"] = instance.AnchorY,
            };

            if (instance.OffsetXUnits != 0f)
            {
                entry["offsetXUnits"] = instance.OffsetXUnits;
            }

            if (instance.OffsetYUnits != 0f)
            {
                entry["offsetYUnits"] = instance.OffsetYUnits;
            }

            if (instance.Scale != 1f)
            {
                entry["scale"] = instance.Scale;
            }

            if (instance.RotationDegrees != 0f)
            {
                entry["rotationDegrees"] = instance.RotationDegrees;
            }

            entry["zIndex"] = instance.ZIndex;

            if (instance.GroupId is { } group)
            {
                entry["groupId"] = group;
            }

            if (instance.Latch is { } latch)
            {
                entry["latch"] = latch;
            }

            array.Add(entry);
        }

        return array;
    }

    internal abstract record InstancesDecode
    {
        private InstancesDecode()
        {
        }

        public sealed record Ok(IReadOnlyList<TouchControlInstance> Controls) : InstancesDecode;

        public sealed record Bad(string Problem) : InstancesDecode;
    }

    /// <summary>
    /// One instance array.
    ///
    /// Range checks live here, once, so a hand-edited or imported document has to satisfy
    /// exactly what an editor operation would have enforced. Structural repair —
    /// duplicate identities, catalog entries that no longer exist — is deliberately NOT
    /// done here: it needs the personality catalog, and
    /// <see cref="TouchLayoutDocumentValidator"/> owns it.
    /// </summary>
    internal static InstancesDecode DecodeInstances(JsonArray array)
    {
        var controls = new List<TouchControlInstance>();

        foreach (var element in array)
        {
            if (element is not JsonObject value)
            {
                return new InstancesDecode.Bad("A layout control is not an object");
            }

            var instanceId = Text(value, "instanceId");
            if (instanceId is null)
            {
                return new InstancesDecode.Bad("A layout control has no identity");
            }

            var catalogId = Text(value, "catalogId");
            if (catalogId is null)
            {
                return new InstancesDecode.Bad($"Control '{instanceId}' names no control type");
            }

            if (Finite(value, "anchorX") is not { } anchorX)
            {
                return new InstancesDecode.Bad($"Control '{instanceId}' has an invalid anchorX");
            }

            if (Finite(value, "anchorY") is not { } anchorY)
            {
                return new InstancesDecode.Bad($"Control '{instanceId}' has an invalid anchorY");
            }

            if (anchorX is < TouchLayoutLimits.MinAnchor or > TouchLayoutLimits.MaxAnchor ||
                anchorY is < TouchLayoutLimits.MinAnchor or > TouchLayoutLimits.MaxAnchor)
            {
                return new InstancesDecode.Bad($"Control '{instanceId}' has an out-of-range anchor");
            }

            if (OptionalFinite(value, "offsetXUnits") is not { } offsetX)
            {
                return new InstancesDecode.Bad(
                    $"Control '{instanceId}' has an invalid offsetXUnits");
            }

            if (OptionalFinite(value, "offsetYUnits") is not { } offsetY)
            {
                return new InstancesDecode.Bad(
                    $"Control '{instanceId}' has an invalid offsetYUnits");
            }

            if (MathF.Abs(offsetX) > TouchLayoutLimits.MaxOffsetUnits ||
                MathF.Abs(offsetY) > TouchLayoutLimits.MaxOffsetUnits)
            {
                return new InstancesDecode.Bad($"Control '{instanceId}' has an out-of-range offset");
            }

            if (OptionalFinite(value, "scale", 1f) is not { } scale)
            {
                return new InstancesDecode.Bad($"Control '{instanceId}' has an invalid scale");
            }

            if (scale < TouchLayoutLimits.MinScale || scale > TouchLayoutLimits.MaxScale)
            {
                return new InstancesDecode.Bad($"Control '{instanceId}' has an out-of-range scale");
            }

            if (OptionalFinite(value, "rotationDegrees") is not { } rotation)
            {
                return new InstancesDecode.Bad($"Control '{instanceId}' has an invalid rotation");
            }

            var zIndex = TouchLayoutOverrideJsonCodec.Int(value, "zIndex") ?? controls.Count;

            if (!TouchLayoutOverrideJsonCodec.TryBool(value, "latch", out var latch))
            {
                return new InstancesDecode.Bad($"Control '{instanceId}' has an invalid latch flag");
            }

            controls.Add(new TouchControlInstance
            {
                InstanceId = instanceId,
                CatalogId = catalogId,
                AnchorX = anchorX,
                AnchorY = anchorY,
                OffsetXUnits = offsetX,
                OffsetYUnits = offsetY,
                Scale = scale,
                RotationDegrees = TouchLayoutLimits.NormalizeRotation(rotation),
                ZIndex = zIndex,
                GroupId = Text(value, "groupId"),
                Latch = latch,
            });
        }

        return new InstancesDecode.Ok(controls);
    }

    /// <summary>
    /// One profile body, from either document and either schema version.
    ///
    /// An exported document has no <c>id</c> of its own worth trusting — importing is
    /// always an insert into some other library — so a missing id is accepted and the
    /// caller re-allocates one. A LIBRARY document without ids would be a library whose
    /// selection cannot mean anything, so ids are required there.
    /// </summary>
    private static TouchProfileDecodeResult DecodeProfileBody(
        JsonObject root, TouchProfileId personality, int schema)
    {
        var id = TouchLayoutOverrideJsonCodec.Str(root, "id") is { Length: > 0 } stored &&
                 !string.IsNullOrWhiteSpace(stored)
            ? stored
            : ImportedId;

        if (id == TouchProfileLibrary.FactoryProfileId)
        {
            return new TouchProfileDecodeResult.Invalid(
                "A stored layout profile claims the reserved default id");
        }

        var name = TouchLayoutOverrideJsonCodec.Str(root, "name");
        if (string.IsNullOrWhiteSpace(name))
        {
            return new TouchProfileDecodeResult.Invalid("A layout profile has no name");
        }

        var templateId = TouchLayoutOverrideJsonCodec.Str(root, "templateId");
        if (string.IsNullOrWhiteSpace(templateId))
        {
            return new TouchProfileDecodeResult.Invalid("A layout profile has no template id");
        }

        if (TouchLayoutOverrideJsonCodec.Int(root, "templateRevision") is not { } revision ||
            revision < 1)
        {
            return new TouchProfileDecodeResult.Invalid(
                "A layout profile has an invalid template revision");
        }

        TouchLayoutDocument document;
        if (schema == LegacySchemaVersion)
        {
            var migrated = DecodeLegacyDocument(root, personality, templateId, revision);
            if (migrated is TouchProfileDecodeResult.Invalid)
            {
                return migrated;
            }

            document = ((TouchProfileDecodeResult.Valid)migrated).Value.Document;
        }
        else
        {
            if (root["controls"] is not JsonArray array)
            {
                return new TouchProfileDecodeResult.Invalid(
                    "A layout profile has no controls list");
            }

            var decoded = DecodeInstances(array);
            if (decoded is InstancesDecode.Bad bad)
            {
                return new TouchProfileDecodeResult.Invalid(bad.Problem);
            }

            document = new TouchLayoutDocument
            {
                ProfileId = personality,
                TemplateId = templateId,
                BasedOnRevision = revision,
                Controls = ((InstancesDecode.Ok)decoded).Controls,
            };
        }

        return new TouchProfileDecodeResult.Valid(
            new TouchLayoutProfile(
                id,

                // Names are sanitized on the way IN as well as on the way out: a
                // hand-edited document must not be able to put control characters or an
                // unbounded string into the profile picker.
                TouchProfileLibraryEditor.SanitizeName(name),
                document)
            {
                Metadata = new TouchProfileMetadata(
                    Math.Max(TouchLayoutOverrideJsonCodec.Long(root, "createdAtEpochMs") ?? 0L, 0L),
                    Math.Max(TouchLayoutOverrideJsonCodec.Long(root, "updatedAtEpochMs") ?? 0L, 0L),
                    Text(root, "gameKey")),
            });
    }

    /// <summary>
    /// A schema-1 profile body, migrated on the way in.
    ///
    /// Reuses the retired override decoder — including its range checks — so a legacy
    /// document has to have been valid legacy data before it becomes a valid instance
    /// document. Migration is applied against the CURRENT catalog, which is what lets a
    /// control that has since been retired disappear from the migrated layout instead of
    /// becoming a dangling instance.
    /// </summary>
    private static TouchProfileDecodeResult DecodeLegacyDocument(
        JsonObject root, TouchProfileId personality, string templateId, int revision)
    {
        if (root["controls"] is not JsonObject controlsObject)
        {
            return new TouchProfileDecodeResult.Invalid("A layout profile has no controls object");
        }

        var decoded = TouchLayoutOverrideJsonCodec.DecodeControls(controlsObject);
        if (decoded is TouchLayoutOverrideJsonCodec.ControlsDecode.Bad bad)
        {
            return new TouchProfileDecodeResult.Invalid(bad.Problem);
        }

        var controls = ((TouchLayoutOverrideJsonCodec.ControlsDecode.Ok)decoded).Controls;
        var profile = TouchProfileCatalog.Require(personality);
        var document = TouchLayoutMigration.FromOverride(profile, new TouchLayoutOverride
        {
            SchemaVersion = TouchLayoutOverride.CurrentSchemaVersion,
            ProfileId = personality,
            TemplateId = templateId,
            BasedOnRevision = revision,
            Controls = controls,
        });

        return new TouchProfileDecodeResult.Valid(
            new TouchLayoutProfile(ImportedId, ImportedId, document));
    }

    private static string? Text(JsonObject root, string key) =>
        TouchLayoutOverrideJsonCodec.Str(root, key) is { } value &&
        !string.IsNullOrWhiteSpace(value)
            ? value
            : null;

    private static float? Finite(JsonObject root, string key) =>
        TouchLayoutOverrideJsonCodec.TryFloat(root, key, out var value) ? value : null;

    /// <summary>Absent means the default; present-but-unreadable is an error, so null is the failure.</summary>
    private static float? OptionalFinite(JsonObject root, string key, float fallback = 0f)
    {
        if (!root.ContainsKey(key))
        {
            return fallback;
        }

        return TouchLayoutOverrideJsonCodec.TryFloat(root, key, out var value) ? value : null;
    }
}
