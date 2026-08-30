using System.Text;
using System.Text.Json;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The local profile library on disk.
/// </summary>
/// <remarks>
/// Its own document rather than a corner of the adapter registry: it has a
/// different lifetime (it belongs to the user, not to any adapter) and a
/// different write rate (every Save), and folding it in would rewrite the
/// registry on every profile edit.
///
/// Decoded TOTALLY, like every other document here: an unreadable file must cost
/// the user their profiles, never their ability to launch the app.
/// </remarks>
public sealed class KbmProfileLibraryStore(WindowsDocumentStore documents)
{
    public const string DocumentName = "kbm-profiles.json";

    public KbmProfileLibrary Load() =>
        KbmProfileLibraryCodec.Decode(documents.Read(DocumentName));

    /// <summary>
    /// Writes atomically, via the shared document store. A half-written library
    /// is not "some profiles missing" — it is an unparseable file and an empty
    /// list on next launch.
    /// </summary>
    public bool Save(KbmProfileLibrary library) =>
        documents.Write(DocumentName, KbmProfileLibraryCodec.Encode(library));
}

public static class KbmProfileLibraryCodec
{
    public const int Schema = KbmProfileLibrary.CurrentVersion;

    public static string Encode(KbmProfileLibrary library)
    {
        var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schema", Schema);
            writer.WriteStartArray("profiles");
            foreach (var profile in library.Profiles)
            {
                writer.WriteStartObject();
                writer.WriteString("id", profile.Id);
                writer.WriteString("layout", profile.Layout.Wire());
                writer.WriteString("name", profile.Name);
                writer.WriteNumber("fingerprint", profile.Fingerprint);
                writer.WriteNumber("modified", profile.Modified.ToUnixTimeMilliseconds());

                writer.WriteStartArray("bindings");
                foreach (var binding in profile.Bindings)
                {
                    writer.WriteStartObject();
                    writer.WriteString("src", binding.Source.Wire);
                    writer.WriteString("dst", binding.Destination.Wire());
                    writer.WriteEndObject();
                }

                writer.WriteEndArray();

                // Only the PROFILE-OWNED mouse values. The adapter-reported
                // ranges are a property of the firmware, not of the profile, and
                // persisting them would let a stale range outlive the adapter
                // that reported it.
                writer.WriteStartObject("mouse");
                writer.WriteNumber("sensitivityX", profile.Mouse.SensitivityX);
                writer.WriteNumber("sensitivityY", profile.Mouse.SensitivityY);
                writer.WriteNumber("velocityWindowMs", profile.Mouse.VelocityWindowMs);
                writer.WriteNumber("antiDeadzone", profile.Mouse.AntiDeadzone);
                writer.WriteBoolean("invertX", profile.Mouse.InvertX);
                writer.WriteBoolean("invertY", profile.Mouse.InvertY);
                writer.WriteEndObject();

                writer.WriteEndObject();
            }

            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        return Encoding.UTF8.GetString(buffer.ToArray());
    }

    /// <summary>
    /// Never throws, and never returns a partially-trusted profile.
    /// </summary>
    /// <remarks>
    /// A row this build cannot read is SKIPPED rather than repaired into
    /// something plausible: a profile with an unreadable layout or an invented
    /// binding would be offered to the user as theirs and then behave in a way
    /// they never configured. Losing one row loudly is better than keeping a
    /// wrong one silently.
    /// </remarks>
    public static KbmProfileLibrary Decode(string? text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return KbmProfileLibrary.Empty;
        }

        try
        {
            using var document = JsonDocument.Parse(text);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                return KbmProfileLibrary.Empty;
            }

            // A future schema is not readable by this build. Returning empty
            // loses the library; returning a misparse would silently rewrite it
            // on the next save, which is worse.
            if (root.TryGetProperty("schema", out var schema) &&
                schema.TryGetInt32(out var version) && version > Schema)
            {
                return KbmProfileLibrary.Empty;
            }

            if (!root.TryGetProperty("profiles", out var rows) ||
                rows.ValueKind != JsonValueKind.Array)
            {
                return KbmProfileLibrary.Empty;
            }

            var profiles = new List<KbmLocalProfile>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var row in rows.EnumerateArray())
            {
                var profile = DecodeProfile(row);
                // Duplicate ids would make Find() and Without() ambiguous.
                if (profile is not null && seen.Add(profile.Id))
                {
                    profiles.Add(profile);
                }
            }

            return new KbmProfileLibrary
            {
                Profiles = new ValueList<KbmLocalProfile>(profiles),
            };
        }
        catch (Exception)
        {
            return KbmProfileLibrary.Empty;
        }
    }

    private static KbmLocalProfile? DecodeProfile(JsonElement row)
    {
        if (row.ValueKind != JsonValueKind.Object)
        {
            return null;
        }

        var id = Text(row, "id");
        var name = Text(row, "name");
        var layout = KbmLayouts.FromWire(Text(row, "layout"));
        if (string.IsNullOrWhiteSpace(id) || string.IsNullOrWhiteSpace(name) ||
            layout is null)
        {
            return null;
        }

        var bindings = new List<KbmBinding>();
        if (row.TryGetProperty("bindings", out var list) &&
            list.ValueKind == JsonValueKind.Array)
        {
            foreach (var entry in list.EnumerateArray())
            {
                if (entry.ValueKind != JsonValueKind.Object)
                {
                    return null;
                }

                var text = Text(entry, "src");
                var source = text is null ? null : KbmSource.Parse(text);
                var destination = KbmDestinations.FromWire(Text(entry, "dst"));
                if (source is null || destination is null)
                {
                    // An unreadable binding invalidates the whole profile: a
                    // mapping missing one key is not the mapping the user saved.
                    return null;
                }

                bindings.Add(new KbmBinding(source, destination.Value, Custom: true));
            }
        }

        return new KbmLocalProfile
        {
            Id = id!,
            Layout = layout.Value,
            Name = name!,
            Bindings = new ValueList<KbmBinding>(bindings),
            Mouse = DecodeMouse(row),
            Fingerprint = Number(row, "fingerprint"),
            Modified = DateTimeOffset.FromUnixTimeMilliseconds(
                Number(row, "modified")),
        };
    }

    private static KbmMouseConfig DecodeMouse(JsonElement row)
    {
        if (!row.TryGetProperty("mouse", out var mouse) ||
            mouse.ValueKind != JsonValueKind.Object)
        {
            return new KbmMouseConfig();
        }

        return new KbmMouseConfig(
            SensitivityX: (int)Number(mouse, "sensitivityX"),
            SensitivityY: (int)Number(mouse, "sensitivityY"),
            VelocityWindowMs: (int)Number(mouse, "velocityWindowMs"),
            InvertX: Flag(mouse, "invertX"),
            InvertY: Flag(mouse, "invertY"),
            AntiDeadzone: (int)Number(mouse, "antiDeadzone"));
    }

    private static string? Text(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;

    private static long Number(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out var result)
            ? result
            : 0L;

    private static bool Flag(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.True;
}
