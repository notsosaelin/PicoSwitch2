using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The browser's query state, and the defect that made it necessary.
/// </summary>
/// <remarks>
/// THE BUG: clicking an Amiibo sent a 1000-item library back to the top.
///
/// The page called one monolithic Render from its selection handler, and Render
/// reassigned the browser's ItemsSource. WinUI discards its container generation
/// and scroll offset when ItemsSource is replaced, so every click reset the
/// scroll — and, with a 1000-entry collection, made the library unusable.
///
/// The structural fix is that a selection CANNOT influence the projection.
/// <see cref="AmiiboGalleryFilters"/> carries no selection at all, so no
/// projection can depend on one even by accident; the page then compares the
/// projected sequence against the last and leaves the control untouched when it
/// has not changed.
///
/// These tests pin the presentation-layer half of that. They are deliberately
/// about state semantics, not pixels or scroll offsets.
/// </remarks>
public sealed class AmiiboBrowserStateTests
{
    private static AmiiboLibraryItem Item(string id, string name, string figureId) => new()
    {
        Id = id,
        DisplayName = name,
        FileName = $"{id}.bin",
        Size = 540,
        Crc32 = "00000000",
        Uid = "04" + id.PadLeft(12, '0'),
        FigureId = figureId,
        Imported = DateTimeOffset.UnixEpoch.AddDays(id.Length),
    };

    private static readonly AmiiboLibraryItem[] Library =
    [
        Item("a", "Nook", "0183000002420502"),
        Item("bb", "Kirby", "1F00000004C41E03"),
        Item("ccc", "Dedede", "1F02000004C71E03"),
    ];

    private static AmiiboCatalogEntry Entry(string figureId, string name, string series) => new()
    {
        Id = figureId,
        Name = name,
        Character = name,
        GameSeries = series,
        AmiiboSeries = series,
        Type = "Figure",
        ReleaseDate = "2015-07-30",
        ImageUrl = $"https://example.invalid/{figureId}.png",
    };

    private static Func<string, AmiiboCatalogEntry?> Catalog()
    {
        var index = new Dictionary<string, AmiiboCatalogEntry>(StringComparer.OrdinalIgnoreCase)
        {
            ["0183000002420502"] = Entry("0183000002420502", "Tom Nook", "Animal Crossing"),
            ["1F00000004C41E03"] = Entry("1F00000004C41E03", "Kirby", "Kirby"),
            ["1F02000004C71E03"] = Entry("1F02000004C71E03", "King Dedede", "Kirby"),
        };
        return figureId => index.GetValueOrDefault(figureId);
    }

    private static IReadOnlyList<string> Ids(AmiiboGalleryFilters filters, string? loadedId = null) =>
        [.. AmiiboGallery.Build(Library, Catalog(), loadedId, filters).Select(card => card.Id)];

    // --------------------------------------------------- the root-cause fix

    [Fact]
    public void TheQueryRecordCannotCarryASelection()
    {
        // The structural guarantee. If a selection could be put in here, some
        // future projection could read it, and the defect could come back by
        // accident. It cannot, so it cannot.
        var properties = typeof(AmiiboGalleryFilters)
            .GetProperties()
            .Select(property => property.Name)
            .ToList();

        Assert.DoesNotContain("Selected", properties);
        Assert.DoesNotContain("SelectedId", properties);
        Assert.DoesNotContain("Selection", properties);
    }

    [Fact]
    public void ProjectingIsIndependentOfWhichItemIsSelected()
    {
        // The projection takes no selection, so selecting cannot reorder or
        // re-filter anything. This is what lets the page leave the collection
        // completely alone on a selection change.
        var filters = new AmiiboGalleryFilters { Sort = AmiiboSort.Name };
        var before = Ids(filters);

        // Whatever the page does with a selection, the query result for the
        // same filters is byte-for-byte the same sequence.
        Assert.Equal(before, Ids(filters));
        Assert.Equal(before, Ids(filters with { }));
    }

    [Fact]
    public void TheAdapterTagChangesBadgesWithoutChangingTheSequence()
    {
        // The one thing that legitimately repaints tiles without reordering
        // them. The page's rebuild signature covers the badge for exactly this
        // reason, so "now on the adapter" shows up while ordinary re-renders
        // still leave the collection alone.
        var filters = new AmiiboGalleryFilters { Sort = AmiiboSort.Name };

        Assert.Equal(Ids(filters), Ids(filters, loadedId: "bb"));

        var badges = AmiiboGallery.Build(Library, Catalog(), "bb", filters)
            .Where(card => card.Badge.Length > 0)
            .Select(card => card.Id);
        Assert.Equal(["bb"], badges);
    }

    // --------------------------------------------------- query preservation

    [Fact]
    public void ChangingViewModeDoesNotChangeTheResult()
    {
        // All three views consume ONE query result. Switching between them is a
        // change of presentation and nothing else, so it cannot reorder the
        // library, lose a filter, or move the selection to an unrelated item.
        var filters = new AmiiboGalleryFilters
        {
            Search = "kir",
            Sort = AmiiboSort.Name,
            GameSeries = "Kirby",
        };

        var grid = Ids(filters with { View = AmiiboViewMode.Grid });
        var carousel = Ids(filters with { View = AmiiboViewMode.Carousel });
        var list = Ids(filters with { View = AmiiboViewMode.List });

        Assert.Equal(grid, carousel);
        Assert.Equal(grid, list);
        Assert.NotEmpty(grid);
    }

    [Fact]
    public void ViewModeIsNotPartOfTheQueryIdentity()
    {
        // The page rebuilds its collection when the query identity changes.
        // View must not be in it, or switching view would needlessly rebuild —
        // and rebuilding is precisely what loses scroll position.
        var grid = new AmiiboGalleryFilters { View = AmiiboViewMode.Grid };
        var list = grid with { View = AmiiboViewMode.List };

        Assert.Equal(grid.QueryIdentity, list.QueryIdentity);
    }

    [Fact]
    public void EveryNarrowingOrOrderingChangeIsPartOfTheQueryIdentity()
    {
        // The other half: anything that DOES change the result must change the
        // identity, or the page would skip a rebuild it needed and show a stale
        // list.
        var baseline = new AmiiboGalleryFilters();

        Assert.NotEqual(baseline.QueryIdentity, (baseline with { Search = "x" }).QueryIdentity);
        Assert.NotEqual(baseline.QueryIdentity, (baseline with { GameSeries = "Kirby" }).QueryIdentity);
        Assert.NotEqual(baseline.QueryIdentity, (baseline with { AmiiboSeries = "Kirby" }).QueryIdentity);
        Assert.NotEqual(baseline.QueryIdentity, (baseline with { Type = "Figure" }).QueryIdentity);
        Assert.NotEqual(baseline.QueryIdentity, (baseline with { Sort = AmiiboSort.Name }).QueryIdentity);
        Assert.NotEqual(baseline.QueryIdentity, (baseline with { Descending = true }).QueryIdentity);
    }

    [Fact]
    public void ClearingFiltersKeepsSortAndView()
    {
        // Sort and view are preferences, not narrowing. "Clear filters" that
        // silently reset them would be an unasked-for change, so Any excludes
        // both and the page's Clear carries them forward.
        var filters = new AmiiboGalleryFilters
        {
            Search = "nook",
            GameSeries = "Animal Crossing",
            Sort = AmiiboSort.Release,
            Descending = true,
            View = AmiiboViewMode.List,
        };

        Assert.True(filters.Any);

        var cleared = new AmiiboGalleryFilters
        {
            Sort = filters.Sort,
            Descending = filters.Descending,
            View = filters.View,
        };

        Assert.False(cleared.Any);
        Assert.Equal(AmiiboSort.Release, cleared.Sort);
        Assert.True(cleared.Descending);
        Assert.Equal(AmiiboViewMode.List, cleared.View);
    }

    [Fact]
    public void ASelectedItemSurvivesAQueryThatStillIncludesIt()
    {
        // Selection is held by id, so narrowing the library keeps the selection
        // as long as the item is still in the result.
        var narrowed = Ids(new AmiiboGalleryFilters { GameSeries = "Kirby" });
        Assert.Contains("bb", narrowed);
    }

    [Fact]
    public void ASelectedItemFilteredAwayIsSimplyNotInTheResult()
    {
        // The page resolves its selection against the result and finds nothing,
        // which empties the inspection pane rather than selecting something the
        // user did not pick.
        var narrowed = Ids(new AmiiboGalleryFilters { GameSeries = "Animal Crossing" });
        Assert.DoesNotContain("bb", narrowed);
    }

    // ------------------------------------------------------- list view data

    [Fact]
    public void EveryCardCarriesTheDetailedListColumns()
    {
        // The list view reads the SAME cards as the grid rather than a second
        // projection -- two projections is how "one shared query state" quietly
        // stops being true.
        var card = AmiiboGallery
            .Build(Library, Catalog(), null, new AmiiboGalleryFilters { Sort = AmiiboSort.Name })
            .First(candidate => candidate.Id == "a");

        Assert.Equal("Animal Crossing", card.GameSeries);
        Assert.Equal("Animal Crossing", card.AmiiboSeries);
        Assert.Equal("2015-07-30", card.ReleaseDate);
        Assert.Equal("0183000002420502", card.FigureId);
    }

    [Fact]
    public void ColumnsAreEmptyRatherThanInventedWithNoCatalog()
    {
        var card = Assert.Single(
            AmiiboGallery.Build([Library[0]], _ => null, null, new AmiiboGalleryFilters()));

        Assert.Equal("", card.GameSeries);
        Assert.Equal("", card.ReleaseDate);
        // The figure id comes from the tag itself, so it is always known.
        Assert.Equal("0183000002420502", card.FigureId);
    }

    [Fact]
    public void ArtworkFailureIsRepresentedAsSimplyHavingNone()
    {
        // A catalog that answers without an image, which is what an unknown
        // figure or a failed fetch looks like from here. HasArtwork drives the
        // placeholder, so the tile still renders and nothing else is affected.
        var entry = Entry("0183000002420502", "Tom Nook", "Animal Crossing") with { ImageUrl = "" };
        var card = Assert.Single(
            AmiiboGallery.Build([Library[0]], _ => entry, null, new AmiiboGalleryFilters()));

        Assert.False(card.HasArtwork);
        Assert.Equal("Tom Nook", card.Title);
        Assert.NotEqual("", card.Subtitle);
    }
}
