using PicoSwitch.Companion.Services;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// AmiiboAPI enrichment: parsing, caching, and failing quietly.
/// </summary>
/// <remarks>
/// NO NETWORK. The fetcher is injected, so what is tested is the part that can
/// actually be wrong — how a response is interpreted, what happens when it is
/// absent or malformed, and that a failure costs enrichment and nothing else.
/// </remarks>
public sealed class AmiiboCatalogTests : IDisposable
{
    private readonly string root =
        Path.Combine(Path.GetTempPath(), "picoswitch-catalog-" + Guid.NewGuid().ToString("N"));

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

    /// <summary>A response in the documented shape, for Tom Nook.</summary>
    private const string Payload = """
    {
      "amiibo": [
        {
          "head": "01830000",
          "tail": "02420502",
          "name": "Tom Nook",
          "character": "Tom Nook",
          "gameSeries": "Animal Crossing",
          "amiiboSeries": "Animal Crossing",
          "type": "Figure",
          "image": "https://example.invalid/nook.png",
          "release": { "na": "2015-11-13", "jp": "2015-07-30", "eu": "2015-11-20" },
          "gamesSwitch": [
            { "gameName": "Animal Crossing: New Horizons", "gameID": ["0100F95004ECC000"] }
          ]
        }
      ]
    }
    """;

    private AmiiboCatalog Catalog(Func<string, CancellationToken, Task<string>> fetcher) =>
        new(root, fetcher);

    private static Func<string, CancellationToken, Task<string>> Returns(string payload) =>
        (_, _) => Task.FromResult(payload);

    private static Func<string, CancellationToken, Task<string>> Throws() =>
        (_, _) => throw new HttpRequestException("offline");

    [Fact]
    public void ParsingKeysByHeadAndTail()
    {
        // The figure id derived from tag bytes IS head+tail. A mirror that
        // populated its own `id` field differently would silently match nothing,
        // so the key is built rather than taken.
        var entries = AmiiboCatalog.Parse(Payload);

        var entry = Assert.Single(entries);
        Assert.Equal("0183000002420502", entry.Id);
        Assert.Equal("Tom Nook", entry.Name);
        Assert.Equal("Animal Crossing", entry.GameSeries);
        Assert.Equal("https://example.invalid/nook.png", entry.ImageUrl);
    }

    [Fact]
    public void TheEarliestRegionDateIsUsed()
    {
        // Stable regardless of which region the response happens to list first.
        Assert.Equal("2015-07-30", AmiiboCatalog.Parse(Payload)[0].ReleaseDate);
    }

    [Fact]
    public void TitleIdsMapToGameNames()
    {
        var entry = AmiiboCatalog.Parse(Payload)[0];
        var title = Assert.Single(entry.TitleIds);
        Assert.Equal("0100F95004ECC000", title.Key);
        Assert.Equal("Animal Crossing: New Horizons", title.Value);
    }

    [Fact]
    public void MalformedResponsesYieldNothingRatherThanThrowing()
    {
        Assert.Empty(AmiiboCatalog.Parse("not json"));
        Assert.Empty(AmiiboCatalog.Parse("{}"));
        Assert.Empty(AmiiboCatalog.Parse("""{"amiibo": "wrong shape"}"""));
        // An entry whose id is not a 16-hex figure id is dropped, not guessed at.
        Assert.Empty(AmiiboCatalog.Parse("""{"amiibo":[{"head":"zz","tail":"yy"}]}"""));
    }

    [Fact]
    public async Task ALoadedCatalogAnswersLookups()
    {
        var catalog = Catalog(Returns(Payload));

        Assert.True(await catalog.EnsureLoadedAsync());
        Assert.True(catalog.Available);
        Assert.Equal("Tom Nook", catalog.Find("0183000002420502")?.Name);
        // Case-insensitive: the id comes from formatted tag bytes.
        Assert.NotNull(catalog.Find("0183000002420502".ToLowerInvariant()));
        Assert.Equal(
            "Animal Crossing: New Horizons",
            catalog.GameForTitleId("0100F95004ECC000"));
    }

    [Fact]
    public async Task AnUnknownFigureSimplyHasNoAnswer()
    {
        // Legitimate and common: a homebrew or unreleased figure. Returning null
        // is right; inventing a name would be worse than showing the id.
        var catalog = Catalog(Returns(Payload));
        await catalog.EnsureLoadedAsync();

        Assert.Null(catalog.Find("FFFFFFFFFFFFFFFF"));
        Assert.Null(catalog.GameForTitleId("0000000000000000"));
    }

    [Fact]
    public async Task BeingOfflineIsNotAnError()
    {
        // The catalog is enrichment. Offline must degrade to the identity the tag
        // itself carries, with nothing on the page breaking and nothing thrown.
        var catalog = Catalog(Throws());

        Assert.False(await catalog.EnsureLoadedAsync());
        Assert.False(catalog.Available);
        Assert.Null(catalog.Find("0183000002420502"));
    }

    [Fact]
    public async Task AGarbageResponseIsTreatedAsNoAnswer()
    {
        var catalog = Catalog(Returns("<html>rate limited</html>"));

        Assert.False(await catalog.EnsureLoadedAsync());
        Assert.False(catalog.Available);
    }

    [Fact]
    public async Task TheCacheSurvivesARestart()
    {
        var first = Catalog(Returns(Payload));
        Assert.True(await first.EnsureLoadedAsync());

        // A second instance over the same directory is what a restart looks like,
        // and it must answer without going near the network.
        var offline = Catalog(Throws());
        Assert.True(offline.Available);
        Assert.Equal("Tom Nook", offline.Find("0183000002420502")?.Name);
        Assert.Equal(
            "Animal Crossing: New Horizons",
            offline.GameForTitleId("0100F95004ECC000"));
    }

    [Fact]
    public async Task AFreshCacheIsNotRefetched()
    {
        var calls = 0;
        var catalog = Catalog((_, _) =>
        {
            calls++;
            return Task.FromResult(Payload);
        });

        Assert.True(await catalog.EnsureLoadedAsync());
        Assert.True(await catalog.EnsureLoadedAsync());
        Assert.True(await catalog.EnsureLoadedAsync());

        Assert.Equal(1, calls);
    }

    [Fact]
    public async Task AFailingEndpointIsNotHammered()
    {
        // A retry floor, so a page that renders repeatedly does not turn into a
        // request per render against a service that is already refusing.
        var calls = 0;
        var catalog = Catalog((_, _) =>
        {
            calls++;
            throw new HttpRequestException("offline");
        });

        Assert.False(await catalog.EnsureLoadedAsync());
        Assert.False(await catalog.EnsureLoadedAsync());
        Assert.False(await catalog.EnsureLoadedAsync());

        // Both endpoints tried once, then the retry floor holds.
        Assert.Equal(2, calls);
    }

    [Fact]
    public async Task ADamagedCacheCostsEnrichmentAndNothingElse()
    {
        var first = Catalog(Returns(Payload));
        await first.EnsureLoadedAsync();

        File.WriteAllText(Path.Combine(root, "amiibo-catalog.json"), "{ not json");

        var reopened = Catalog(Throws());
        Assert.False(reopened.Available);
        Assert.Null(reopened.Find("0183000002420502"));
    }
}
