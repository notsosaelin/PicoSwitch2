using System.IO.Compression;
using System.Text;
using System.Text.Json;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The amiibo library exchange archive: a v3 <c>library.json</c> plus one
/// <c>.bin</c> per entry.
/// </summary>
/// <remarks>
/// A CROSS-PLATFORM FORMAT, not this app's private save file. The same shape is
/// written and read by the Android companion and by the web portal, so an
/// archive exported from any of the three opens in the other two. Everything
/// here that looks arbitrary — the version number, the field names, entry order,
/// the stored (uncompressed) entries, the <c>loadedKey</c> rule — is part of
/// that contract and cannot be "tidied" one-sidedly.
///
/// The manifest is METADATA ONLY. Names and the loaded marker come from it; the
/// images are authoritative and are validated independently, so a manifest that
/// disagrees with the bytes loses. An archive with no readable manifest at all
/// still imports, with names taken from the filenames — losing a display name is
/// a far better outcome than refusing someone's backup.
///
/// ## Everything is bounded
///
/// A ZIP is attacker-shaped input: nested paths, absurd entry counts, entries
/// that decompress to gigabytes. Every dimension has an explicit ceiling and
/// every name is checked for traversal before it is used, because this runs on a
/// file the user was handed by someone else.
/// </remarks>
public static class AmiiboArchive
{
    public const string Format = "PicoSwitch2 Amiibo Library";
    public const int Version = 3;

    public const int MaxArchiveBytes = 8 * 1024 * 1024;
    public const int MaxEntries = 128;
    public const int MaxManifestBytes = 128 * 1024;
    public const int MaxImageBytes = 2048;
    public const int MaxTotalImageBytes = 256 * 1024;
    public const int MaxNameChars = 120;

    public sealed record ExportItem(AmiiboLibraryItem Item, byte[] Bytes, bool Loaded);

    public sealed record ImportedEntry(
        string FileName,
        string DisplayName,
        byte[] Bytes,
        bool Loaded);

    /// <summary>Write a library to a portal-compatible archive.</summary>
    public static byte[] Write(IReadOnlyList<ExportItem> items)
    {
        if (items.Count > MaxEntries)
        {
            throw new InvalidOperationException("Amiibo library has too many entries");
        }

        var usedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "library.json" };
        var images = new List<(string Name, byte[] Bytes)>();
        var manifestEntries = new List<Dictionary<string, object>>();

        foreach (var exported in items)
        {
            AmiiboFiles.Validate(exported.Bytes);
            if (exported.Bytes.Length > MaxImageBytes)
            {
                throw new InvalidOperationException("Amiibo image exceeds the archive limit");
            }

            var fileName = UniqueName(
                SafeBaseName(exported.Item.DisplayName, exported.Item.FigureId), usedNames);
            images.Add((fileName, (byte[])exported.Bytes.Clone()));
            manifestEntries.Add(new Dictionary<string, object>
            {
                ["file"] = fileName,
                ["id"] = exported.Item.FigureId,
                ["uid"] = exported.Item.Uid,
                ["name"] = Truncate(exported.Item.DisplayName, MaxNameChars),
                ["loaded"] = exported.Loaded,
            });
        }

        var loaded = items.FirstOrDefault(item => item.Loaded);
        var manifest = JsonSerializer.SerializeToUtf8Bytes(new Dictionary<string, object>
        {
            ["format"] = Format,
            ["version"] = Version,
            ["exportedAt"] = DateTimeOffset.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffffffZ"),
            ["loadedKey"] = loaded is null ? "" : PortalKey(loaded.Bytes),
            ["entries"] = manifestEntries,
        });

        if (manifest.Length > MaxManifestBytes)
        {
            throw new InvalidOperationException("Amiibo library manifest is too large");
        }

        using var output = new MemoryStream();
        using (var zip = new ZipArchive(output, ZipArchiveMode.Create, leaveOpen: true))
        {
            // library.json FIRST, as the portal writes it. A simple browser
            // reader takes the first entry, so the order is part of the format
            // rather than a stylistic choice.
            WriteStored(zip, "library.json", manifest);
            foreach (var (name, bytes) in images)
            {
                WriteStored(zip, name, bytes);
            }
        }

        var result = output.ToArray();
        if (result.Length > MaxArchiveBytes)
        {
            throw new InvalidOperationException("Amiibo library archive is too large");
        }

        return result;
    }

    /// <summary>
    /// Parse and validate every entry, touching no local state.
    /// </summary>
    /// <remarks>
    /// Deliberately a pure read: the caller decides what to keep. An import that
    /// half-applied and then threw would leave a library the user cannot reason
    /// about, so validation finishes completely before anything is written.
    /// </remarks>
    public static IReadOnlyList<ImportedEntry> Read(byte[] raw)
    {
        if (raw.Length > MaxArchiveBytes)
        {
            throw new InvalidOperationException("Amiibo library archive is too large");
        }

        if (raw.Length < 4 || raw[0] != (byte)'P' || raw[1] != (byte)'K')
        {
            throw new InvalidOperationException("Amiibo library backup is not a ZIP archive");
        }

        var images = new List<(string Name, byte[] Bytes)>();
        byte[]? manifestBytes = null;
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var totalImageBytes = 0;
        var entryCount = 0;

        using (var input = new MemoryStream(raw, writable: false))
        using (var zip = new ZipArchive(input, ZipArchiveMode.Read))
        {
            foreach (var entry in zip.Entries)
            {
                entryCount++;
                if (entryCount > MaxEntries + 1)
                {
                    throw new InvalidOperationException(
                        "Amiibo library archive has too many entries");
                }

                var name = SafeEntryName(entry.FullName);
                if (!names.Add(name))
                {
                    throw new InvalidOperationException(
                        "Amiibo library archive contains duplicate entries");
                }

                // A directory entry ends in a separator, which SafeEntryName has
                // already refused, so anything reaching here is a file.
                if (string.Equals(name, "library.json", StringComparison.OrdinalIgnoreCase))
                {
                    if (manifestBytes is not null)
                    {
                        throw new InvalidOperationException(
                            "Amiibo library archive has duplicate manifests");
                    }

                    manifestBytes = ReadBounded(entry, MaxManifestBytes, "library manifest");
                }
                else if (name.EndsWith(".bin", StringComparison.OrdinalIgnoreCase))
                {
                    if (images.Count >= MaxEntries)
                    {
                        throw new InvalidOperationException(
                            "Amiibo library archive has too many images");
                    }

                    var bytes = ReadBounded(entry, MaxImageBytes, "Amiibo image");
                    totalImageBytes += bytes.Length;
                    if (totalImageBytes > MaxTotalImageBytes)
                    {
                        throw new InvalidOperationException(
                            "Amiibo library archive contains too much image data");
                    }

                    images.Add((name, AmiiboFiles.NormalizeImport(bytes)));
                }
                else
                {
                    throw new InvalidOperationException(
                        $"Unsupported Amiibo library entry: {name}");
                }
            }
        }

        if (images.Count == 0)
        {
            throw new InvalidOperationException(
                "Amiibo library archive contains no valid .bin files");
        }

        // A manifest that cannot be read costs the display names and nothing
        // else. Refusing the whole archive over its metadata would throw away
        // images that are perfectly valid.
        Manifest? manifest = null;
        if (manifestBytes is not null)
        {
            try
            {
                manifest = ParseManifest(manifestBytes);
            }
            catch (Exception)
            {
                manifest = null;
            }
        }

        var byFile = manifest?.Entries.ToDictionary(
            entry => entry.FileName, StringComparer.OrdinalIgnoreCase);

        var loadedFile = manifest?.Entries.FirstOrDefault(entry => entry.Loaded)?.FileName;
        if (loadedFile is null && !string.IsNullOrEmpty(manifest?.LoadedKey))
        {
            // The portal identifies the loaded tag by content rather than by
            // filename, so an archive it wrote is still understood here.
            loadedFile = images
                .FirstOrDefault(image => PortalKey(image.Bytes) == manifest!.LoadedKey)
                .Name;
        }

        return images.Select(image =>
        {
            ManifestEntry? metadata = null;
            byFile?.TryGetValue(image.Name, out metadata);

            var fallback = Path.GetFileNameWithoutExtension(image.Name);
            var displayName = Truncate(
                (metadata?.Name ?? fallback).Trim(), MaxNameChars);

            return new ImportedEntry(
                FileName: image.Name,
                DisplayName: displayName.Length == 0 ? "Imported Amiibo" : displayName,
                Bytes: image.Bytes,
                Loaded: string.Equals(image.Name, loadedFile, StringComparison.OrdinalIgnoreCase));
        }).ToList();
    }

    // ---------------------------------------------------------------- manifest

    private sealed record ManifestEntry(string FileName, string Name, bool Loaded);

    private sealed record Manifest(IReadOnlyList<ManifestEntry> Entries, string? LoadedKey);

    private static Manifest ParseManifest(byte[] bytes)
    {
        using var document = JsonDocument.Parse(bytes);
        var root = document.RootElement;

        if (root.GetProperty("format").GetString() != Format)
        {
            throw new InvalidOperationException("Unsupported Amiibo library format");
        }

        if (root.GetProperty("version").GetInt32() != Version)
        {
            throw new InvalidOperationException("Unsupported Amiibo library version");
        }

        var entries = new List<ManifestEntry>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        if (root.TryGetProperty("entries", out var array) &&
            array.ValueKind == JsonValueKind.Array)
        {
            if (array.GetArrayLength() > MaxEntries)
            {
                throw new InvalidOperationException(
                    "Amiibo library manifest has too many entries");
            }

            foreach (var value in array.EnumerateArray())
            {
                var file = value.GetProperty("file").GetString()
                    ?? throw new InvalidOperationException("Manifest entry has no file");

                if (string.Equals(file, "library.json", StringComparison.OrdinalIgnoreCase) ||
                    !file.EndsWith(".bin", StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException(
                        "Manifest entry has an invalid image file");
                }

                SafeEntryName(file);
                if (!seen.Add(file))
                {
                    throw new InvalidOperationException(
                        "Manifest contains duplicate image files");
                }

                entries.Add(new ManifestEntry(
                    FileName: file,
                    Name: Truncate(
                        value.TryGetProperty("name", out var name)
                            ? name.GetString() ?? ""
                            : "",
                        MaxNameChars),
                    Loaded: value.TryGetProperty("loaded", out var loaded) &&
                            loaded.ValueKind == JsonValueKind.True));
            }
        }

        var loadedKey = root.TryGetProperty("loadedKey", out var key)
            ? key.GetString()
            : null;

        return new Manifest(entries, loadedKey?.Length <= 200 ? loadedKey : null);
    }

    // ----------------------------------------------------------------- helpers

    /// <summary>
    /// STORED, not deflated, with the CRC and size set explicitly.
    /// </summary>
    /// <remarks>
    /// Matches what the portal writes so the archives are interchangeable, and a
    /// 540-byte encrypted image does not compress meaningfully anyway.
    /// </remarks>
    private static void WriteStored(ZipArchive zip, string name, byte[] bytes)
    {
        var entry = zip.CreateEntry(name, CompressionLevel.NoCompression);
        using var stream = entry.Open();
        stream.Write(bytes, 0, bytes.Length);
    }

    /// <summary>Read one entry, refusing to allocate past its ceiling.</summary>
    /// <remarks>
    /// Bounded by what is actually read rather than by the entry's declared
    /// <c>Length</c>, which is attacker-controlled metadata: a zip bomb declares
    /// a small size and expands.
    /// </remarks>
    private static byte[] ReadBounded(ZipArchiveEntry entry, int limit, string what)
    {
        using var stream = entry.Open();
        using var output = new MemoryStream();
        var buffer = new byte[1024];

        while (true)
        {
            var count = stream.Read(buffer, 0, buffer.Length);
            if (count <= 0)
            {
                break;
            }

            if (output.Length + count > limit)
            {
                throw new InvalidOperationException($"{what} exceeds the archive limit");
            }

            output.Write(buffer, 0, count);
        }

        return output.ToArray();
    }

    /// <summary>
    /// Refuse anything that is not a plain filename.
    /// </summary>
    /// <remarks>
    /// The archive comes from outside, and its entry names are the classic
    /// path-traversal vector. No separators, no leading dot, no control
    /// characters — checked before the name is used for anything at all.
    /// </remarks>
    private static string SafeEntryName(string name)
    {
        if (string.IsNullOrWhiteSpace(name) || name.Length > MaxNameChars + 16)
        {
            throw new InvalidOperationException("Unsafe Amiibo archive filename");
        }

        if (name.Contains('/') || name.Contains('\\') || name == "." || name.StartsWith('.'))
        {
            throw new InvalidOperationException("Unsafe Amiibo archive filename");
        }

        foreach (var c in name)
        {
            if (c == '\0' || char.IsControl(c))
            {
                throw new InvalidOperationException("Unsafe Amiibo archive filename");
            }
        }

        return name;
    }

    private static string SafeBaseName(string value, string fallback)
    {
        var trimmed = value.Trim();
        if (trimmed.Length == 0)
        {
            trimmed = $"Amiibo {Truncate(fallback, 16)}";
        }

        var builder = new StringBuilder(trimmed.Length);
        foreach (var c in trimmed)
        {
            builder.Append(
                char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or ' ' or '-' ? c : '_');
        }

        var name = Truncate(builder.ToString().Trim(), MaxNameChars);
        if (name.Length == 0)
        {
            name = "Amiibo";
        }

        return name.EndsWith(".bin", StringComparison.OrdinalIgnoreCase) ? name : name + ".bin";
    }

    private static string UniqueName(string baseName, HashSet<string> used)
    {
        if (used.Add(baseName))
        {
            return baseName;
        }

        var stem = baseName.EndsWith(".bin", StringComparison.OrdinalIgnoreCase)
            ? baseName[..^4]
            : baseName;

        for (var suffix = 2; ; suffix++)
        {
            var candidate = $"{stem} ({suffix}).bin";
            if (used.Add(candidate))
            {
                return candidate;
            }
        }
    }

    /// <summary>
    /// How the portal names the loaded tag: by content, not by filename.
    /// </summary>
    /// <remarks>
    /// A v3 image includes its CRC because the same figure can exist in several
    /// distinct saved states; an NTAG215 figure is identified by its figure id
    /// alone. Both forms are the portal's, and changing either would silently
    /// lose the loaded marker on a round trip through it.
    /// </remarks>
    private static string PortalKey(byte[] bytes)
    {
        var identity = AmiiboCrypto.Identity(bytes);
        return identity.TagType == AmiiboTagType.FigureV3
            ? $"amiibo:{identity.FigureId}:{AmiiboFiles.Crc32(bytes).ToLowerInvariant()}"
            : $"amiibo:{identity.FigureId}";
    }

    private static string Truncate(string value, int max) =>
        value.Length <= max ? value : value[..max];
}
