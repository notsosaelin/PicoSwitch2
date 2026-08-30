using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The LOCAL profile library.
/// </summary>
/// <remarks>
/// THE CORRECTION THESE TESTS PIN. The adapter holds six resident profiles —
/// three positions in each of two layout banks — because those have to work with
/// no companion attached. That is the adapter's working set, NOT the user's
/// capacity. Treating the two as one thing made Save mean "write to the adapter",
/// which made every edit a flash erase and capped the user at six.
///
/// The library has no management client and cannot acquire one, so "zero adapter
/// writes while editing" is structural rather than a convention.
/// </remarks>
public sealed class KbmLibraryRepositoryTests : IDisposable
{
    private readonly string root =
        Path.Combine(Path.GetTempPath(), "picoswitch-kbm-" + Guid.NewGuid().ToString("N"));

    private KbmLibraryRepository Open() =>
        new(new KbmProfileLibraryStore(new WindowsDocumentStore(root)));

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
            // A temp directory that will not delete is not a test failure.
        }
    }

    private static KbmBinding Bind(int usage, KbmDestination destination) =>
        new(new KbmSource(KbmSourceKind.Key, usage), destination, Custom: true);

    /* ------------------------------------------------- the six-profile myth */

    [Fact]
    public void TheLibraryIsNotCappedAtTheAdaptersSixResidentProfiles()
    {
        // THE POINT. Six is how many the ADAPTER holds; the user may keep as
        // many as they like. Twenty-five is well past both the six records and
        // the three-per-layout bank limit, and none of that applies here.
        var repository = Open();
        for (var i = 0; i < 25; i++)
        {
            var layout = i % 2 == 0 ? KbmLayout.Keyboard : KbmLayout.KeyboardMouse;
            repository.Create(layout, $"Profile {i}");
        }

        Assert.Equal(25, repository.Value.Profiles.Count);
        Assert.Equal(13, repository.Value.For(KbmLayout.Keyboard).Count);
        Assert.Equal(12, repository.Value.For(KbmLayout.KeyboardMouse).Count);

        // And it survives a restart, because a library that vanishes is not a
        // library.
        Assert.Equal(25, Open().Value.Profiles.Count);
    }

    /* ------------------------------------------------------------ identity */

    [Fact]
    public void RenamingDoesNotChangeIdentityOrBehaviour()
    {
        var repository = Open();
        var created = repository.Create(KbmLayout.Keyboard, "Halo",
                                        [Bind(0x04, KbmDestination.Zr)]);

        var renamed = repository.Rename(created.Id, "Halo Infinite");

        Assert.NotNull(renamed);
        Assert.Equal(created.Id, renamed!.Id);
        // A rename changes no behaviour, so an adapter copy that matched before
        // still matches.
        Assert.Equal(created.Fingerprint, renamed.Fingerprint);
    }

    [Fact]
    public void DeletingAndRecreatingDoesNotAliasTheOldIdentity()
    {
        // A cached draft or an adapter assignment that referred to the old
        // profile must not silently rebind to an unrelated new one.
        var repository = Open();
        var first = repository.Create(KbmLayout.Keyboard, "Halo");
        Assert.True(repository.Delete(first.Id));

        var second = repository.Create(KbmLayout.Keyboard, "Halo");

        Assert.NotEqual(first.Id, second.Id);
        Assert.Null(repository.Value.Find(first.Id));
    }

    [Fact]
    public void DuplicateCopiesContentUnderANewIdentity()
    {
        var repository = Open();
        var source = repository.Create(KbmLayout.Keyboard, "Halo",
                                       [Bind(0x04, KbmDestination.Zr)]);

        var copy = repository.Duplicate(source.Id, "Halo Copy");

        Assert.NotEqual(source.Id, copy.Id);
        // Same behaviour, so the same fingerprint: two profiles that behave
        // identically must be recognisably identical to the adapter.
        Assert.Equal(source.Fingerprint, copy.Fingerprint);

        // And editing the copy cannot reach the original.
        repository.Save(copy.Id, copy.Name, [Bind(0x04, KbmDestination.A)],
                        copy.Mouse);
        Assert.Equal(source.Fingerprint,
                     repository.Value.Find(source.Id)!.Fingerprint);
    }

    /* ------------------------------------------------------------- content */

    [Fact]
    public void SavingMovesTheFingerprintAndPuttingItBackRestoresIt()
    {
        // The comparison the whole divergence model rests on.
        var repository = Open();
        var created = repository.Create(KbmLayout.Keyboard, "Halo",
                                        [Bind(0x04, KbmDestination.Zr)]);

        var edited = repository.Save(created.Id, created.Name,
                                     [Bind(0x04, KbmDestination.A)],
                                     created.Mouse);
        Assert.NotEqual(created.Fingerprint, edited.Fingerprint);

        var restored = repository.Save(created.Id, created.Name,
                                       [Bind(0x04, KbmDestination.Zr)],
                                       created.Mouse);
        Assert.Equal(created.Fingerprint, restored.Fingerprint);
    }

    [Fact]
    public void ProfileOwnedMouseTuningIsPartOfTheContent()
    {
        // Switching profiles switches tuning too, so a tuning change must make
        // the adapter's copy out of date exactly as a rebind does.
        var repository = Open();
        var created = repository.Create(KbmLayout.KeyboardMouse, "Metroid");

        var tuned = repository.Save(created.Id, created.Name, created.Bindings,
                                    created.Mouse with { SensitivityX = 900 });

        Assert.NotEqual(created.Fingerprint, tuned.Fingerprint);
    }

    [Fact]
    public void OnlyUserOverridesAreStored()
    {
        // The adapter stores content sparsely against its canonical table. A
        // binding the user never changed is not part of the profile, and keeping
        // it would make two identical mappings fingerprint differently.
        var repository = Open();
        var created = repository.Create(
            KbmLayout.Keyboard, "Halo",
            [
                Bind(0x04, KbmDestination.Zr),
                new KbmBinding(new KbmSource(KbmSourceKind.Key, 0x16),
                               KbmDestination.LStickDown, Custom: false),
            ]);

        Assert.Single(created.Bindings);
        Assert.Equal(0x04, created.Bindings[0].Source.Code);
    }

    /* -------------------------------------------------------------- import */

    [Fact]
    public void ImportingTheSameResidentProfileTwiceDoesNotDuplicateIt()
    {
        // The cross-platform bridge: an Android-created profile reaches Windows
        // only as an adapter resident copy. Local ids are not shared, so the
        // match is by CONTENT — and without that, every reconnect would add
        // another copy.
        var repository = Open();
        IReadOnlyList<KbmBinding> bindings = [Bind(0x04, KbmDestination.Zr)];
        var mouse = new KbmMouseConfig(SensitivityX: 512);

        var first = repository.Import(KbmLayout.Keyboard, "Halo", bindings, mouse);
        var second = repository.Import(KbmLayout.Keyboard, "Halo", bindings, mouse);

        Assert.Equal(first.Id, second.Id);
        Assert.Single(repository.Value.Profiles);
    }

    [Fact]
    public void ImportingDifferentContentUnderTheSameNameKeepsBoth()
    {
        // Same name, different behaviour: these are two profiles, and silently
        // merging them would lose one.
        var repository = Open();
        var mouse = new KbmMouseConfig(SensitivityX: 512);

        var first = repository.Import(KbmLayout.Keyboard, "Halo",
                                      [Bind(0x04, KbmDestination.Zr)], mouse);
        var second = repository.Import(KbmLayout.Keyboard, "Halo",
                                       [Bind(0x04, KbmDestination.A)], mouse);

        Assert.NotEqual(first.Id, second.Id);
        Assert.Equal(2, repository.Value.Profiles.Count);
        // The second gets a distinguishable name rather than a duplicate one.
        Assert.NotEqual(first.Name, second.Name);
    }

    [Fact]
    public void TheSameContentInTheOtherLayoutIsADifferentProfile()
    {
        var repository = Open();
        IReadOnlyList<KbmBinding> bindings = [Bind(0x04, KbmDestination.Zr)];
        var mouse = new KbmMouseConfig();

        var keyboard = repository.Import(KbmLayout.Keyboard, "Halo", bindings, mouse);
        var combined = repository.Import(KbmLayout.KeyboardMouse, "Halo", bindings,
                                         mouse);

        Assert.NotEqual(keyboard.Id, combined.Id);
        Assert.NotEqual(keyboard.Fingerprint, combined.Fingerprint);
    }

    /* ----------------------------------------------------------- durability */

    [Fact]
    public void AMalformedDocumentCostsTheLibraryAndNotTheLaunch()
    {
        var repository = Open();
        repository.Create(KbmLayout.Keyboard, "Halo");

        var path = Path.Combine(root, KbmProfileLibraryStore.DocumentName);
        File.WriteAllText(path, "{ this is not json");

        // Empty, not an exception. Decoding totally is the rule every document
        // here follows: an unreadable file must cost its contents, never the
        // user's ability to start the app.
        Assert.Empty(Open().Value.Profiles);
    }

    [Fact]
    public void AProfileRowThisBuildCannotReadIsSkippedRatherThanRepaired()
    {
        // A profile with an unreadable binding would be offered to the user as
        // theirs and then behave in a way they never configured. Losing one row
        // is better than keeping a wrong one.
        var repository = Open();
        repository.Create(KbmLayout.Keyboard, "Good", [Bind(0x04, KbmDestination.Zr)]);

        var path = Path.Combine(root, KbmProfileLibraryStore.DocumentName);
        var text = File.ReadAllText(path).Replace("\"dst\":\"zr\"",
                                                  "\"dst\":\"from_the_future\"");
        File.WriteAllText(path, text);

        Assert.Empty(Open().Value.Profiles);
    }

    [Fact]
    public void ADocumentFromANewerSchemaIsNotMisread()
    {
        var repository = Open();
        repository.Create(KbmLayout.Keyboard, "Halo");

        var path = Path.Combine(root, KbmProfileLibraryStore.DocumentName);
        File.WriteAllText(path,
                          File.ReadAllText(path).Replace("\"schema\":1",
                                                         "\"schema\":99"));

        // Refusing is the honest answer: a misparse would silently rewrite the
        // user's library in the old shape on the next save.
        Assert.Empty(Open().Value.Profiles);
    }

    [Fact]
    public void EveryOperationRoundTripsThroughDisk()
    {
        var repository = Open();
        var halo = repository.Create(KbmLayout.Keyboard, "Halo",
                                     [Bind(0x04, KbmDestination.Zr)]);
        repository.Create(KbmLayout.KeyboardMouse, "Metroid");
        repository.Rename(halo.Id, "Halo Infinite");
        var doomed = repository.Create(KbmLayout.Keyboard, "Doomed");
        repository.Delete(doomed.Id);

        var reopened = Open().Value;

        Assert.Equal(2, reopened.Profiles.Count);
        Assert.Equal("Halo Infinite", reopened.Find(halo.Id)!.Name);
        Assert.Null(reopened.Find(doomed.Id));
        Assert.Single(reopened.For(KbmLayout.KeyboardMouse));
    }
}
