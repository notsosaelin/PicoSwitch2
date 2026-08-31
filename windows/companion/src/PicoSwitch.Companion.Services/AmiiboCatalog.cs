using System.Text.Json;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>One figure's public metadata, as AmiiboAPI describes it.</summary>
public sealed record AmiiboCatalogEntry
{
    /// <summary>The 16-hex figure id, which is `head` + `tail`.</summary>
    public required string Id { get; init; }

    /// <summary>AmiiboAPI's display name, kept separate from the character.</summary>
    public string Name { get; init; } = "";

    public string Character { get; init; } = "";

    public string GameSeries { get; init; } = "";

    public string AmiiboSeries { get; init; } = "";

    public string Type { get; init; } = "";

    public string ReleaseDate { get; init; } = "";

    public string ImageUrl { get; init; } = "";

    /// <summary>Console title id to game name, for naming the tag's app data.</summary>
    public ValueList<KeyValuePair<string, string>> TitleIds { get; init; } =
        ValueList<KeyValuePair<string, string>>.Empty;
}

/// <summary>
/// Cache-first AmiiboAPI enrichment: what a figure is actually called.
/// </summary>
/// <remarks>
/// WHY THIS EXISTS. Without it the library can only show what is on the tag —
/// `0183000002420502` — which is a correct answer to a question nobody asked.
/// AmiiboAPI turns that into "Tom Nook", its series, and the games that use it.
/// The Android companion has done this since before the Windows port; this is
/// the missing half of that parity.
///
/// ## What goes on the wire
///
/// NOTHING OF THE USER'S. The request is a plain GET of the whole public
/// catalog, identical for every user of the app. No UID, no figure id, no tag
/// bytes, no decrypted register field, no key material, and no per-tag lookup
/// that would leak which figures somebody owns. Matching happens locally against
/// the downloaded table.
///
/// ## Failure is not an error
///
/// The catalog is enrichment. Offline, blocked, rate-limited, or simply
/// unreachable all degrade to the identity the tag itself carries, and nothing
/// on the page stops working. That is why every method here is total and none of
/// them reports a failure to the user.
///
/// Cached for a week, with a retry floor, so a launch does not depend on a third
/// party being up and a failing endpoint is not hammered.
/// </remarks>
public sealed class AmiiboCatalog
{
    private const int MaxResponseBytes = 4 * 1024 * 1024;
    private static readonly TimeSpan MaxCacheAge = TimeSpan.FromDays(7);
    private static readonly TimeSpan RetryInterval = TimeSpan.FromMinutes(1);

    /// <summary>
    /// Mirrors first, then the primary. Both are the documented public API; the
    /// Android client uses the same pair in the same order, so the two platforms
    /// cannot disagree about where this data comes from.
    /// </summary>
    private static readonly string[] Endpoints =
    [
        "https://amiiboapi.org/api/amiibo/?showgames",
        "https://www.amiiboapi.com/api/amiibo/?showgames",
    ];

    private readonly string cachePath;
    private readonly Func<string, CancellationToken, Task<string>> fetch;
    private readonly Lock gate = new();

    private Dictionary<string, AmiiboCatalogEntry> byId = new(StringComparer.OrdinalIgnoreCase);
    private Dictionary<string, string> titleNames = new(StringComparer.OrdinalIgnoreCase);
    private DateTimeOffset cachedAt = DateTimeOffset.MinValue;
    private DateTimeOffset lastAttempt = DateTimeOffset.MinValue;

    public AmiiboCatalog(
        string directory,
        Func<string, CancellationToken, Task<string>>? fetcher = null)
    {
        Directory.CreateDirectory(directory);
        cachePath = Path.Combine(directory, "amiibo-catalog.json");

        // Injectable so the parsing and caching rules are testable without a
        // network, which is most of what can actually go wrong here.
        fetch = fetcher ?? DownloadAsync;
        LoadCache();
    }

    /// <summary>True when anything at all is known; the page uses it for a hint.</summary>
    public bool Available
    {
        get
        {
            lock (gate)
            {
                return byId.Count > 0;
            }
        }
    }

    /// <summary>What this figure is, or null when the catalog cannot say.</summary>
    public AmiiboCatalogEntry? Find(string figureId)
    {
        lock (gate)
        {
            return byId.GetValueOrDefault(figureId);
        }
    }

    /// <summary>The game a title id belongs to, for naming a tag's app data.</summary>
    public string? GameForTitleId(string titleId)
    {
        lock (gate)
        {
            return titleNames.GetValueOrDefault(titleId);
        }
    }

    /// <summary>
    /// Refresh if the cache is stale, and report whether anything is known.
    /// </summary>
    /// <remarks>
    /// Never throws. A caller is expected to fire this and carry on rendering;
    /// the result only says whether a later render will have more to show.
    /// </remarks>
    public async Task<bool> EnsureLoadedAsync(CancellationToken cancellationToken = default)
    {
        var now = DateTimeOffset.UtcNow;

        lock (gate)
        {
            if (byId.Count > 0 && now - cachedAt < MaxCacheAge)
            {
                return true;
            }

            if (now - lastAttempt < RetryInterval)
            {
                return byId.Count > 0;
            }

            lastAttempt = now;
        }

        foreach (var endpoint in Endpoints)
        {
            string payload;
            try
            {
                payload = await fetch(endpoint, cancellationToken).ConfigureAwait(false);
            }
            catch (Exception)
            {
                // Offline, blocked, rate-limited, DNS-poisoned: all the same to
                // this class. Try the next mirror, then give up quietly.
                continue;
            }

            var entries = Parse(payload);
            if (entries.Count == 0)
            {
                continue;
            }

            Install(entries, now);
            TryWriteCache();
            return true;
        }

        lock (gate)
        {
            return byId.Count > 0;
        }
    }

    /// <summary>
    /// Parse a documented AmiiboAPI response. Pure, and never throws.
    /// </summary>
    /// <remarks>
    /// Keyed by <c>head</c> + <c>tail</c> rather than by the server's own
    /// <c>id</c> field, deliberately: the figure id this app derives from tag
    /// bytes IS head+tail, and a mirror that populated <c>id</c> differently
    /// would silently match nothing.
    /// </remarks>
    public static IReadOnlyList<AmiiboCatalogEntry> Parse(string payload)
    {
        var entries = new List<AmiiboCatalogEntry>();

        try
        {
            using var document = JsonDocument.Parse(payload);
            if (!document.RootElement.TryGetProperty("amiibo", out var array) ||
                array.ValueKind != JsonValueKind.Array)
            {
                return entries;
            }

            foreach (var value in array.EnumerateArray())
            {
                var entry = ParseEntry(value);
                if (entry is not null)
                {
                    entries.Add(entry);
                }
            }
        }
        catch (JsonException)
        {
            return entries;
        }

        return entries;
    }

    private static AmiiboCatalogEntry? ParseEntry(JsonElement value)
    {
        var head = Text(value, "head");
        var tail = Text(value, "tail");
        var id = (head.Length > 0 || tail.Length > 0 ? head + tail : Text(value, "id"))
            .ToUpperInvariant();

        if (id.Length != 16 || !id.All(Uri.IsHexDigit))
        {
            return null;
        }

        // The earliest announced region date, so the field is stable rather than
        // dependent on which region the response happens to list first.
        var releaseDate = "";
        if (value.TryGetProperty("release", out var release) &&
            release.ValueKind == JsonValueKind.Object)
        {
            releaseDate = new[] { "na", "us", "jp", "eu", "au", "nz" }
                .Select(region => Text(release, region))
                .Where(date => date.Length > 0)
                .OrderBy(date => date, StringComparer.Ordinal)
                .FirstOrDefault() ?? "";
        }

        var titleIds = new List<KeyValuePair<string, string>>();
        foreach (var field in new[]
                 {
                     "gamesSwitch", "gamesWiiU", "games3DS",
                 })
        {
            if (!value.TryGetProperty(field, out var games) ||
                games.ValueKind != JsonValueKind.Array)
            {
                continue;
            }

            foreach (var game in games.EnumerateArray())
            {
                var name = Text(game, "gameName");
                if (name.Length == 0 ||
                    !game.TryGetProperty("gameID", out var ids) ||
                    ids.ValueKind != JsonValueKind.Array)
                {
                    continue;
                }

                foreach (var titleId in ids.EnumerateArray())
                {
                    var text = (titleId.GetString() ?? "").ToUpperInvariant();
                    if (text.Length == 16)
                    {
                        titleIds.Add(new KeyValuePair<string, string>(text, name));
                    }
                }
            }
        }

        return new AmiiboCatalogEntry
        {
            Id = id,
            Name = Text(value, "name"),
            Character = Text(value, "character"),
            GameSeries = Text(value, "gameSeries"),
            AmiiboSeries = Text(value, "amiiboSeries"),
            Type = Text(value, "type"),
            ReleaseDate = releaseDate,
            ImageUrl = Text(value, "image"),
            TitleIds = new ValueList<KeyValuePair<string, string>>(titleIds),
        };
    }

    private void Install(IReadOnlyList<AmiiboCatalogEntry> entries, DateTimeOffset now)
    {
        var index = new Dictionary<string, AmiiboCatalogEntry>(StringComparer.OrdinalIgnoreCase);
        var titles = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        foreach (var entry in entries)
        {
            index[entry.Id] = entry;
            foreach (var (titleId, name) in entry.TitleIds)
            {
                titles[titleId] = name;
            }
        }

        lock (gate)
        {
            byId = index;
            titleNames = titles;
            cachedAt = now;
        }
    }

    private static async Task<string> DownloadAsync(string url, CancellationToken cancellationToken)
    {
        using var client = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
        using var response = await client.GetAsync(url, cancellationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();

        if (response.Content.Headers.ContentLength > MaxResponseBytes)
        {
            throw new InvalidOperationException("Amiibo catalog response is too large");
        }

        // Bounded by what is actually read, not by the declared length: the
        // header is the server's claim, not a limit.
        using var stream = await response.Content.ReadAsStreamAsync(cancellationToken)
            .ConfigureAwait(false);
        using var buffer = new MemoryStream();
        var chunk = new byte[64 * 1024];

        while (true)
        {
            var count = await stream.ReadAsync(chunk, cancellationToken).ConfigureAwait(false);
            if (count <= 0)
            {
                break;
            }

            if (buffer.Length + count > MaxResponseBytes)
            {
                throw new InvalidOperationException("Amiibo catalog response is too large");
            }

            buffer.Write(chunk, 0, count);
        }

        return System.Text.Encoding.UTF8.GetString(buffer.ToArray());
    }

    // ------------------------------------------------------------------ cache

    private void LoadCache()
    {
        try
        {
            if (!File.Exists(cachePath))
            {
                return;
            }

            using var document = JsonDocument.Parse(File.ReadAllText(cachePath));
            var root = document.RootElement;

            var stamp = root.TryGetProperty("cachedAt", out var value)
                ? DateTimeOffset.Parse(value.GetString()!)
                : DateTimeOffset.MinValue;

            var entries = new List<AmiiboCatalogEntry>();
            if (root.TryGetProperty("entries", out var array) &&
                array.ValueKind == JsonValueKind.Array)
            {
                foreach (var entry in array.EnumerateArray())
                {
                    var titleIds = new List<KeyValuePair<string, string>>();
                    if (entry.TryGetProperty("titleIds", out var titles) &&
                        titles.ValueKind == JsonValueKind.Object)
                    {
                        foreach (var title in titles.EnumerateObject())
                        {
                            titleIds.Add(new KeyValuePair<string, string>(
                                title.Name, title.Value.GetString() ?? ""));
                        }
                    }

                    entries.Add(new AmiiboCatalogEntry
                    {
                        Id = Text(entry, "id"),
                        Name = Text(entry, "name"),
                        Character = Text(entry, "character"),
                        GameSeries = Text(entry, "gameSeries"),
                        AmiiboSeries = Text(entry, "amiiboSeries"),
                        Type = Text(entry, "type"),
                        ReleaseDate = Text(entry, "releaseDate"),
                        ImageUrl = Text(entry, "imageUrl"),
                        TitleIds = new ValueList<KeyValuePair<string, string>>(titleIds),
                    });
                }
            }

            Install(entries, stamp);
        }
        catch (Exception)
        {
            // A damaged cache costs enrichment until the next refresh, which is
            // the cheapest possible consequence.
        }
    }

    private void TryWriteCache()
    {
        try
        {
            AmiiboCatalogEntry[] snapshot;
            DateTimeOffset stamp;
            lock (gate)
            {
                snapshot = [.. byId.Values];
                stamp = cachedAt;
            }

            var document = new Dictionary<string, object>
            {
                ["cachedAt"] = stamp.ToString("O"),
                ["entries"] = snapshot.Select(entry => new Dictionary<string, object>
                {
                    ["id"] = entry.Id,
                    ["name"] = entry.Name,
                    ["character"] = entry.Character,
                    ["gameSeries"] = entry.GameSeries,
                    ["amiiboSeries"] = entry.AmiiboSeries,
                    ["type"] = entry.Type,
                    ["releaseDate"] = entry.ReleaseDate,
                    ["imageUrl"] = entry.ImageUrl,
                    ["titleIds"] = entry.TitleIds.ToDictionary(
                        pair => pair.Key, pair => pair.Value),
                }).ToList(),
            };

            var temporary = cachePath + ".tmp";
            File.WriteAllBytes(temporary, JsonSerializer.SerializeToUtf8Bytes(document));
            File.Move(temporary, cachePath, overwrite: true);
        }
        catch (Exception)
        {
            // An unwritable cache costs a refresh next launch, nothing more.
        }
    }

    private static string Text(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? ""
            : "";
}
