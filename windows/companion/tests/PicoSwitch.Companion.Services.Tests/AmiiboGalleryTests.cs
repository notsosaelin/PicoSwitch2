using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The library as a browsable collection: searched, filtered, sorted.
/// </summary>
/// <remarks>
/// A flat list of names is the wrong shape for amiibo. People think in series
/// and characters and recognise figures by artwork long before they read an id,
/// so the rules that matter are the ones that let somebody FIND a tag. All of
/// them are pure, and the catalog arrives as a lookup function, so none of this
/// needs a page or a network.
/// </remarks>
public sealed class AmiiboGalleryTests
{
    private static AmiiboLibraryItem Item(
        string id,
        string name,
        string figureId = "0183000002420502",
        int importedDaysAgo = 0,
        bool dirty = false) => new()
    {
        Id = id,
        DisplayName = name,
        FileName = $"{id}.bin",
        Size = 540,
        Crc32 = "00000000",
        Uid = "04" + id.PadLeft(12, '0'),
        FigureId = figureId,
        Imported = DateTimeOffset.UnixEpoch.AddDays(100 - importedDaysAgo),
        DirtyFromAdapter = dirty,
    };

    private static AmiiboCatalogEntry Entry(
        string figureId,
        string name,
        string gameSeries = "Animal Crossing",
        string amiiboSeries = "Animal Crossing",
        string type = "Figure",
        string release = "2015-07-30",
        string image = "https://example.invalid/a.png") => new()
    {
        Id = figureId,
        Name = name,
        Character = name,
        GameSeries = gameSeries,
        AmiiboSeries = amiiboSeries,
        Type = type,
        ReleaseDate = release,
        ImageUrl = image,
    };

    private static Func<string, AmiiboCatalogEntry?> Catalog(
        params AmiiboCatalogEntry[] entries)
    {
        var index = entries.ToDictionary(entry => entry.Id, StringComparer.OrdinalIgnoreCase);
        return figureId => index.GetValueOrDefault(figureId);
    }

    private static Func<string, AmiiboCatalogEntry?> NoCatalog() => _ => null;

    // ------------------------------------------------------------------ cards

    [Fact]
    public void TheCatalogNameLeadsAndTheUsersOwnNameIsKept()
    {
        // "Tom Nook" is what the figure IS. The user's own name is not
        // discarded, though -- they may well have called it "Nook for trading".
        var cards = AmiiboGallery.Build(
            [Item("a", "Nook for trading")],
            Catalog(Entry("0183000002420502", "Tom Nook")),
            loadedId: null,
            new AmiiboGalleryFilters());

        var card = Assert.Single(cards);
        Assert.Equal("Tom Nook", card.Title);
        Assert.Equal("Nook for trading", card.OwnName);
        Assert.Contains("Animal Crossing", card.Subtitle);
        Assert.True(card.HasArtwork);
    }

    [Fact]
    public void WithNoCatalogTheTagStillDescribesItself()
    {
        // Offline, or a figure the catalog does not know. Showing the user's own
        // name and what the tag says beats an empty tile.
        var cards = AmiiboGallery.Build(
            [Item("a", "My backup")],
            NoCatalog(),
            loadedId: null,
            new AmiiboGalleryFilters());

        var card = Assert.Single(cards);
        Assert.Equal("My backup", card.Title);
        Assert.Contains("NTAG215", card.Subtitle);
        Assert.Contains("0183000002420502", card.Subtitle);
        Assert.False(card.HasArtwork);
        // No duplicate name when the catalog had nothing to add.
        Assert.Equal("", card.OwnName);
    }

    [Fact]
    public void BadgesSayWhatIsOnTheAdapterAndWhatChanged()
    {
        var loaded = AmiiboGallery.Build(
            [Item("a", "Nook")], NoCatalog(), "a", new AmiiboGalleryFilters());
        Assert.Equal("On adapter", loaded[0].Badge);

        // Unsynced console writes outrank simply being loaded: it is the fact
        // the user has to act on.
        var changed = AmiiboGallery.Build(
            [Item("a", "Nook", dirty: true)], NoCatalog(), "a", new AmiiboGalleryFilters());
        Assert.Equal("Changed", changed[0].Badge);

        var plain = AmiiboGallery.Build(
            [Item("a", "Nook")], NoCatalog(), null, new AmiiboGalleryFilters());
        Assert.Equal("", plain[0].Badge);
    }

    // ----------------------------------------------------------------- search

    [Fact]
    public void SearchMatchesAnythingAPersonMightRemember()
    {
        var library = new[] { Item("a", "My backup") };
        var catalog = Catalog(Entry("0183000002420502", "Tom Nook", "Animal Crossing"));

        foreach (var needle in new[]
                 {
                     "My backup", "Tom", "nook", "animal", "0183", "CROSSING",
                 })
        {
            var cards = AmiiboGallery.Build(
                library, catalog, null, new AmiiboGalleryFilters { Search = needle });
            Assert.True(cards.Count == 1, $"'{needle}' should have matched");
        }

        Assert.Empty(AmiiboGallery.Build(
            library, catalog, null, new AmiiboGalleryFilters { Search = "Kirby" }));
    }

    [Fact]
    public void SearchIgnoresSurroundingSpace()
    {
        var cards = AmiiboGallery.Build(
            [Item("a", "Tom Nook")], NoCatalog(), null,
            new AmiiboGalleryFilters { Search = "  nook  " });

        Assert.Single(cards);
    }

    // ---------------------------------------------------------------- filters

    [Fact]
    public void FiltersNarrowBySeriesAndType()
    {
        var library = new[]
        {
            Item("a", "Nook", "0183000002420502"),
            Item("b", "Kirby", "1F00000004C41E03"),
        };
        var catalog = Catalog(
            Entry("0183000002420502", "Tom Nook", "Animal Crossing", "Animal Crossing", "Figure"),
            Entry("1F00000004C41E03", "Kirby", "Kirby", "Air Riders", "Figure"));

        var byGame = AmiiboGallery.Build(
            library, catalog, null, new AmiiboGalleryFilters { GameSeries = "Kirby" });
        Assert.Equal("Kirby", Assert.Single(byGame).Title);

        var bySeries = AmiiboGallery.Build(
            library, catalog, null, new AmiiboGalleryFilters { AmiiboSeries = "Air Riders" });
        Assert.Equal("Kirby", Assert.Single(bySeries).Title);

        // A type both share narrows nothing, which is correct.
        var byType = AmiiboGallery.Build(
            library, catalog, null, new AmiiboGalleryFilters { Type = "Figure" });
        Assert.Equal(2, byType.Count);
    }

    [Fact]
    public void AFilteredFieldTheCatalogCannotAnswerExcludesTheTag()
    {
        // An unknown figure has no series, so filtering BY series must not
        // silently include it -- the user asked for that series specifically.
        var cards = AmiiboGallery.Build(
            [Item("a", "Mystery")], NoCatalog(), null,
            new AmiiboGalleryFilters { GameSeries = "Animal Crossing" });

        Assert.Empty(cards);
    }

    [Fact]
    public void FiltersCombine()
    {
        var library = new[]
        {
            Item("a", "Nook", "0183000002420502"),
            Item("b", "Kirby", "1F00000004C41E03"),
        };
        var catalog = Catalog(
            Entry("0183000002420502", "Tom Nook", "Animal Crossing"),
            Entry("1F00000004C41E03", "Kirby", "Kirby"));

        var cards = AmiiboGallery.Build(
            library, catalog, null,
            new AmiiboGalleryFilters { GameSeries = "Animal Crossing", Search = "Kirby" });

        Assert.Empty(cards);
    }

    [Fact]
    public void FilterOptionsComeFromWhatTheUserOwns()
    {
        // A dropdown listing every amiibo series in existence when the library
        // holds two figures is a worse control than one listing those two's.
        var options = AmiiboGallery.Options(
            [Item("a", "Nook", "0183000002420502"), Item("b", "Kirby", "1F00000004C41E03")],
            Catalog(
                Entry("0183000002420502", "Tom Nook", "Animal Crossing", "Animal Crossing"),
                Entry("1F00000004C41E03", "Kirby", "Kirby", "Air Riders")));

        Assert.Equal(["Animal Crossing", "Kirby"], options.GameSeries);
        Assert.Equal(["Air Riders", "Animal Crossing"], options.AmiiboSeries);
        Assert.Equal(["Figure"], options.Types);
    }

    [Fact]
    public void FilterOptionsAreEmptyWithNoCatalog()
    {
        var options = AmiiboGallery.Options([Item("a", "Nook")], NoCatalog());
        Assert.Empty(options.GameSeries);
        Assert.Empty(options.AmiiboSeries);
        Assert.Empty(options.Types);
    }

    // ---------------------------------------------------------------- sorting

    [Fact]
    public void TheDefaultOrderIsMostRecentlyAdded()
    {
        // Where someone looks for the tag they just imported.
        var cards = AmiiboGallery.Build(
            [
                Item("old", "Old", importedDaysAgo: 10),
                Item("new", "New", importedDaysAgo: 0),
            ],
            NoCatalog(), null, new AmiiboGalleryFilters());

        Assert.Equal(["New", "Old"], cards.Select(card => card.Title));
    }

    [Fact]
    public void SortByNameUsesTheDisplayedName()
    {
        // The CATALOG name, because that is the one on the tile. Sorting by the
        // user's own name would order the grid by something invisible.
        var cards = AmiiboGallery.Build(
            [Item("a", "Zzz backup"), Item("b", "Aaa backup", "1F00000004C41E03")],
            Catalog(
                Entry("0183000002420502", "Alpha"),
                Entry("1F00000004C41E03", "Omega")),
            null,
            new AmiiboGalleryFilters { Sort = AmiiboSort.Name });

        Assert.Equal(["Alpha", "Omega"], cards.Select(card => card.Title));
    }

    [Fact]
    public void SortByNumberGroupsASeriesTheWayAShelfDoes()
    {
        var cards = AmiiboGallery.Build(
            [
                Item("a", "Kirby", "1F00000004C41E03"),
                Item("b", "Nook", "0183000002420502"),
            ],
            NoCatalog(), null,
            new AmiiboGalleryFilters { Sort = AmiiboSort.Number });

        Assert.Equal(["Nook", "Kirby"], cards.Select(card => card.Title));
    }

    [Fact]
    public void SortByReleasePutsUndatedFiguresLast()
    {
        // An empty date would otherwise sort before every real one and put the
        // unknowns on top, which is the opposite of useful.
        var cards = AmiiboGallery.Build(
            [
                Item("a", "Known", "0183000002420502"),
                Item("b", "Unknown", "1F00000004C41E03"),
            ],
            Catalog(Entry("0183000002420502", "Known", release: "2015-07-30")),
            null,
            new AmiiboGalleryFilters { Sort = AmiiboSort.Release });

        Assert.Equal(["Known", "Unknown"], cards.Select(card => card.Title));
    }

    [Fact]
    public void ReversingAppliesToEverySortMode()
    {
        var library = new[]
        {
            Item("a", "Alpha", "0183000002420502"),
            Item("b", "Omega", "1F00000004C41E03"),
        };

        foreach (var sort in Enum.GetValues<AmiiboSort>())
        {
            var forward = AmiiboGallery.Build(
                library, NoCatalog(), null, new AmiiboGalleryFilters { Sort = sort });
            var reversed = AmiiboGallery.Build(
                library, NoCatalog(), null,
                new AmiiboGalleryFilters { Sort = sort, Descending = true });

            Assert.Equal(
                forward.Select(card => card.Title).Reverse(),
                reversed.Select(card => card.Title));
        }
    }

    [Fact]
    public void AnEmptyLibraryProducesNoCardsRatherThanThrowing()
    {
        Assert.Empty(AmiiboGallery.Build([], NoCatalog(), null, new AmiiboGalleryFilters()));
        var options = AmiiboGallery.Options([], NoCatalog());
        Assert.Empty(options.GameSeries);
    }

    [Fact]
    public void AnyFilterIsReportedSoTheClearButtonKnowsWhenItMatters()
    {
        Assert.False(new AmiiboGalleryFilters().Any);
        Assert.False(new AmiiboGalleryFilters { Sort = AmiiboSort.Name }.Any);
        Assert.True(new AmiiboGalleryFilters { Search = "x" }.Any);
        Assert.True(new AmiiboGalleryFilters { GameSeries = "Kirby" }.Any);
    }
}
