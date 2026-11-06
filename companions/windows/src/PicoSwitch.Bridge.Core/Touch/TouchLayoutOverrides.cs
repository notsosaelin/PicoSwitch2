using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// The RETIRED schema-1 layout document: a sparse override map over an immutable
/// template.
///
/// Kept for exactly one reason — reading what earlier builds wrote, so
/// <see cref="TouchLayoutMigration"/> can turn it into an instance document.
/// Nothing writes it as a user's live layout any more, and nothing should: the
/// model it encodes cannot express a duplicate control, a deleted one, a free
/// rotation or an arbitrary group.
///
/// The ENCODER survives for one reason: it is what pins the decoder. A round trip
/// through both is the only check that this reader still accepts exactly what
/// earlier builds wrote, and losing that would mean discovering on somebody's
/// device that an upgrade had quietly thrown their layout away.
/// </summary>
public sealed record TouchLayoutOverride
{
    public const int CurrentSchemaVersion = 1;

    public int SchemaVersion { get; init; } = CurrentSchemaVersion;

    public required TouchProfileId ProfileId { get; init; }

    public required string TemplateId { get; init; }

    public required int BasedOnRevision { get; init; }

    /// <summary>Unknown ids remain here and are ignored by migration.</summary>
    public IReadOnlyDictionary<string, TouchControlOverride> Controls { get; init; } =
        new Dictionary<string, TouchControlOverride>(StringComparer.Ordinal);
}

public sealed record TouchControlOverride
{
    public float? AnchorX { get; init; }

    public float? AnchorY { get; init; }

    public float? Scale { get; init; }

    public bool? Visible { get; init; }

    /// <summary>Scales a template's group-relative offset; written only by group scaling.</summary>
    public float? GroupOffsetScale { get; init; }

    /// <summary>
    /// Hold-to-latch for this control: <c>null</c> follows the global setting.
    ///
    /// CONFIGURATION, never the hold itself. Nothing here says a control is
    /// currently down; a document that could would be a document that presses a
    /// button on the console the moment a dead process comes back.
    /// </summary>
    public bool? Latch { get; init; }

    public bool IsEmpty =>
        AnchorX is null && AnchorY is null && Scale is null &&
        Visible is null && GroupOffsetScale is null && Latch is null;
}

public abstract record TouchOverrideDecodeResult
{
    private TouchOverrideDecodeResult()
    {
    }

    public sealed record Valid(TouchLayoutOverride Value) : TouchOverrideDecodeResult;

    public sealed record Invalid(string Problem) : TouchOverrideDecodeResult;
}

/// <summary>JSON reference codec for the platform-neutral persisted document schema.</summary>
public static class TouchLayoutOverrideJsonCodec
{
    public static string Encode(TouchLayoutOverride value)
    {
        var root = new JsonObject
        {
            ["schemaVersion"] = value.SchemaVersion,
            ["profileId"] = value.ProfileId.Key(),
            ["templateId"] = value.TemplateId,
            ["basedOnRevision"] = value.BasedOnRevision,
            ["controls"] = EncodeControls(value.Controls),
        };

        return root.ToJsonString();
    }

    /// <summary>Shared with the profile-library codec so one schema has exactly one writer.</summary>
    internal static JsonObject EncodeControls(
        IReadOnlyDictionary<string, TouchControlOverride> controls)
    {
        var result = new JsonObject();
        foreach (var (id, control) in controls.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            var entry = new JsonObject();
            if (control.AnchorX is { } anchorX)
            {
                entry["anchorX"] = anchorX;
            }

            if (control.AnchorY is { } anchorY)
            {
                entry["anchorY"] = anchorY;
            }

            if (control.Scale is { } scale)
            {
                entry["scale"] = scale;
            }

            if (control.Visible is { } visible)
            {
                entry["visible"] = visible;
            }

            if (control.GroupOffsetScale is { } groupOffsetScale)
            {
                entry["groupOffsetScale"] = groupOffsetScale;
            }

            if (control.Latch is { } latch)
            {
                entry["latch"] = latch;
            }

            result[id] = entry;
        }

        return result;
    }

    public static TouchOverrideDecodeResult Decode(string raw)
    {
        try
        {
            return DecodeDocument(raw);
        }
        catch (Exception)
        {
            return new TouchOverrideDecodeResult.Invalid(
                "Layout override contains malformed JSON values");
        }
    }

    private static TouchOverrideDecodeResult DecodeDocument(string raw)
    {
        JsonObject? root;
        try
        {
            root = JsonNode.Parse(raw) as JsonObject;
        }
        catch (JsonException)
        {
            return new TouchOverrideDecodeResult.Invalid("Layout override is not valid JSON");
        }

        if (root is null)
        {
            return new TouchOverrideDecodeResult.Invalid("Layout override is not valid JSON");
        }

        if (Int(root, "schemaVersion") is not { } schema)
        {
            return new TouchOverrideDecodeResult.Invalid("Layout override has no schema version");
        }

        if (schema != TouchLayoutOverride.CurrentSchemaVersion)
        {
            return new TouchOverrideDecodeResult.Invalid(
                schema > TouchLayoutOverride.CurrentSchemaVersion
                    ? "Layout override was written by a newer app"
                    : $"Layout override schema {schema} has no sequential migration");
        }

        if (TouchProfileIds.FromKey(Str(root, "profileId")) is not { } profile)
        {
            return new TouchOverrideDecodeResult.Invalid("Layout override has an unknown profile");
        }

        var templateId = Str(root, "templateId");
        if (string.IsNullOrWhiteSpace(templateId))
        {
            return new TouchOverrideDecodeResult.Invalid("Layout override has no template id");
        }

        if (Int(root, "basedOnRevision") is not { } revision || revision < 1)
        {
            return new TouchOverrideDecodeResult.Invalid(
                "Layout override has an invalid template revision");
        }

        if (root["controls"] is not JsonObject controlsObject)
        {
            return new TouchOverrideDecodeResult.Invalid("Layout override has no controls object");
        }

        var decoded = DecodeControls(controlsObject);
        return decoded switch
        {
            ControlsDecode.Bad bad => new TouchOverrideDecodeResult.Invalid(bad.Problem),
            ControlsDecode.Ok ok => new TouchOverrideDecodeResult.Valid(new TouchLayoutOverride
            {
                SchemaVersion = schema,
                ProfileId = profile,
                TemplateId = templateId,
                BasedOnRevision = revision,
                Controls = ok.Controls,
            }),
            _ => new TouchOverrideDecodeResult.Invalid("Layout override could not be read"),
        };
    }

    internal abstract record ControlsDecode
    {
        private ControlsDecode()
        {
        }

        public sealed record Ok(IReadOnlyDictionary<string, TouchControlOverride> Controls)
            : ControlsDecode;

        public sealed record Bad(string Problem) : ControlsDecode;
    }

    /// <summary>
    /// Shared with the profile-library codec.
    ///
    /// Every range check a stored control has to pass lives here exactly once, so a
    /// profile document cannot smuggle in geometry a bare override document would
    /// have been refused for.
    /// </summary>
    internal static ControlsDecode DecodeControls(JsonObject controlsObject)
    {
        var controls = new Dictionary<string, TouchControlOverride>(StringComparer.Ordinal);

        foreach (var (id, element) in controlsObject)
        {
            if (string.IsNullOrWhiteSpace(id))
            {
                return new ControlsDecode.Bad("Layout override has a blank control id");
            }

            if (element is not JsonObject entry)
            {
                return new ControlsDecode.Bad($"Override '{id}' is not an object");
            }

            if (!TryFloat(entry, "anchorX", out var anchorX))
            {
                return new ControlsDecode.Bad($"Override '{id}' has an invalid anchorX");
            }

            if (!TryFloat(entry, "anchorY", out var anchorY))
            {
                return new ControlsDecode.Bad($"Override '{id}' has an invalid anchorY");
            }

            if (!TryFloat(entry, "scale", out var scale))
            {
                return new ControlsDecode.Bad($"Override '{id}' has an invalid scale");
            }

            if (!TryBool(entry, "visible", out var visible))
            {
                return new ControlsDecode.Bad($"Override '{id}' has an invalid visible flag");
            }

            if (!TryFloat(entry, "groupOffsetScale", out var groupOffsetScale))
            {
                return new ControlsDecode.Bad($"Override '{id}' has an invalid groupOffsetScale");
            }

            if ((anchorX is { } ax && ax is < 0f or > 1f) ||
                (anchorY is { } ay && ay is < 0f or > 1f))
            {
                return new ControlsDecode.Bad($"Override '{id}' has an out-of-range anchor");
            }

            if (scale is { } s &&
                (s < TouchLayoutLimits.MinScale || s > TouchLayoutLimits.MaxScale))
            {
                return new ControlsDecode.Bad($"Override '{id}' has an out-of-range scale");
            }

            if (groupOffsetScale is { } g &&
                (g < TouchLayoutLimits.MinScale || g > TouchLayoutLimits.MaxScale))
            {
                return new ControlsDecode.Bad(
                    $"Override '{id}' has an out-of-range groupOffsetScale");
            }

            if (!TryBool(entry, "latch", out var latch))
            {
                return new ControlsDecode.Bad($"Override '{id}' has an invalid latch flag");
            }

            var control = new TouchControlOverride
            {
                AnchorX = anchorX,
                AnchorY = anchorY,
                Scale = scale,
                Visible = visible,
                GroupOffsetScale = groupOffsetScale,
                Latch = latch,
            };

            if (!control.IsEmpty)
            {
                controls[id] = control;
            }
        }

        return new ControlsDecode.Ok(controls);
    }

    internal static string? Str(JsonObject root, string key) =>
        root[key] is JsonValue value && value.TryGetValue<string>(out var text) ? text : null;

    internal static int? Int(JsonObject root, string key) =>
        root[key] is JsonValue value && value.TryGetValue<int>(out var number) ? number : null;

    internal static long? Long(JsonObject root, string key) =>
        root[key] is JsonValue value && value.TryGetValue<long>(out var number) ? number : null;

    /// <summary>
    /// Absent is fine; present-and-unreadable is not.
    ///
    /// The distinction matters: a missing key means "this control has no opinion",
    /// while a key holding a string or a NaN means the document is damaged, and
    /// silently treating the second as the first would import geometry nobody wrote.
    /// </summary>
    internal static bool TryFloat(JsonObject entry, string key, out float? value)
    {
        value = null;
        if (!entry.TryGetPropertyValue(key, out var node))
        {
            return true;
        }

        if (node is JsonValue jsonValue && jsonValue.TryGetValue<float>(out var number) &&
            float.IsFinite(number))
        {
            value = number;
            return true;
        }

        return false;
    }

    internal static bool TryBool(JsonObject entry, string key, out bool? value)
    {
        value = null;
        if (!entry.TryGetPropertyValue(key, out var node))
        {
            return true;
        }

        if (node is JsonValue jsonValue && jsonValue.TryGetValue<bool>(out var flag))
        {
            value = flag;
            return true;
        }

        return false;
    }

    internal static string Number(float value) =>
        value.ToString("R", CultureInfo.InvariantCulture);
}

/// <summary>
/// Read-only access to whatever a pre-2.0 build left behind.
///
/// No write side, deliberately: this schema is a migration SOURCE, and a store that
/// could still write it would eventually be written to. Invalid raw documents are
/// reported, never deleted — a later build may understand one, and the runtime is
/// safe in the meantime because the authored default needs nothing from storage.
/// </summary>
public interface ITouchLayoutOverrideStore
{
    TouchOverrideDecodeResult? Load(TouchProfileId profileId);
}
