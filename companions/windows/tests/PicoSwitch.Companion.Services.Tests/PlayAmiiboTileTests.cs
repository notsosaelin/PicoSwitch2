using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>Play's Amiibo tile (companion-shell-layout.md, "Play"), as Android's AmiiboTileCard.</summary>
public sealed class PlayAmiiboTileTests
{
    private const string Figure = "0183000002420502";

    private static AmiiboView View(AmiiboStatus status, bool connected = true, params AmiiboLibraryItem[] library) =>
        AmiiboView.From(
            new AdapterSnapshot { Amiibo = status, Capabilities = new AdapterCapabilities(Amiibo: CapabilityState.Available) },
            connected,
            new ValueList<AmiiboLibraryItem>(library),
            keysAvailable: true,
            selectedId: null);

    private static AmiiboLibraryItem Item(string id, string uid) => new()
    {
        Id = id,
        DisplayName = "My Nook",
        FileName = $"{id}.bin",
        Size = 540,
        Crc32 = "00000000",
        Uid = uid,
        FigureId = Figure,
        Imported = DateTimeOffset.UnixEpoch,
    };

    private static readonly AmiiboCatalogEntry Nook = new()
    {
        Id = Figure,
        Name = "Tom Nook",
        GameSeries = "Animal Crossing",
        AmiiboSeries = "Animal Crossing",
        ImageUrl = "https://example.invalid/nook.png",
    };

    private static AmiiboCatalogEntry? Catalog(string id) => id == Figure ? Nook : null;

    private static AmiiboStatus Loaded(string uid = "04AA", bool presented = false) =>
        new() { Loaded = true, Uid = uid, FigureId = Figure, Presented = presented };

    [Fact]
    public void ALoadedFigureShowsItsCatalogNameArtAndPresent()
    {
        var tile = PlayAmiiboTile.Of(View(Loaded(), library: Item("a", "04AA")), Catalog, new ConsoleView(), pro2: true);
        Assert.True(tile.Resident);
        Assert.Equal("Tom Nook", tile.Title);
        Assert.Equal("Animal Crossing · Animal Crossing", tile.Subtitle);
        Assert.Equal(Nook.ImageUrl, tile.ImageUrl);
        Assert.Equal(PlayAmiiboAction.Present, tile.Action);
        Assert.True(tile.ActionEnabled);
        Assert.True(tile.ActionPrimary);
        Assert.Equal("a", tile.LibraryTarget);
    }

    [Fact]
    public void PresentedBecomesEjectAndNotProTwoCannotPresent()
    {
        Assert.Equal(PlayAmiiboAction.Eject, PlayAmiiboTile.Of(View(Loaded(presented: true)), Catalog, new(), pro2: true).Action);
        Assert.False(PlayAmiiboTile.Of(View(Loaded()), Catalog, new(), pro2: false).ActionEnabled);
    }

    [Fact]
    public void TheLibraryOpensOnTheSameFigureWhenThisExactTagIsNotSaved()
    {
        var tile = PlayAmiiboTile.Of(View(Loaded(uid: "04FF"), library: Item("other-dump", "04AA")), Catalog, new(), pro2: true);
        Assert.Equal("other-dump", tile.LibraryTarget);
        Assert.Equal("Animal Crossing · Animal Crossing", tile.Subtitle);
    }

    [Fact]
    public void EmptyIsLibraryAndAGameWaitingMakesItPrimary()
    {
        var empty = PlayAmiiboTile.Of(View(new AmiiboStatus()), Catalog, new(), pro2: true);
        Assert.False(empty.Resident);
        Assert.Equal("No Amiibo", empty.Title);
        Assert.Equal(PlayAmiiboAction.Library, empty.Action);
        Assert.False(empty.ActionPrimary);
        Assert.Null(empty.LibraryTarget);

        var wanted = PlayAmiiboTile.Of(View(new AmiiboStatus()), Catalog, new ConsoleView(1, AmiiboWanted: true), pro2: true);
        Assert.True(wanted.Wanted);
        Assert.True(wanted.ActionPrimary);
        Assert.Equal("Choose one from the library", wanted.Subtitle);
        Assert.StartsWith("The game is waiting for an amiibo.", wanted.AccessibleName);
    }

    [Fact]
    public void APresentedFigureSatisfiesTheGameAndADisconnectedAdapterSaysSo()
    {
        Assert.False(PlayAmiiboTile.Of(View(Loaded(presented: true)), Catalog, new ConsoleView(1, true), pro2: true).Wanted);
        var away = PlayAmiiboTile.Of(View(Loaded(), connected: false), Catalog, new ConsoleView(1, true), pro2: true);
        Assert.False(away.Resident);
        Assert.False(away.Wanted);
        Assert.Equal("Adapter not connected", away.Subtitle);
    }
}
