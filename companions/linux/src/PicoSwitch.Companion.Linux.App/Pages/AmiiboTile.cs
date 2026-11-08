using System.ComponentModel;
using Avalonia.Media.Imaging;
using Avalonia.Threading;

namespace PicoSwitch.Companion.Linux.App.Pages;

/// <summary>
/// One item in the Amiibo library browser, in whichever view is showing. The
/// counterpart of the WinUI AmiiboTile: ONE type for all three views, so the
/// three cannot drift into three collections.
/// </summary>
/// <remarks>
/// Content is immutable; the bulk-selection marks and the artwork are
/// observable and mutated in place, so ticking an item or an image arriving
/// never rebuilds the collection (which would lose the scroll position).
/// </remarks>
internal sealed class AmiiboTile(
    string id,
    string title,
    string subtitle,
    string badge,
    string imageUrl,
    string series = "",
    string collection = "",
    string released = "",
    string figureId = "") : INotifyPropertyChanged
{
    private Bitmap? image;
    private bool imageRequested;
    private bool selectable;
    private bool ticked;

    public event PropertyChangedEventHandler? PropertyChanged;

    public string Id { get; } = id;

    public string Title { get; } = title;

    public string Subtitle { get; } = subtitle;

    public string Badge { get; } = badge;

    public bool HasBadge => Badge.Length > 0;

    public string Series { get; } = series;

    public string Collection { get; } = collection;

    public string Released { get; } = released;

    public string FigureId { get; } = figureId;

    public string AccessibleName => Subtitle.Length > 0 ? $"{Title}, {Subtitle}" : Title;

    /// <summary>
    /// The artwork, requested the first time a REALISED container asks for it,
    /// so a thousand-item library fetches what is on screen and nothing else.
    /// </summary>
    public Bitmap? Image
    {
        get
        {
            if (!imageRequested)
            {
                imageRequested = true;
                _ = LoadAsync();
            }

            return image;
        }
    }

    public bool HasImage => image is not null;

    public bool Selectable
    {
        get => selectable;
        set => Set(ref selectable, value, nameof(Selectable), nameof(ShowTicked));
    }

    public bool Ticked
    {
        get => ticked;
        set => Set(ref ticked, value, nameof(Ticked), nameof(ShowTicked));
    }

    public bool ShowTicked => Selectable && Ticked;

    private async Task LoadAsync()
    {
        var loaded = await AmiiboArtwork.LoadAsync(imageUrl);
        if (loaded is null)
        {
            return;
        }

        Dispatcher.UIThread.Post(() =>
        {
            image = loaded;
            Raise(nameof(Image));
            Raise(nameof(HasImage));
        });
    }

    private void Set(ref bool field, bool value, params string[] names)
    {
        if (field == value)
        {
            return;
        }

        field = value;
        foreach (var name in names)
        {
            Raise(name);
        }
    }

    private void Raise(string name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

/// <summary>
/// Catalog artwork, fetched once per address and decoded at tile width.
/// </summary>
/// <remarks>
/// Decoration only: a failure leaves the placeholder glyph and can never block
/// the library or an adapter action. This is the one request made per FIGURE,
/// so it reveals which figures are in the library to whoever serves the images;
/// it is gated by <see cref="AppServices.OnlineArtworkEnabled"/> in one place.
/// Bounded, and cleared wholesale rather than evicted: a refetch is cheap.
/// </remarks>
internal static class AmiiboArtwork
{
    private const int MaximumCached = 400;
    private const int DecodeWidth = 200;

    private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(20) };
    private static readonly Dictionary<string, Task<Bitmap?>> Cache = new(StringComparer.Ordinal);
    private static readonly Lock Gate = new();

    public static Task<Bitmap?> LoadAsync(string url)
    {
        if (url.Length == 0 || !AppServices.OnlineArtworkEnabled ||
            !Uri.TryCreate(url, UriKind.Absolute, out var uri) ||
            (uri.Scheme != Uri.UriSchemeHttps && uri.Scheme != Uri.UriSchemeHttp))
        {
            return Task.FromResult<Bitmap?>(null);
        }

        lock (Gate)
        {
            if (Cache.TryGetValue(url, out var cached))
            {
                return cached;
            }

            if (Cache.Count >= MaximumCached)
            {
                Cache.Clear();
            }

            var load = FetchAsync(uri);
            Cache[url] = load;
            return load;
        }
    }

    private static async Task<Bitmap?> FetchAsync(Uri uri)
    {
        try
        {
            var bytes = await Http.GetByteArrayAsync(uri).ConfigureAwait(false);
            using var stream = new MemoryStream(bytes);
            return Bitmap.DecodeToWidth(stream, DecodeWidth);
        }
        catch (Exception)
        {
            return null;
        }
    }
}
