using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>One label/value line in the inspection pane.</summary>
/// <param name="Monospace">
/// For identifiers a person compares character by character — a UID or a figure
/// id — where proportional digits make scanning harder.
/// </param>
public sealed record AmiiboDetailRow(string Label, string Value, bool Monospace = false);

/// <summary>A named group of related lines.</summary>
public sealed record AmiiboDetailGroup(string Title, ValueList<AmiiboDetailRow> Rows)
{
    public bool Any => Rows.Count > 0;
}

/// <summary>
/// The selected Amiibo, organised the way someone actually reads it.
/// </summary>
/// <remarks>
/// REPLACES A FLAT KEY/VALUE SLAB. The same facts were all present before, in
/// one undifferentiated column, which made the page read like a database row
/// rather than a description of a figure. Grouping them means a user can find
/// "who owns this" without reading past the tag size.
///
/// Groups are omitted when they have nothing to say. A tag that was never set up
/// has no owner, no nickname and no dates, and printing five "—" lines is worse
/// than printing none: it implies the information exists and is missing, when in
/// fact the tag is simply blank.
///
/// Pure, so the grouping and omission rules are tested without a page.
/// </remarks>
public static class AmiiboInspection
{
    public static IReadOnlyList<AmiiboDetailGroup> Build(
        AmiiboLibraryItem item,
        AmiiboCatalogEntry? catalog,
        AmiiboDetails? decoded,
        bool onAdapter,
        bool adapterChanged)
    {
        var groups = new List<AmiiboDetailGroup>
        {
            Group("Identity", Identity(item, catalog)),
            Group("Tag", Tag(item, decoded)),
        };

        // Only when the tag carries them. See the remarks above.
        var registration = Registration(decoded);
        if (registration.Count > 0)
        {
            groups.Add(Group("Registration", registration));
        }

        groups.Add(Group("Game data", GameData(decoded, catalog)));
        groups.Add(Group("Adapter", Adapter(onAdapter, adapterChanged)));

        return [.. groups.Where(group => group.Any)];
    }

    private static AmiiboDetailGroup Group(string title, List<AmiiboDetailRow> rows) =>
        new(title, new ValueList<AmiiboDetailRow>(rows));

    private static List<AmiiboDetailRow> Identity(
        AmiiboLibraryItem item, AmiiboCatalogEntry? catalog)
    {
        var rows = new List<AmiiboDetailRow> { new("Your name", item.DisplayName) };

        var figure = catalog?.Name is { Length: > 0 } name
            ? name
            : catalog?.Character is { Length: > 0 } character
                ? character
                : null;

        // Only when it says something the user's own name does not.
        if (figure is not null &&
            !string.Equals(figure, item.DisplayName, StringComparison.CurrentCultureIgnoreCase))
        {
            rows.Add(new AmiiboDetailRow("Figure", figure));
        }

        Add(rows, "Game series", catalog?.GameSeries);
        Add(rows, "Collection", catalog?.AmiiboSeries);
        Add(rows, "Type", catalog?.Type);
        Add(rows, "Released", catalog?.ReleaseDate);
        rows.Add(new AmiiboDetailRow("Figure ID", item.FigureId, Monospace: true));
        return rows;
    }

    private static List<AmiiboDetailRow> Tag(
        AmiiboLibraryItem item, AmiiboDetails? decoded) =>
    [
        new("Tag type", item.TagType == AmiiboTagType.FigureV3 ? "Figure v3" : "NTAG215"),
        new("UID", item.Uid, Monospace: true),
        new("Size", $"{item.Size} bytes"),
        new("Contents", Contents(decoded)),
    ];

    /// <summary>
    /// What could be read from the encrypted body, in the user's terms.
    /// </summary>
    /// <remarks>
    /// Three distinguishable answers, because they need three different actions:
    /// import a key, investigate a bad file, or nothing at all.
    /// </remarks>
    private static string Contents(AmiiboDetails? decoded) => decoded?.Crypto switch
    {
        AmiiboCryptoState.Valid => decoded.SetUp ? "Set up" : "Blank, never set up",
        AmiiboCryptoState.Invalid => "Could not be decrypted with the imported keys",
        _ => "Import your Amiibo keys to read this",
    };

    private static List<AmiiboDetailRow> Registration(AmiiboDetails? decoded)
    {
        var rows = new List<AmiiboDetailRow>();
        if (decoded?.Crypto != AmiiboCryptoState.Valid || !decoded.SetUp)
        {
            return rows;
        }

        Add(rows, "Nickname", decoded.Nickname);
        Add(rows, "Owner", decoded.Owner);
        Add(rows, "Registered", decoded.SetupDate);
        Add(rows, "Last written", decoded.LastWriteDate);
        if (decoded.WriteCounter > 0)
        {
            rows.Add(new AmiiboDetailRow("Times written", decoded.WriteCounter.ToString()));
        }

        return rows;
    }

    private static List<AmiiboDetailRow> GameData(
        AmiiboDetails? decoded, AmiiboCatalogEntry? catalog)
    {
        if (decoded?.Crypto != AmiiboCryptoState.Valid)
        {
            return [];
        }

        if (!decoded.HasAppData)
        {
            // "Game", not "Game data": the group heading already says that, and
            // the row label must match the has-data branch below so the two
            // states line up in the same column.
            return [new AmiiboDetailRow("Game", "None")];
        }

        // The catalog can name the game where the firmware's small built-in
        // table cannot; its answer is preferred when it has one.
        var named = decoded.TitleId.Length > 0
            ? catalog?.TitleIds.FirstOrDefault(pair =>
                string.Equals(pair.Key, decoded.TitleId, StringComparison.OrdinalIgnoreCase)).Value
            : null;

        var rows = new List<AmiiboDetailRow>
        {
            new("Game", named is { Length: > 0 } ? named : decoded.AppDataLabel),
        };

        if (decoded.TitleId.Length > 0)
        {
            rows.Add(new AmiiboDetailRow("Title ID", decoded.TitleId, Monospace: true));
        }

        return rows;
    }

    private static List<AmiiboDetailRow> Adapter(bool onAdapter, bool changed) =>
    [
        new("Status", changed
            ? "On the adapter, changed by the console"
            : onAdapter
                ? "On the adapter"
                : "Not on the adapter"),
    ];

    private static void Add(List<AmiiboDetailRow> rows, string label, string? value)
    {
        if (value is { Length: > 0 })
        {
            rows.Add(new AmiiboDetailRow(label, value));
        }
    }
}
