using System.Text.Json;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>One tag backup the user owns, and what it is.</summary>
/// <remarks>
/// The identity fields are cached from the image rather than re-derived on every
/// read, so a library list draws without touching 40 files. They are refreshed
/// whenever the image is replaced, and the image is always authoritative — a
/// mismatch means the index is stale, never that the tag is.
/// </remarks>
public sealed record AmiiboLibraryItem
{
    public required string Id { get; init; }

    public required string DisplayName { get; init; }

    /// <summary>Relative to the library root; never a path the user chose.</summary>
    public required string FileName { get; init; }

    public required int Size { get; init; }

    public required string Crc32 { get; init; }

    public required string Uid { get; init; }

    public required string FigureId { get; init; }

    public required DateTimeOffset Imported { get; init; }

    public DateTimeOffset Updated { get; init; }

    /// <summary>The adapter's copy has changed and has not been synced back.</summary>
    public bool DirtyFromAdapter { get; init; }

    public string CharacterGameCode { get; init; } = "";

    public int CharacterVariant { get; init; }

    public AmiiboTagType TagType { get; init; } = AmiiboTagType.Ntag215;

    public string TypeName { get; init; } = "Figure";

    public string ModelNumber { get; init; } = "";

    public int SeriesCode { get; init; }

    public int FormatVersion { get; init; }

    public string ExtendedVariant { get; init; } = "";
}

public sealed record AmiiboImportResult(AmiiboLibraryItem Item, bool Duplicate);

public sealed record AmiiboArchiveImportResult(
    ValueList<AmiiboLibraryItem> Imported,
    int Duplicates,
    ValueList<string> Warnings);

/// <summary>
/// The user's local amiibo backups.
/// </summary>
/// <remarks>
/// ONE IMAGE PER FILE, PLUS AN INDEX. The images are the truth; the index is a
/// cache of what they are, so a corrupt or missing index costs display names and
/// ordering, never a backup. That asymmetry is the whole storage design: a tag
/// dump can be irreplaceable — the physical figure may have been written since —
/// and no metadata problem may ever be allowed to consume one.
///
/// Every mutation writes the image first, then the index, and rolls the image
/// back if the index write fails. The reverse order would leave an index
/// pointing at a file that does not exist.
///
/// Synchronous on purpose: the images are under 2 KB and the index is small, so
/// this costs microseconds. It is called from the UI thread and does not need a
/// task-per-rename.
/// </remarks>
public sealed class AmiiboLibrary
{
    private const string IndexName = "library.json";
    private const int IndexVersion = 1;

    private readonly string root;
    private readonly Lock gate = new();
    private readonly StateValue<ValueList<AmiiboLibraryItem>> items = new(ValueList<AmiiboLibraryItem>.Empty);
    private readonly StateValue<ValueList<string>> warnings = new(ValueList<string>.Empty);

    public AmiiboLibrary(string rootDirectory)
    {
        root = rootDirectory;
        Directory.CreateDirectory(root);
        items.Set(new ValueList<AmiiboLibraryItem>(LoadAndRecover()));

        // Rewrite the recovered index immediately, so a library repaired on this
        // launch is not re-repaired on every launch after it.
        try
        {
            PersistIndex(items.Value);
        }
        catch (Exception error)
        {
            AddWarning($"Library index could not be repaired: {error.Message}");
        }
    }

    public IReadOnlyStateValue<ValueList<AmiiboLibraryItem>> Items => items;

    /// <summary>What was wrong with the stored library, in the user's terms.</summary>
    public IReadOnlyStateValue<ValueList<string>> Warnings => warnings;

    public AmiiboLibraryItem? Find(string id) =>
        items.Value.FirstOrDefault(item => item.Id == id);

    /// <summary>The stored image. Throws when the file has gone missing.</summary>
    public byte[] Bytes(string id)
    {
        var item = Find(id)
            ?? throw new InvalidOperationException("That Amiibo is no longer in the library");
        return File.ReadAllBytes(PathFor(item.FileName));
    }

    /// <summary>
    /// Add a tag image to the library.
    /// </summary>
    /// <remarks>
    /// An identical image already present is reported as a DUPLICATE rather than
    /// stored twice, and the existing entry is returned so the caller can select
    /// it. Compared on the bytes, not on the identity: the same figure legitimately
    /// exists in several different saved states, and collapsing those would
    /// silently discard a backup.
    /// </remarks>
    public AmiiboImportResult Import(string displayName, string sourceName, byte[] raw)
    {
        lock (gate)
        {
            return ImportLocked(displayName, sourceName, raw);
        }
    }

    private AmiiboImportResult ImportLocked(string displayName, string sourceName, byte[] raw)
    {
        var normalized = AmiiboFiles.NormalizeImport(raw);
        var crc = AmiiboFiles.Crc32(normalized);
        var identity = AmiiboCrypto.Identity(normalized);

        foreach (var candidate in items.Value.Where(
                     item => item.Size == normalized.Length &&
                             string.Equals(item.Crc32, crc, StringComparison.OrdinalIgnoreCase)))
        {
            // CRC agreement is not identity. It narrows the search; the byte
            // comparison decides.
            try
            {
                if (File.ReadAllBytes(PathFor(candidate.FileName)).AsSpan()
                    .SequenceEqual(normalized))
                {
                    return new AmiiboImportResult(candidate, Duplicate: true);
                }
            }
            catch (IOException)
            {
                // An unreadable candidate simply is not a match.
            }
        }

        var id = Guid.NewGuid().ToString("N");
        var fileName = $"{id}.bin";
        var now = DateTimeOffset.UtcNow;
        WriteAtomic(PathFor(fileName), normalized);

        var item = Describe(
            new AmiiboLibraryItem
            {
                Id = id,
                DisplayName = CleanName(displayName, sourceName),
                FileName = fileName,
                Size = normalized.Length,
                Crc32 = crc,
                Uid = identity.Uid,
                FigureId = identity.FigureId,
                Imported = now,
                Updated = now,
            },
            identity);

        var next = new ValueList<AmiiboLibraryItem>(items.Value.Prepend(item));
        try
        {
            PersistIndex(next);
        }
        catch
        {
            // The index is the thing that failed, so the orphan image is removed
            // rather than left for a future recovery pass to adopt under a name
            // the user never chose.
            TryDelete(PathFor(fileName));
            throw;
        }

        items.Set(next);
        return new AmiiboImportResult(item, Duplicate: false);
    }

    /// <summary>
    /// Replace a library entry with what the adapter now holds.
    /// </summary>
    /// <remarks>
    /// The sync-back path, and the one place an existing backup is overwritten.
    /// The previous image is copied aside first and restored if the index write
    /// fails, because the bytes being replaced may be the only copy of a state
    /// the physical figure has already moved past.
    ///
    /// Matching prefers the entry the caller named, but only if its UID agrees —
    /// a stale selection must never cause one figure's backup to be overwritten
    /// with another's. With no usable match the tag is imported as new rather
    /// than dropped.
    /// </remarks>
    public AmiiboLibraryItem UpdateFromAdapter(string? id, byte[] data)
    {
        lock (gate)
        {
            AmiiboFiles.Validate(data);
            var identity = AmiiboCrypto.Identity(data);

            var existing =
                (id is null ? null : Find(id)) is { } named && named.Uid == identity.Uid
                    ? named
                    : items.Value.FirstOrDefault(
                        item => item.Uid == identity.Uid && item.FigureId == identity.FigureId);

            if (existing is null)
            {
                return ImportLocked("Synced Amiibo", "adapter.bin", data).Item;
            }

            var destination = PathFor(existing.FileName);
            var rollback = PathFor($"{existing.Id}.rollback");
            if (File.Exists(destination))
            {
                File.Copy(destination, rollback, overwrite: true);
            }

            WriteAtomic(destination, data);

            var updated = Describe(
                existing with
                {
                    Size = data.Length,
                    Crc32 = AmiiboFiles.Crc32(data),
                    Uid = identity.Uid,
                    FigureId = identity.FigureId,
                    Updated = DateTimeOffset.UtcNow,
                    DirtyFromAdapter = false,
                },
                identity);

            var next = new ValueList<AmiiboLibraryItem>(
                items.Value.Select(item => item.Id == updated.Id ? updated : item));

            try
            {
                PersistIndex(next);
                TryDelete(rollback);
            }
            catch
            {
                if (File.Exists(rollback))
                {
                    File.Move(rollback, destination, overwrite: true);
                }

                throw;
            }

            items.Set(next);
            return updated;
        }
    }

    public void Rename(string id, string displayName)
    {
        lock (gate)
        {
            var next = new ValueList<AmiiboLibraryItem>(items.Value.Select(item =>
                item.Id == id
                    ? item with { DisplayName = CleanName(displayName, item.DisplayName) }
                    : item));
            PersistIndex(next);
            items.Set(next);
        }
    }

    /// <summary>Remove a backup, image and all.</summary>
    /// <remarks>
    /// The index is written FIRST here, which is the opposite of import and
    /// deliberately so: if the file delete then fails, the user has lost an entry
    /// they asked to lose and gained a stray file, rather than kept an entry
    /// pointing at bytes that are gone.
    /// </remarks>
    public void Delete(string id)
    {
        lock (gate)
        {
            var item = Find(id);
            if (item is null)
            {
                return;
            }

            var next = new ValueList<AmiiboLibraryItem>(
                items.Value.Where(candidate => candidate.Id != id));
            PersistIndex(next);
            items.Set(next);
            TryDelete(PathFor(item.FileName));
        }
    }

    /// <summary>Mark an entry as changed on the adapter since it was uploaded.</summary>
    public void MarkDirtyFromAdapter(string id, bool dirty)
    {
        lock (gate)
        {
            var next = new ValueList<AmiiboLibraryItem>(items.Value.Select(item =>
                item.Id == id ? item with { DirtyFromAdapter = dirty } : item));
            PersistIndex(next);
            items.Set(next);
        }
    }

    // ---------------------------------------------------------------- archive

    /// <summary>Export the whole library, marking one entry as the loaded tag.</summary>
    public byte[] ExportArchive(string? loadedId = null)
    {
        lock (gate)
        {
            var exported = new List<AmiiboArchive.ExportItem>();
            foreach (var item in items.Value)
            {
                byte[] bytes;
                try
                {
                    bytes = File.ReadAllBytes(PathFor(item.FileName));
                }
                catch (IOException)
                {
                    // One unreadable image must not cost the user the export of
                    // everything else they own.
                    AddWarning($"'{item.DisplayName}' could not be read and was not exported");
                    continue;
                }

                exported.Add(new AmiiboArchive.ExportItem(item, bytes, item.Id == loadedId));
            }

            if (exported.Count == 0)
            {
                throw new InvalidOperationException("There is nothing in the library to export");
            }

            return AmiiboArchive.Write(exported);
        }
    }

    /// <summary>
    /// Import every entry of an archive, skipping ones already held.
    /// </summary>
    /// <remarks>
    /// The archive is fully parsed and validated before anything is written, so a
    /// malformed one changes nothing. Individual entries that fail after that are
    /// reported and skipped rather than aborting the rest — a user importing
    /// twenty backups should not lose nineteen to one bad file.
    /// </remarks>
    public AmiiboArchiveImportResult ImportArchive(byte[] raw)
    {
        var entries = AmiiboArchive.Read(raw);

        lock (gate)
        {
            var imported = new List<AmiiboLibraryItem>();
            var problems = new List<string>();
            var duplicates = 0;

            foreach (var entry in entries)
            {
                try
                {
                    var result = ImportLocked(entry.DisplayName, entry.FileName, entry.Bytes);
                    if (result.Duplicate)
                    {
                        duplicates++;
                    }
                    else
                    {
                        imported.Add(result.Item);
                    }
                }
                catch (Exception error)
                {
                    problems.Add($"'{entry.DisplayName}' was not imported: {error.Message}");
                }
            }

            return new AmiiboArchiveImportResult(
                new ValueList<AmiiboLibraryItem>(imported),
                duplicates,
                new ValueList<string>(problems));
        }
    }

    // ------------------------------------------------------------- persistence

    private string PathFor(string fileName) => Path.Combine(root, fileName);

    /// <summary>
    /// Read the index, and reconcile it against what is actually on disk.
    /// </summary>
    /// <remarks>
    /// TWO RECOVERIES, both silent-data-loss cases:
    ///
    /// An index entry whose image is missing is DROPPED — it describes a backup
    /// that no longer exists, and listing it would offer the user an upload that
    /// cannot happen.
    ///
    /// An image with no index entry is ADOPTED under a generated name. That file
    /// is somebody's tag backup; the alternative is that a damaged index quietly
    /// orphans real data that is still sitting on disk.
    /// </remarks>
    private List<AmiiboLibraryItem> LoadAndRecover()
    {
        var stored = new List<AmiiboLibraryItem>();

        try
        {
            var path = PathFor(IndexName);
            if (File.Exists(path))
            {
                stored = ParseIndex(File.ReadAllText(path));
            }
        }
        catch (Exception error)
        {
            AddWarning(
                "The Amiibo library index could not be read; entries were recovered from " +
                $"the stored files. ({error.Message})");
            stored = [];
        }

        var recovered = new List<AmiiboLibraryItem>();
        var claimed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (var item in stored)
        {
            if (!File.Exists(PathFor(item.FileName)))
            {
                AddWarning($"'{item.DisplayName}' is listed but its backup file is missing");
                continue;
            }

            claimed.Add(item.FileName);
            recovered.Add(item);
        }

        foreach (var path in SafeEnumerate())
        {
            var fileName = Path.GetFileName(path);
            if (claimed.Contains(fileName) ||
                !fileName.EndsWith(".bin", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            try
            {
                var bytes = File.ReadAllBytes(path);
                AmiiboFiles.Validate(bytes);
                var identity = AmiiboCrypto.Identity(bytes);
                var stamp = File.GetLastWriteTimeUtc(path);

                recovered.Add(Describe(
                    new AmiiboLibraryItem
                    {
                        Id = Path.GetFileNameWithoutExtension(fileName),
                        DisplayName = $"Recovered {identity.FigureId}",
                        FileName = fileName,
                        Size = bytes.Length,
                        Crc32 = AmiiboFiles.Crc32(bytes),
                        Uid = identity.Uid,
                        FigureId = identity.FigureId,
                        Imported = stamp,
                        Updated = stamp,
                    },
                    identity));

                AddWarning($"Recovered an Amiibo backup that was missing from the index ({fileName})");
            }
            catch (Exception)
            {
                // Not a tag image, or unreadable. Left alone rather than deleted:
                // this directory is the user's, and guessing is not worth the risk.
            }
        }

        return recovered;
    }

    private IEnumerable<string> SafeEnumerate()
    {
        try
        {
            return Directory.EnumerateFiles(root, "*.bin");
        }
        catch (IOException)
        {
            return [];
        }
    }

    private List<AmiiboLibraryItem> ParseIndex(string text)
    {
        using var document = JsonDocument.Parse(text);
        var root = document.RootElement;

        var result = new List<AmiiboLibraryItem>();
        if (!root.TryGetProperty("items", out var array) ||
            array.ValueKind != JsonValueKind.Array)
        {
            return result;
        }

        foreach (var value in array.EnumerateArray())
        {
            // One unreadable row costs that row. The image is still on disk and
            // the adoption pass above will pick it up.
            try
            {
                result.Add(new AmiiboLibraryItem
                {
                    Id = value.GetProperty("id").GetString()!,
                    DisplayName = value.GetProperty("displayName").GetString()!,
                    FileName = value.GetProperty("fileName").GetString()!,
                    Size = value.GetProperty("size").GetInt32(),
                    Crc32 = value.GetProperty("crc32").GetString()!,
                    Uid = value.GetProperty("uid").GetString()!,
                    FigureId = value.GetProperty("figureId").GetString()!,
                    Imported = DateTimeOffset.Parse(value.GetProperty("imported").GetString()!),
                    Updated = value.TryGetProperty("updated", out var updated)
                        ? DateTimeOffset.Parse(updated.GetString()!)
                        : DateTimeOffset.Parse(value.GetProperty("imported").GetString()!),
                    DirtyFromAdapter = value.TryGetProperty("dirtyFromAdapter", out var dirty) &&
                                       dirty.ValueKind == JsonValueKind.True,
                    CharacterGameCode = Text(value, "characterGameCode"),
                    CharacterVariant = Number(value, "characterVariant"),
                    TagType = Text(value, "tagType") == nameof(AmiiboTagType.FigureV3)
                        ? AmiiboTagType.FigureV3
                        : AmiiboTagType.Ntag215,
                    TypeName = Text(value, "typeName"),
                    ModelNumber = Text(value, "modelNumber"),
                    SeriesCode = Number(value, "seriesCode"),
                    FormatVersion = Number(value, "formatVersion"),
                    ExtendedVariant = Text(value, "extendedVariant"),
                });
            }
            catch (Exception)
            {
                // Skipped deliberately; see above.
            }
        }

        return result;
    }

    private void PersistIndex(IEnumerable<AmiiboLibraryItem> next)
    {
        var document = new Dictionary<string, object>
        {
            ["version"] = IndexVersion,
            ["items"] = next.Select(item => new Dictionary<string, object>
            {
                ["id"] = item.Id,
                ["displayName"] = item.DisplayName,
                ["fileName"] = item.FileName,
                ["size"] = item.Size,
                ["crc32"] = item.Crc32,
                ["uid"] = item.Uid,
                ["figureId"] = item.FigureId,
                ["imported"] = item.Imported.ToString("O"),
                ["updated"] = item.Updated.ToString("O"),
                ["dirtyFromAdapter"] = item.DirtyFromAdapter,
                ["characterGameCode"] = item.CharacterGameCode,
                ["characterVariant"] = item.CharacterVariant,
                ["tagType"] = item.TagType.ToString(),
                ["typeName"] = item.TypeName,
                ["modelNumber"] = item.ModelNumber,
                ["seriesCode"] = item.SeriesCode,
                ["formatVersion"] = item.FormatVersion,
                ["extendedVariant"] = item.ExtendedVariant,
            }).ToList(),
        };

        WriteAtomic(
            PathFor(IndexName),
            JsonSerializer.SerializeToUtf8Bytes(
                document, new JsonSerializerOptions { WriteIndented = true }));
    }

    /// <summary>
    /// Write through a temporary file and move it into place.
    /// </summary>
    /// <remarks>
    /// A half-written index or image is the failure this exists to prevent: the
    /// move is the only step that is observable, so a crash mid-write leaves the
    /// previous content intact rather than a truncated file that parses to
    /// nothing.
    /// </remarks>
    private static void WriteAtomic(string path, byte[] bytes)
    {
        var temporary = path + ".tmp";
        File.WriteAllBytes(temporary, bytes);
        File.Move(temporary, path, overwrite: true);
    }

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
            // Best effort; a stray file is not worth failing an operation over.
        }
    }

    private static AmiiboLibraryItem Describe(AmiiboLibraryItem item, AmiiboIdentity identity) =>
        item with
        {
            CharacterGameCode = identity.CharacterGameCode,
            CharacterVariant = identity.CharacterVariant,
            TagType = identity.TagType,
            TypeName = identity.TypeName,
            ModelNumber = identity.ModelNumber,
            SeriesCode = identity.SeriesCode,
            FormatVersion = identity.FormatVersion,
            ExtendedVariant = identity.ExtendedVariant,
        };

    private static string CleanName(string value, string fallback)
    {
        var trimmed = value.Trim();
        if (trimmed.Length > 0)
        {
            return trimmed.Length > AmiiboArchive.MaxNameChars
                ? trimmed[..AmiiboArchive.MaxNameChars]
                : trimmed;
        }

        var stem = Path.GetFileNameWithoutExtension(fallback).Trim();
        return stem.Length > 0 ? stem : "Amiibo";
    }

    private static string Text(JsonElement value, string name) =>
        value.TryGetProperty(name, out var found) && found.ValueKind == JsonValueKind.String
            ? found.GetString() ?? ""
            : "";

    private static int Number(JsonElement value, string name) =>
        value.TryGetProperty(name, out var found) && found.ValueKind == JsonValueKind.Number
            ? found.GetInt32()
            : 0;

    private void AddWarning(string message)
    {
        if (!warnings.Value.Contains(message))
        {
            warnings.Set(new ValueList<string>(warnings.Value.Append(message)));
        }
    }
}
