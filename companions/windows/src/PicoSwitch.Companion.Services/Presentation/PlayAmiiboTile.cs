using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>What Play's Amiibo tile does (companion-shell-layout.md, "Play").</summary>
public enum PlayAmiiboAction
{
    Present,
    Eject,
    Library,
}

/// <summary>
/// Play's Amiibo tile: the figure the adapter holds, as art and a name (never
/// a UID), and one full-width action — Present / Eject when one is loaded,
/// Library otherwise. While a game has an NFC scan running the tile asks for a
/// figure and its action becomes the primary one. Android's AmiiboTileCard,
/// as one projection both desktop shells paint.
/// </summary>
public sealed record PlayAmiiboTile
{
    public required bool Resident { get; init; }

    /// <summary>The game is waiting for an amiibo (and none is presented).</summary>
    public required bool Wanted { get; init; }

    public required string Title { get; init; }

    public required string Subtitle { get; init; }

    public required string ImageUrl { get; init; }

    public required PlayAmiiboAction Action { get; init; }

    public required bool ActionEnabled { get; init; }

    /// <summary>The action is the page's accent (Present, or Library while a game waits).</summary>
    public required bool ActionPrimary { get; init; }

    /// <summary>
    /// Where the library opens: this exact tag when the library has it, else the
    /// same figure (another dump of it), so the tile lands on what the adapter
    /// holds rather than on the first figure in the library. Null: no target.
    /// </summary>
    public string? LibraryTarget { get; init; }

    public required string AccessibleName { get; init; }

    public string ActionLabel => Action switch
    {
        PlayAmiiboAction.Present => "Present",
        PlayAmiiboAction.Eject => "Eject",
        _ => "Library",
    };

    public static PlayAmiiboTile Of(
        AmiiboView view,
        Func<string, AmiiboCatalogEntry?> catalog,
        ConsoleView console,
        bool pro2)
    {
        var status = view.Status;
        var resident = view.Connected && view.AnythingLoaded;
        var backup = resident ? view.LoadedFromLibrary : null;
        var entry = resident && status.FigureId.Length > 0 ? catalog(status.FigureId) : null;
        var name = entry?.Name is { Length: > 0 } known ? known : backup?.DisplayName ?? "Amiibo";
        var target = backup?.Id ?? (resident && status.FigureId.Length > 0
            ? view.Library.FirstOrDefault(item =>
                string.Equals(item.FigureId, status.FigureId, StringComparison.OrdinalIgnoreCase))?.Id
            : null);
        var wanted = ConsoleCues.AmiiboWanted(view.Connected, console) && !status.Presented;

        var catalogLine = string.Join(" · ", new[] { entry?.GameSeries, entry?.AmiiboSeries }
            .Where(part => !string.IsNullOrWhiteSpace(part)));
        var subtitle = !view.Connected
            ? "Adapter not connected"
            : !resident
                ? wanted ? "Choose one from the library" : ""
                : catalogLine.Length > 0
                    ? catalogLine
                    : backup is not null ? "Saved on this computer" : "Not in your library";

        var action = !resident
            ? PlayAmiiboAction.Library
            : status.Presented ? PlayAmiiboAction.Eject : PlayAmiiboAction.Present;
        var enabled = action switch
        {
            PlayAmiiboAction.Present => pro2 && view.CanPresent,
            PlayAmiiboAction.Eject => pro2 && view.CanEject,
            _ => true,
        };

        var spoken = resident ? $"Amiibo: {name}. Open library." : "No Amiibo loaded. Open library.";
        return new PlayAmiiboTile
        {
            Resident = resident,
            Wanted = wanted,
            Title = resident ? name : "No Amiibo",
            Subtitle = subtitle,
            ImageUrl = entry?.ImageUrl ?? "",
            Action = action,
            ActionEnabled = enabled,
            ActionPrimary = action == PlayAmiiboAction.Present || (action == PlayAmiiboAction.Library && wanted),
            LibraryTarget = target,
            AccessibleName = wanted ? "The game is waiting for an amiibo. " + spoken : spoken,
        };
    }
}
