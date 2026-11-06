using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The inspection pane's grouping and omission rules.
/// </summary>
/// <remarks>
/// The same facts were previously in one flat key/value column, which made the
/// pane read like a database row rather than a description of a figure. What is
/// tested here is the hierarchy — and, more importantly, what is left OUT: a tag
/// that was never set up has no owner, no nickname and no dates, and printing
/// five empty lines implies information exists and is missing when the tag is
/// simply blank.
/// </remarks>
public sealed class AmiiboInspectionTests
{
    private static AmiiboLibraryItem Item(bool v3 = false) => new()
    {
        Id = "a",
        DisplayName = "My Nook",
        FileName = "a.bin",
        Size = v3 ? 2048 : 540,
        Crc32 = "DEADBEEF",
        Uid = "04676FFAE04981",
        FigureId = "0183000002420502",
        Imported = DateTimeOffset.UnixEpoch,
        TagType = v3 ? AmiiboTagType.FigureV3 : AmiiboTagType.Ntag215,
    };

    private static AmiiboCatalogEntry Entry() => new()
    {
        Id = "0183000002420502",
        Name = "Tom Nook",
        Character = "Tom Nook",
        GameSeries = "Animal Crossing",
        AmiiboSeries = "Animal Crossing",
        Type = "Figure",
        ReleaseDate = "2015-07-30",
        TitleIds = new ValueList<KeyValuePair<string, string>>(
        [
            new KeyValuePair<string, string>("0100F95004ECC000", "Animal Crossing: New Horizons"),
        ]),
    };

    private static AmiiboDetails Decoded(
        bool setUp = false,
        bool appData = false,
        AmiiboCryptoState crypto = AmiiboCryptoState.Valid) => new()
    {
        Identity = new AmiiboIdentity
        {
            Uid = "04676FFAE04981",
            FigureId = "0183000002420502",
            TagType = AmiiboTagType.Ntag215,
            CharacterGameCode = "0183",
            CharacterVariant = 0,
            TypeName = "Figure",
            ModelNumber = "0242",
            SeriesCode = 5,
            FormatVersion = 2,
            ExtendedVariant = "0D129507",
        },
        Size = 540,
        Crc32 = "DEADBEEF",
        Crypto = crypto,
        SetUp = setUp,
        Nickname = setUp ? "Nooky" : "",
        Owner = setUp ? "Sam" : "",
        SetupDate = setUp ? "2016-01-02" : null,
        LastWriteDate = setUp ? "2016-03-04" : null,
        WriteCounter = setUp ? 7 : 0,
        HasAppData = appData,
        TitleId = appData ? "0100F95004ECC000" : "",
        AppId = appData ? "42C40700" : "",
        AppDataLabel = appData ? "Unrecognised game" : "None",
    };

    private static IReadOnlyList<AmiiboDetailGroup> Build(
        AmiiboDetails? decoded, bool onAdapter = false, bool changed = false) =>
        AmiiboInspection.Build(Item(), Entry(), decoded, onAdapter, changed);

    private static AmiiboDetailGroup Group(
        IReadOnlyList<AmiiboDetailGroup> groups, string title) =>
        groups.Single(group => group.Title == title);

    private static string? Value(IReadOnlyList<AmiiboDetailGroup> groups, string label) =>
        groups.SelectMany(group => group.Rows)
            .FirstOrDefault(row => row.Label == label)?.Value;

    [Fact]
    public void IdentityAndTagAreAlwaysPresent()
    {
        var groups = Build(decoded: null);

        Assert.Contains(groups, group => group.Title == "Identity");
        Assert.Contains(groups, group => group.Title == "Tag");
        Assert.Equal("My Nook", Value(groups, "Your name"));
        Assert.Equal("Tom Nook", Value(groups, "Figure"));
        Assert.Equal("Animal Crossing", Value(groups, "Game series"));
        Assert.Equal("NTAG215", Value(groups, "Tag type"));
    }

    [Fact]
    public void TheCatalogNameIsOmittedWhenItRepeatsTheUsersOwn()
    {
        // Two identical lines say less than one.
        var item = Item() with { DisplayName = "Tom Nook" };
        var groups = AmiiboInspection.Build(item, Entry(), null, false, false);

        Assert.Equal("Tom Nook", Value(groups, "Your name"));
        Assert.Null(Value(groups, "Figure"));
    }

    [Fact]
    public void IdentifiersAreMarkedForMonospace()
    {
        // UID and figure id get compared character by character; proportional
        // digits make that harder.
        var groups = Build(decoded: null);
        var rows = groups.SelectMany(group => group.Rows).ToList();

        Assert.True(rows.Single(row => row.Label == "Figure ID").Monospace);
        Assert.True(rows.Single(row => row.Label == "UID").Monospace);
        Assert.False(rows.Single(row => row.Label == "Your name").Monospace);
    }

    [Fact]
    public void RegistrationIsOmittedEntirelyForABlankTag()
    {
        // THE OMISSION THAT MATTERS. A never-set-up tag has no owner and no
        // dates; five "—" rows would imply the data exists and is missing.
        var groups = Build(Decoded(setUp: false));

        Assert.DoesNotContain(groups, group => group.Title == "Registration");
        Assert.Equal("Blank, never set up", Value(groups, "Contents"));
    }

    [Fact]
    public void RegistrationAppearsForASetUpTag()
    {
        var groups = Build(Decoded(setUp: true));
        var registration = Group(groups, "Registration");

        Assert.Equal("Nooky", Value(groups, "Nickname"));
        Assert.Equal("Sam", Value(groups, "Owner"));
        Assert.Equal("2016-01-02", Value(groups, "Registered"));
        Assert.Equal("2016-03-04", Value(groups, "Last written"));
        Assert.Equal("7", Value(groups, "Times written"));
        Assert.True(registration.Any);
    }

    [Fact]
    public void RegistrationIsOmittedWithoutKeysRatherThanShownEmpty()
    {
        // No keys is not the same as a blank tag, and the Contents line says
        // which -- but inventing empty owner rows would be wrong either way.
        var groups = Build(Decoded(crypto: AmiiboCryptoState.KeyUnavailable));

        Assert.DoesNotContain(groups, group => group.Title == "Registration");
        Assert.Equal("Import your Amiibo keys to read this", Value(groups, "Contents"));
    }

    [Fact]
    public void UndecryptableContentsAreDistinguishedFromMissingKeys()
    {
        // Different problems: one is "import a key", the other is "this file or
        // these keys are wrong".
        var groups = Build(Decoded(crypto: AmiiboCryptoState.Invalid));
        Assert.Equal(
            "Could not be decrypted with the imported keys", Value(groups, "Contents"));
    }

    [Fact]
    public void GameDataNamesTheGameFromTheCatalogWhenItCan()
    {
        // The catalog knows titles the firmware's small built-in table does not.
        var groups = Build(Decoded(setUp: true, appData: true));

        Assert.Equal("Animal Crossing: New Horizons", Value(groups, "Game"));
        Assert.Equal("0100F95004ECC000", Value(groups, "Title ID"));
    }

    [Fact]
    public void GameDataSaysNoneRatherThanVanishing()
    {
        // "This tag has no game data" is a useful answer; an absent group would
        // leave the user wondering whether it was checked.
        var groups = Build(Decoded(setUp: true));
        Assert.Equal("None", Value(groups, "Game"));
    }

    [Fact]
    public void TheAdapterGroupDistinguishesThreeStates()
    {
        Assert.Equal("Not on the adapter", Value(Build(null), "Status"));
        Assert.Equal("On the adapter", Value(Build(null, onAdapter: true), "Status"));
        Assert.Equal(
            "On the adapter, changed by the console",
            Value(Build(null, onAdapter: true, changed: true), "Status"));
    }

    [Fact]
    public void EveryReturnedGroupHasRows()
    {
        // An empty group is a heading with nothing under it, which reads as a
        // rendering bug.
        foreach (var decoded in new AmiiboDetails?[]
                 {
                     null, Decoded(), Decoded(setUp: true), Decoded(setUp: true, appData: true),
                 })
        {
            Assert.All(Build(decoded), group => Assert.True(group.Any));
        }
    }

    [Fact]
    public void AV3TagReportsItsOwnFamily()
    {
        var groups = AmiiboInspection.Build(Item(v3: true), Entry(), null, false, false);
        Assert.Equal("Figure v3", Value(groups, "Tag type"));
        Assert.Equal("2048 bytes", Value(groups, "Size"));
    }
}
