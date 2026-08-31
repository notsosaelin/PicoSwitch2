using System.IO.Compression;
using System.Text;
using System.Text.Json;
using PicoSwitch.Companion.Services;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The local amiibo library and its exchange archive.
/// </summary>
/// <remarks>
/// THE ASYMMETRY UNDER TEST: a tag dump can be irreplaceable. The physical
/// figure may have been written since, so the bytes in this library may be the
/// only record of a state that no longer exists anywhere. Every rule here
/// follows from that — the index is a cache and the images are the truth, a
/// damaged index costs names and never backups, an orphaned image is adopted
/// rather than ignored, and a failed write rolls back rather than half-applying.
///
/// Driven against real tag images from <c>dumps/amiibo/</c> rather than
/// synthesised bytes, because the validation being tested is precisely what
/// distinguishes a real dump from plausible-looking bytes.
/// </remarks>
public sealed class AmiiboLibraryTests : IDisposable
{
    private readonly string root =
        Path.Combine(Path.GetTempPath(), "picoswitch-amiibo-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
        catch (IOException)
        {
        }
    }

    private AmiiboLibrary Library() => new(root);

    /// <summary>A real 540-byte NTAG215 dump.</summary>
    private static byte[] Ntag215() => Tag("ntag215-animal-crossing-tom-nook.bin");

    /// <summary>A real 2048-byte figure v3 dump.</summary>
    private static byte[] FigureV3() => Tag("v3-kirby-tank-star-factory.bin");

    private static byte[] Tag(string name) =>
        File.ReadAllBytes(RepositoryFile($"dumps/amiibo/{name}"));

    // Real dumps are read from the repository rather than copied into the test
    // project: a copy is a second authority, and the validation being tested is
    // exactly what separates a genuine dump from plausible-looking bytes.
    private static string RepositoryFile(string relativePath)
    {
        var cursor = new DirectoryInfo(AppContext.BaseDirectory);
        while (cursor is not null)
        {
            var candidate = Path.Combine(cursor.FullName, relativePath);
            if (File.Exists(candidate))
            {
                return candidate;
            }

            cursor = cursor.Parent;
        }

        throw new FileNotFoundException(
            $"Cannot find {relativePath} above {AppContext.BaseDirectory}");
    }

    // ------------------------------------------------------------- importing

    [Fact]
    public void ImportStoresTheImageAndDescribesIt()
    {
        var library = Library();
        var result = library.Import("Tom Nook", "whatever.bin", Ntag215());

        Assert.False(result.Duplicate);
        Assert.Equal("Tom Nook", result.Item.DisplayName);
        Assert.Equal(540, result.Item.Size);
        Assert.Equal(AmiiboTagType.Ntag215, result.Item.TagType);
        Assert.Equal("0183000002420502", result.Item.FigureId);
        Assert.Single(library.Items.Value);

        // The image round-trips byte for byte; the library is a store, not a
        // transformer.
        Assert.Equal(Ntag215(), library.Bytes(result.Item.Id));
    }

    [Fact]
    public void ImportingTheSameImageTwiceIsReportedAsADuplicate()
    {
        var library = Library();
        var first = library.Import("Tom Nook", "a.bin", Ntag215());
        var second = library.Import("Tom Nook again", "b.bin", Ntag215());

        Assert.True(second.Duplicate);
        Assert.Equal(first.Item.Id, second.Item.Id);
        Assert.Single(library.Items.Value);
    }

    [Fact]
    public void TheSameFigureInADifferentStateIsNotADuplicate()
    {
        // Duplicate means the same BYTES, not the same figure. Two saved states
        // of one amiibo are two backups, and collapsing them would silently
        // discard one of them.
        var library = Library();
        var trained = Tag("v3-air-riders-trained-before-save-2026-07-28.bin");
        var saved = Tag("v3-air-riders-learned-state-after-save-2026-07-28.bin");
        Assert.Equal(
            AmiiboCrypto.Identity(trained).FigureId,
            AmiiboCrypto.Identity(saved).FigureId);

        library.Import("Before", "a.bin", trained);
        var second = library.Import("After", "b.bin", saved);

        Assert.False(second.Duplicate);
        Assert.Equal(2, library.Items.Value.Count);
    }

    [Fact]
    public void ImportRefusesSomethingThatIsNotATagImage()
    {
        var library = Library();
        Assert.ThrowsAny<Exception>(() => library.Import("Nope", "x.bin", new byte[100]));
        Assert.Empty(library.Items.Value);
        // Nothing was left behind by the failed attempt.
        Assert.Empty(Directory.EnumerateFiles(root, "*.bin"));
    }

    [Fact]
    public void ABlankNameFallsBackToTheSourceFileName()
    {
        var library = Library();
        var item = library.Import("   ", "Tom Nook.bin", Ntag215()).Item;
        Assert.Equal("Tom Nook", item.DisplayName);
    }

    // ------------------------------------------------------------- lifecycle

    [Fact]
    public void RenameChangesOnlyTheName()
    {
        var library = Library();
        var item = library.Import("Tom Nook", "a.bin", Ntag215()).Item;

        library.Rename(item.Id, "Nook Inc.");
        var renamed = library.Find(item.Id)!;

        Assert.Equal("Nook Inc.", renamed.DisplayName);
        Assert.Equal(item.Crc32, renamed.Crc32);
        Assert.Equal(Ntag215(), library.Bytes(item.Id));
    }

    [Fact]
    public void DeleteRemovesTheEntryAndItsImage()
    {
        var library = Library();
        var item = library.Import("Tom Nook", "a.bin", Ntag215()).Item;
        var path = Path.Combine(root, item.FileName);
        Assert.True(File.Exists(path));

        library.Delete(item.Id);

        Assert.Empty(library.Items.Value);
        Assert.False(File.Exists(path));
    }

    [Fact]
    public void ALibraryReloadsFromDisk()
    {
        var first = Library();
        first.Import("Tom Nook", "a.bin", Ntag215());
        first.Import("Kirby", "b.bin", FigureV3());

        var reopened = Library();

        Assert.Equal(2, reopened.Items.Value.Count);
        Assert.Empty(reopened.Warnings.Value);
        Assert.Contains(reopened.Items.Value, item => item.DisplayName == "Kirby");
    }

    // -------------------------------------------------------------- recovery

    [Fact]
    public void AnUnreadableIndexRecoversTheImagesRatherThanLosingThem()
    {
        // THE CASE THAT MATTERS MOST. The index is metadata; the images may be
        // the only copy of a tag state that no longer exists. A damaged index
        // must cost the names, never the backups.
        var first = Library();
        first.Import("Tom Nook", "a.bin", Ntag215());
        first.Import("Kirby", "b.bin", FigureV3());

        File.WriteAllText(Path.Combine(root, "library.json"), "{ this is not json");

        var recovered = Library();

        Assert.Equal(2, recovered.Items.Value.Count);
        Assert.NotEmpty(recovered.Warnings.Value);
        // The bytes survived intact even though every name was lost.
        var uids = recovered.Items.Value.Select(item => item.Uid).ToHashSet();
        Assert.Contains(AmiiboCrypto.Identity(Ntag215()).Uid, uids);
        Assert.Contains(AmiiboCrypto.Identity(FigureV3()).Uid, uids);
    }

    [Fact]
    public void AnIndexEntryWithNoImageIsDroppedAndReported()
    {
        // The opposite direction: listing a backup whose file is gone would offer
        // the user an upload that cannot happen.
        var first = Library();
        var item = first.Import("Tom Nook", "a.bin", Ntag215()).Item;
        File.Delete(Path.Combine(root, item.FileName));

        var recovered = Library();

        Assert.Empty(recovered.Items.Value);
        Assert.Contains(recovered.Warnings.Value, w => w.Contains("missing"));
    }

    [Fact]
    public void ARecoveredLibraryIsNotRepairedAgainOnEveryLaunch()
    {
        var first = Library();
        first.Import("Tom Nook", "a.bin", Ntag215());
        File.WriteAllText(Path.Combine(root, "library.json"), "not json");

        Assert.NotEmpty(Library().Warnings.Value);
        // The repair was persisted, so the next launch is clean.
        Assert.Empty(Library().Warnings.Value);
    }

    // ---------------------------------------------------------- adapter sync

    [Fact]
    public void UpdateFromAdapterReplacesTheNamedEntry()
    {
        // Two states of the SAME physical tag -- same UID, different contents --
        // which is exactly what syncing back after a console write looks like.
        var library = Library();
        var before = Tag("v3-air-riders-trained-before-save-2026-07-28.bin");
        var after = Tag("v3-air-riders-learned-state-after-save-2026-07-28.bin");
        Assert.Equal(AmiiboCrypto.Identity(before).Uid, AmiiboCrypto.Identity(after).Uid);

        var item = library.Import("Kirby", "a.bin", before).Item;
        var updated = library.UpdateFromAdapter(item.Id, after);

        Assert.Equal(item.Id, updated.Id);
        Assert.Equal("Kirby", updated.DisplayName);
        Assert.Equal(after, library.Bytes(item.Id));
        Assert.Single(library.Items.Value);
    }

    [Fact]
    public void UpdateFromAdapterRefusesToOverwriteADifferentFigure()
    {
        // A stale selection must never cost the user an unrelated backup. The
        // named entry is used only when its UID agrees; otherwise the tag is
        // matched by identity, and failing that, imported as new.
        var library = Library();
        var nook = library.Import("Tom Nook", "a.bin", Ntag215()).Item;

        var result = library.UpdateFromAdapter(nook.Id, FigureV3());

        Assert.NotEqual(nook.Id, result.Id);
        Assert.Equal(2, library.Items.Value.Count);
        Assert.Equal(Ntag215(), library.Bytes(nook.Id));
    }

    [Fact]
    public void AnUnknownTagFromTheAdapterIsImportedRatherThanDropped()
    {
        var library = Library();
        var synced = library.UpdateFromAdapter(id: null, FigureV3());

        Assert.Single(library.Items.Value);
        Assert.Equal(FigureV3(), library.Bytes(synced.Id));
    }

    // --------------------------------------------------------------- archive

    [Fact]
    public void AnArchiveRoundTripsThroughAFreshLibrary()
    {
        var source = Library();
        var nook = source.Import("Tom Nook", "a.bin", Ntag215()).Item;
        source.Import("Kirby", "b.bin", FigureV3());

        var archive = source.ExportArchive(loadedId: nook.Id);

        using var second = new AmiiboLibraryTests();
        var target = second.Library();
        var result = target.ImportArchive(archive);

        Assert.Equal(2, result.Imported.Count);
        Assert.Equal(0, result.Duplicates);
        Assert.Empty(result.Warnings);

        // Names survive, and so do the bytes.
        var imported = target.Items.Value.Single(item => item.DisplayName == "Tom Nook");
        Assert.Equal(Ntag215(), target.Bytes(imported.Id));
    }

    [Fact]
    public void ImportingTheSameArchiveTwiceAddsNothing()
    {
        var library = Library();
        library.Import("Tom Nook", "a.bin", Ntag215());
        var archive = library.ExportArchive();

        var again = library.ImportArchive(archive);

        Assert.Empty(again.Imported);
        Assert.Equal(1, again.Duplicates);
        Assert.Single(library.Items.Value);
    }

    [Fact]
    public void TheArchiveIsAPortalCompatibleZip()
    {
        // The format is shared with the Android companion and the web portal, so
        // these are contract assertions, not implementation details: a v3
        // manifest named library.json, written first, with stored entries.
        var library = Library();
        library.Import("Tom Nook", "a.bin", Ntag215());
        var archive = library.ExportArchive();

        Assert.Equal((byte)'P', archive[0]);
        Assert.Equal((byte)'K', archive[1]);

        using var zip = new ZipArchive(new MemoryStream(archive), ZipArchiveMode.Read);
        Assert.Equal("library.json", zip.Entries[0].FullName);
        Assert.Contains(zip.Entries, entry => entry.FullName.EndsWith(".bin", StringComparison.Ordinal));

        using var manifest = JsonDocument.Parse(ReadEntry(zip.Entries[0]));
        Assert.Equal(AmiiboArchive.Format, manifest.RootElement.GetProperty("format").GetString());
        Assert.Equal(AmiiboArchive.Version, manifest.RootElement.GetProperty("version").GetInt32());
        Assert.Single(manifest.RootElement.GetProperty("entries").EnumerateArray());
    }

    [Fact]
    public void TheLoadedMarkerSurvivesTheRoundTrip()
    {
        var library = Library();
        library.Import("Tom Nook", "a.bin", Ntag215());
        var kirby = library.Import("Kirby", "b.bin", FigureV3()).Item;

        var entries = AmiiboArchive.Read(library.ExportArchive(loadedId: kirby.Id));

        Assert.Single(entries, entry => entry.Loaded);
        Assert.Equal("Kirby", entries.Single(entry => entry.Loaded).DisplayName);
    }

    [Fact]
    public void AnArchiveWithNoManifestStillImports()
    {
        // Losing display names is a far better outcome than refusing somebody's
        // backup because its metadata was damaged.
        var library = Library();
        library.Import("Tom Nook", "a.bin", Ntag215());
        var archive = StripManifest(library.ExportArchive());

        var entries = AmiiboArchive.Read(archive);

        Assert.Single(entries);
        Assert.Equal(Ntag215(), entries[0].Bytes);
    }

    [Fact]
    public void AnyZipHoldingDumpsIsReadable()
    {
        // NOT limited to our own export format. People keep amiibo dumps in
        // ordinary zips, in folders, next to readmes and cover art. Demanding a
        // re-export from another tool would make this the most annoying way to
        // move a collection.
        var collection = BuildZip(
            ("Animal Crossing/Tom Nook.bin", Ntag215()),
            ("Kirby/Air Riders/Kirby & Tank Star.bin", FigureV3()),
            ("readme.txt", Encoding.UTF8.GetBytes("my amiibo dumps")),
            ("covers/nook.png", [0x89, 0x50, 0x4E, 0x47]));

        var entries = AmiiboArchive.Read(collection);

        Assert.Equal(2, entries.Count);
        // Named from the file, not the folder path, and without the extension.
        Assert.Contains(entries, entry => entry.DisplayName == "Tom Nook");
        Assert.Contains(entries, entry => entry.DisplayName == "Kirby & Tank Star");
        Assert.DoesNotContain(entries, entry => entry.DisplayName.Contains('/'));
        Assert.Contains(entries, entry => entry.Bytes.SequenceEqual(Ntag215()));
        Assert.Contains(entries, entry => entry.Bytes.SequenceEqual(FigureV3()));
    }

    [Fact]
    public void NestedPathsCannotEscapeBecauseTheyAreNeverUsedAsPaths()
    {
        // The classic traversal vector is defused by construction rather than by
        // rejection: the library stores every image under a generated name, so an
        // archive path is display text and never reaches the filesystem.
        var hostile = BuildZip(("../../escaped.bin", Ntag215()));

        var entries = AmiiboArchive.Read(hostile);

        Assert.Single(entries);
        Assert.DoesNotContain("..", entries[0].DisplayName);
        Assert.DoesNotContain("/", entries[0].DisplayName);

        var library = Library();
        var item = library.ImportMany([new AmiiboImportSource("hostile.zip", hostile)])
            .Imported.Single();
        // Stored under a generated name inside the library root.
        Assert.Equal($"{item.Id}.bin", item.FileName);
        Assert.True(File.Exists(Path.Combine(root, item.FileName)));
    }

    [Fact]
    public void OneBadDumpDoesNotCostTheRest()
    {
        // A folder of five hundred good dumps and one stray file must import
        // four hundred and ninety-nine, not zero.
        var mixed = BuildZip(
            ("good.bin", Ntag215()),
            ("bogus.bin", new byte[540]),
            ("also-good.bin", FigureV3()));

        var entries = AmiiboArchive.Read(mixed);

        Assert.Equal(2, entries.Count);
    }

    [Fact]
    public void AnArchiveWithNothingUsableIsStillRefused()
    {
        Assert.ThrowsAny<Exception>(() => AmiiboArchive.Read(Encoding.UTF8.GetBytes("not a zip")));
        Assert.ThrowsAny<Exception>(() => AmiiboArchive.Read(BuildZip(("notes.txt", [1, 2, 3]))));
        Assert.ThrowsAny<Exception>(() =>
            AmiiboArchive.Read(BuildZip(("bogus.bin", new byte[540]))));
        // A ZIP with only a manifest and no images has nothing to import.
        Assert.ThrowsAny<Exception>(() =>
            AmiiboArchive.Read(BuildZip(("library.json", Encoding.UTF8.GetBytes("{}")))));
    }

    // ----------------------------------------------------------- bulk import

    [Fact]
    public void ManyLooseDumpsImportInOneGo()
    {
        var library = Library();
        var result = library.ImportMany(
        [
            new AmiiboImportSource("Tom Nook.bin", Ntag215()),
            new AmiiboImportSource("Kirby.bin", FigureV3()),
        ]);

        Assert.Equal(2, result.Imported.Count);
        Assert.Equal(0, result.Duplicates);
        Assert.Equal(0, result.Skipped);
        Assert.Equal(2, library.Items.Value.Count);
        // Named from the file, with the extension dropped.
        Assert.Contains(library.Items.Value, item => item.DisplayName == "Tom Nook");
    }

    [Fact]
    public void DumpsAndArchivesMixFreelyInOneImport()
    {
        // The user should never have to know, or tell the app, which kind of file
        // they are holding.
        var library = Library();
        var archive = BuildZip(("Kirby.bin", FigureV3()));

        var result = library.ImportMany(
        [
            new AmiiboImportSource("Tom Nook.bin", Ntag215()),
            new AmiiboImportSource("collection.zip", archive),
        ]);

        Assert.Equal(2, result.Imported.Count);
        Assert.Equal(2, library.Items.Value.Count);
    }

    [Fact]
    public void NonTagFilesAreSkippedRatherThanFailing()
    {
        // Pointing this at a folder is EXPECTED to sweep up readmes and cover
        // art. Reporting those as errors would make a successful import of
        // hundreds of tags look broken.
        var library = Library();
        var result = library.ImportMany(
        [
            new AmiiboImportSource("Tom Nook.bin", Ntag215()),
            new AmiiboImportSource("readme.txt", Encoding.UTF8.GetBytes("hello")),
            new AmiiboImportSource("cover.png", [0x89, 0x50, 0x4E, 0x47]),
        ]);

        Assert.Single(result.Imported);
        Assert.Equal(2, result.Skipped);
        Assert.Equal(2, result.Problems.Count);
        Assert.Contains("1 added", result.Summary);
        Assert.Contains("2 skipped", result.Summary);
    }

    [Fact]
    public void DuplicatesAcrossABulkImportAreCountedNotStored()
    {
        var library = Library();
        library.Import("Tom Nook", "a.bin", Ntag215());

        var result = library.ImportMany(
        [
            new AmiiboImportSource("Tom Nook.bin", Ntag215()),
            new AmiiboImportSource("Tom Nook copy.bin", Ntag215()),
            new AmiiboImportSource("Kirby.bin", FigureV3()),
        ]);

        Assert.Single(result.Imported);
        Assert.Equal(2, result.Duplicates);
        Assert.Equal(2, library.Items.Value.Count);
        Assert.Contains("already in your library", result.Summary);
    }

    [Fact]
    public void AnUnreadableArchiveIsOneSkipNotAFailedImport()
    {
        var library = Library();
        var result = library.ImportMany(
        [
            new AmiiboImportSource("broken.zip", Encoding.UTF8.GetBytes("PK not really")),
            new AmiiboImportSource("Tom Nook.bin", Ntag215()),
        ]);

        Assert.Single(result.Imported);
        Assert.Equal(1, result.Skipped);
        Assert.Single(library.Items.Value);
    }

    [Fact]
    public void ImportingNothingSaysSoRatherThanClaimingSuccess()
    {
        var result = Library().ImportMany([]);
        Assert.Equal(0, result.Considered);
        Assert.Equal("Nothing to import.", result.Summary);
    }

    [Fact]
    public void ABulkImportOfARealCollectionSucceeds()
    {
        // Every tracked dump at once, which is what pointing this at a folder
        // looks like: a mix of both tag sizes, several states of the same figure,
        // and two files that are not tags at all.
        var library = Library();
        var sources = Directory
            .EnumerateFiles(RepositoryFile("dumps/amiibo/ntag215-animal-crossing-tom-nook.bin")
                .Replace("ntag215-animal-crossing-tom-nook.bin", ""), "*.bin")
            .Select(path => new AmiiboImportSource(Path.GetFileName(path), File.ReadAllBytes(path)))
            .ToList();

        var result = library.ImportMany(sources);

        // The two 668-byte captures are not tag images and are skipped.
        Assert.Equal(2, result.Skipped);
        Assert.Equal(sources.Count - 2, result.Imported.Count);
        Assert.Equal(result.Imported.Count, library.Items.Value.Count);
    }

    [Fact]
    public void ExportingAnEmptyLibraryIsRefusedRatherThanWritingAnEmptyZip()
    {
        Assert.Throws<InvalidOperationException>(() => Library().ExportArchive());
    }

    // ------------------------------------------------------- cross-platform

    [Fact]
    public void AnArchiveWrittenByTheAndroidCompanionImportsHere()
    {
        // THE INTERCHANGE CLAIM, tested rather than asserted. "Both sides
        // implement the same spec" is not evidence that they interoperate; this
        // ZIP was written by the Kotlin implementation over the same real dumps
        // and checked in, so a one-sided change to either reader or writer breaks
        // here rather than on a user's machine.
        var archive = File.ReadAllBytes(
            RepositoryFile("tools/fixtures/amiibo/library-archive-v3.zip"));

        var library = Library();
        var result = library.ImportArchive(archive);

        Assert.Equal(2, result.Imported.Count);
        Assert.Empty(result.Warnings);

        // Names survived the manifest.
        var names = library.Items.Value.Select(item => item.DisplayName).ToHashSet();
        Assert.Contains("Tom Nook", names);
        Assert.Contains("Kirby & Tank Star", names);

        // One of each size, and the bytes match the dumps they were built from.
        var nook = library.Items.Value.Single(item => item.DisplayName == "Tom Nook");
        Assert.Equal(AmiiboTagType.Ntag215, nook.TagType);
        Assert.Equal(Ntag215(), library.Bytes(nook.Id));

        var kirby = library.Items.Value.Single(item => item.DisplayName == "Kirby & Tank Star");
        Assert.Equal(AmiiboTagType.FigureV3, kirby.TagType);
        Assert.Equal(FigureV3(), library.Bytes(kirby.Id));
    }

    [Fact]
    public void TheLoadedMarkerInAnAndroidArchiveIsUnderstood()
    {
        // The fixture deliberately marks the SECOND entry, so a reader that
        // ignored the marker, or assumed the first entry, would pass the import
        // test above and fail here.
        var entries = AmiiboArchive.Read(File.ReadAllBytes(
            RepositoryFile("tools/fixtures/amiibo/library-archive-v3.zip")));

        Assert.Single(entries, entry => entry.Loaded);
        Assert.Equal("Kirby & Tank Star", entries.Single(entry => entry.Loaded).DisplayName);
    }

    [Fact]
    public void AnArchiveThisSideWritesIsShapedLikeTheOneAndroidWrites()
    {
        // Compared on STRUCTURE, not bytes: the two will differ in timestamps and
        // in zip metadata, and demanding byte equality would be a test of nothing
        // useful that broke on every unrelated change.
        var reference = File.ReadAllBytes(
            RepositoryFile("tools/fixtures/amiibo/library-archive-v3.zip"));

        var library = Library();
        library.Import("Tom Nook", "a.bin", Ntag215());
        var kirby = library.Import("Kirby & Tank Star", "b.bin", FigureV3()).Item;
        var ours = library.ExportArchive(loadedId: kirby.Id);

        Assert.Equal(Describe(reference), Describe(ours));

        // Flattened to a string rather than a tuple carrying an array: tuple
        // equality compares arrays by reference, so the array form passed nothing
        // and failed on values that were identical.
        static string Describe(byte[] zip)
        {
            using var archive = new ZipArchive(new MemoryStream(zip), ZipArchiveMode.Read);
            using var manifest = JsonDocument.Parse(ReadEntry(archive.Entries[0]));
            var entries = manifest.RootElement.GetProperty("entries").EnumerateArray().ToList();

            var names = entries
                .Select(entry => entry.GetProperty("name").GetString()!)
                .OrderBy(name => name, StringComparer.Ordinal);
            var loaded = entries
                .Single(entry => entry.GetProperty("loaded").GetBoolean())
                .GetProperty("name").GetString();

            return $"first={archive.Entries[0].FullName} " +
                   $"count={archive.Entries.Count} " +
                   $"names=[{string.Join(", ", names)}] " +
                   $"loaded={loaded}";
        }
    }

    // --------------------------------------------------------------- helpers

    private static byte[] ReadEntry(ZipArchiveEntry entry)
    {
        using var stream = entry.Open();
        using var buffer = new MemoryStream();
        stream.CopyTo(buffer);
        return buffer.ToArray();
    }

    private static byte[] BuildZip(params (string Name, byte[] Bytes)[] entries)
    {
        using var output = new MemoryStream();
        using (var zip = new ZipArchive(output, ZipArchiveMode.Create, leaveOpen: true))
        {
            foreach (var (name, bytes) in entries)
            {
                using var stream = zip.CreateEntry(name).Open();
                stream.Write(bytes, 0, bytes.Length);
            }
        }

        return output.ToArray();
    }

    private static byte[] StripManifest(byte[] archive)
    {
        using var input = new MemoryStream(archive);
        using var source = new ZipArchive(input, ZipArchiveMode.Read);
        var kept = source.Entries
            .Where(entry => entry.FullName != "library.json")
            .Select(entry => (entry.FullName, ReadEntry(entry)))
            .ToArray();
        return BuildZip(kept);
    }
}
