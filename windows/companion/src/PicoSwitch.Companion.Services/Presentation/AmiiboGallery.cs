using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>How the library is ordered.</summary>
/// <remarks>
/// The same four the web portal offers, because they answer different questions:
/// <see cref="Default"/> is the order tags were added, which is what someone
/// looking for the one they just imported wants; <see cref="Number"/> sorts by
/// figure id, which groups a series in release order the way a shelf does.
/// </remarks>
public enum AmiiboSort
{
    Default,
    Name,
    Number,
    Release,
}

/// <summary>What the user has narrowed the library to.</summary>
public sealed record AmiiboGalleryFilters
{
    public string Search { get; init; } = "";

    public string GameSeries { get; init; } = "";

    public string AmiiboSeries { get; init; } = "";

    public string Type { get; init; } = "";

    public AmiiboSort Sort { get; init; } = AmiiboSort.Default;

    public bool Descending { get; init; }

    public bool Any =>
        Search.Length > 0 || GameSeries.Length > 0 ||
        AmiiboSeries.Length > 0 || Type.Length > 0;
}

/// <summary>One tile in the library grid.</summary>
public sealed record AmiiboCard
{
    public required string Id { get; init; }

    /// <summary>What to call it: the catalog's name, else the user's.</summary>
    public required string Title { get; init; }

    /// <summary>Series and type, or the figure id when the catalog is silent.</summary>
    public required string Subtitle { get; init; }

    public string ImageUrl { get; init; } = "";

    /// <summary>The user's own name, when it differs from the catalog's.</summary>
    public string OwnName { get; init; } = "";

    public bool OnAdapter { get; init; }

    public bool Changed { get; init; }

    public bool HasArtwork => ImageUrl.Length > 0;

    /// <summary>A one-line badge, or empty. Kept short: it sits on a tile.</summary>
    public string Badge => Changed ? "Changed" : OnAdapter ? "On adapter" : "";
}

/// <summary>The choices the filter controls should offer.</summary>
/// <remarks>
/// Derived from what the user actually owns, not from the whole catalog: a
/// dropdown listing every amiibo series in existence when the library holds
/// three figures is a worse control than one listing those three's series.
/// </remarks>
public sealed record AmiiboGalleryOptions(
    ValueList<string> GameSeries,
    ValueList<string> AmiiboSeries,
    ValueList<string> Types);

/// <summary>
/// The library as a browsable gallery: searched, filtered, sorted, illustrated.
/// </summary>
/// <remarks>
/// A flat list of names is the wrong shape for this. Amiibo are a collection —
/// people think in series and characters and recognise figures by their artwork
/// long before they read an id — and a library of forty tags with no picture, no
/// series and no search is unusable for the one job it has.
///
/// Everything here is pure, and the catalog arrives as a lookup function rather
/// than as a dependency, so the ordering and matching rules are testable without
/// a network or a page.
/// </remarks>
public static class AmiiboGallery
{
    public static IReadOnlyList<AmiiboCard> Build(
        IReadOnlyList<AmiiboLibraryItem> library,
        Func<string, AmiiboCatalogEntry?> catalog,
        string? loadedId,
        AmiiboGalleryFilters filters)
    {
        var cards = library
            .Select(item => Card(item, catalog(item.FigureId), item.Id == loadedId))
            .Where(pair => Matches(pair.Card, pair.Entry, pair.Item, filters))
            .ToList();

        var ordered = Order(cards, filters);
        return [.. ordered.Select(pair => pair.Card)];
    }

    public static AmiiboGalleryOptions Options(
        IReadOnlyList<AmiiboLibraryItem> library,
        Func<string, AmiiboCatalogEntry?> catalog)
    {
        var entries = library
            .Select(item => catalog(item.FigureId))
            .Where(entry => entry is not null)
            .Select(entry => entry!)
            .ToList();

        return new AmiiboGalleryOptions(
            Distinct(entries.Select(entry => entry.GameSeries)),
            Distinct(entries.Select(entry => entry.AmiiboSeries)),
            Distinct(entries.Select(entry => entry.Type)));
    }

    private static ValueList<string> Distinct(IEnumerable<string> values) =>
        new(values
            .Where(value => value.Length > 0)
            .Distinct(StringComparer.CurrentCultureIgnoreCase)
            .OrderBy(value => value, StringComparer.CurrentCultureIgnoreCase));

    private readonly record struct Row(
        AmiiboCard Card, AmiiboCatalogEntry? Entry, AmiiboLibraryItem Item);

    private static Row Card(
        AmiiboLibraryItem item, AmiiboCatalogEntry? entry, bool onAdapter)
    {
        var known = entry?.Name is { Length: > 0 } name
            ? name
            : entry?.Character is { Length: > 0 } character
                ? character
                : null;

        // The catalog's name leads when there is one, because "Tom Nook" is what
        // the figure IS. The user's own name is kept alongside rather than
        // discarded: they may well have called it "Nook for trading".
        var title = known ?? item.DisplayName;
        var own = known is not null &&
                  !string.Equals(known, item.DisplayName, StringComparison.CurrentCultureIgnoreCase)
            ? item.DisplayName
            : "";

        var subtitle = new List<string>();
        if (entry?.GameSeries is { Length: > 0 } series)
        {
            subtitle.Add(series);
        }

        if (entry?.Type is { Length: > 0 } type)
        {
            subtitle.Add(type);
        }

        if (subtitle.Count == 0)
        {
            // Nothing from the catalog: show what the tag itself says rather
            // than an empty line.
            subtitle.Add(item.TagType == AmiiboTagType.FigureV3 ? "Figure v3" : "NTAG215");
            subtitle.Add(item.FigureId);
        }

        return new Row(
            new AmiiboCard
            {
                Id = item.Id,
                Title = title,
                Subtitle = string.Join(" · ", subtitle),
                ImageUrl = entry?.ImageUrl ?? "",
                OwnName = own,
                OnAdapter = onAdapter,
                Changed = item.DirtyFromAdapter,
            },
            entry,
            item);
    }

    /// <summary>
    /// Search matches anything a person might type.
    /// </summary>
    /// <remarks>
    /// The user's own name, the catalog name, the character, both series, and the
    /// figure id. Someone hunting for a tag does not know or care which of those
    /// fields their memory of it came from.
    /// </remarks>
    private static bool Matches(
        AmiiboCard card, AmiiboCatalogEntry? entry, AmiiboLibraryItem item,
        AmiiboGalleryFilters filters)
    {
        if (filters.GameSeries.Length > 0 &&
            !Same(entry?.GameSeries, filters.GameSeries))
        {
            return false;
        }

        if (filters.AmiiboSeries.Length > 0 &&
            !Same(entry?.AmiiboSeries, filters.AmiiboSeries))
        {
            return false;
        }

        if (filters.Type.Length > 0 && !Same(entry?.Type, filters.Type))
        {
            return false;
        }

        if (filters.Search.Length == 0)
        {
            return true;
        }

        var needle = filters.Search.Trim();
        return Contains(item.DisplayName, needle) ||
               Contains(entry?.Name, needle) ||
               Contains(entry?.Character, needle) ||
               Contains(entry?.GameSeries, needle) ||
               Contains(entry?.AmiiboSeries, needle) ||
               Contains(item.FigureId, needle);
    }

    private static IEnumerable<Row> Order(List<Row> rows, AmiiboGalleryFilters filters)
    {
        var ordered = filters.Sort switch
        {
            AmiiboSort.Name => rows
                .OrderBy(row => row.Card.Title, StringComparer.CurrentCultureIgnoreCase)
                .ThenBy(row => row.Item.FigureId, StringComparer.Ordinal),

            // By figure id, which groups a series the way a shelf does.
            AmiiboSort.Number => rows
                .OrderBy(row => row.Item.FigureId, StringComparer.Ordinal)
                .ThenBy(row => row.Card.Title, StringComparer.CurrentCultureIgnoreCase),

            // Undated figures sort last rather than first: an empty string would
            // otherwise sort before every real date and put the unknowns on top.
            AmiiboSort.Release => rows
                .OrderBy(row => row.Entry?.ReleaseDate is { Length: > 0 } ? 0 : 1)
                .ThenBy(row => row.Entry?.ReleaseDate ?? "", StringComparer.Ordinal)
                .ThenBy(row => row.Card.Title, StringComparer.CurrentCultureIgnoreCase),

            // The order they were added, newest first, so the tag someone just
            // imported is where they will look for it.
            _ => rows.OrderByDescending(row => row.Item.Imported),
        };

        return filters.Descending ? ordered.Reverse() : ordered;
    }

    private static bool Same(string? value, string expected) =>
        string.Equals(value ?? "", expected, StringComparison.CurrentCultureIgnoreCase);

    private static bool Contains(string? haystack, string needle) =>
        haystack is { Length: > 0 } &&
        haystack.Contains(needle, StringComparison.CurrentCultureIgnoreCase);
}
