using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;
using Windows.Storage;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// One tile in the Amiibo library grid.
/// </summary>
/// <remarks>
/// Top-level and public because the XAML item template binds to it: the markup
/// compiler cannot reach a nested type through <c>x:DataType</c>, and generates
/// code that does not compile if asked to.
///
/// Carries a resolved <see cref="Image"/> rather than a URL, so the template has
/// no converter and no binding that can fail silently on a bad address.
/// </remarks>
public sealed record AmiiboTile(
    string Id,
    string Title,
    string Subtitle,
    string Badge,
    ImageSource? Image)
{
    public Visibility BadgeVisibility =>
        Badge.Length > 0 ? Visibility.Visible : Visibility.Collapsed;

    /// <summary>Shown when there is no artwork, so the tile is never a gap.</summary>
    public Visibility PlaceholderVisibility =>
        Image is null ? Visibility.Visible : Visibility.Collapsed;
}

/// <summary>
/// Amiibo: the user's tag backups, and the one the adapter is holding.
/// </summary>
/// <remarks>
/// TWO STORES AGAIN, the same shape as the Keyboard and Mouse page. The library
/// is the user's and works with nothing paired — import, rename, export, delete
/// and inspect all run offline. The adapter holds exactly one tag at a time, and
/// only Send, Present, Eject, Sync and Clear reach it.
///
/// THE RULE THIS PAGE EXISTS TO ENFORCE: when a game writes to a tag, the change
/// lives ONLY on the adapter. Sending another tag over it, or clearing it, would
/// destroy the only copy of a save. Both are refused until the user syncs, and
/// the warning that says so sits above the cards rather than inside one, because
/// it is the fact that governs everything else on the page.
///
/// All rules live in <see cref="AmiiboView"/>; this file renders state and raises
/// events. Every `async void` handler goes through <see cref="GuardAsync"/>: an
/// exception escaping one has no handler at all and terminates the process.
/// </remarks>
public sealed partial class AmiiboPage : Page
{
    private readonly AdapterConnectionService adapters = AppServices.Adapters;
    private readonly AmiiboLibrary library = AppServices.AmiiboLibrary;
    private readonly WindowsAmiiboKeyStore keys = AppServices.AmiiboKeys;
    private readonly AmiiboCatalog catalog = AppServices.AmiiboCatalog;

    private string? selectedId;
    private bool suppressSelection;
    private bool dialogOpen;
    private (int Completed, int Total)? transfer;
    private CancellationTokenSource? transferCancellation;
    private bool amiiboRead;

    private AmiiboGalleryFilters filters = new();

    /// <summary>Resolved artwork, keyed by URL so a re-render costs nothing.</summary>
    private readonly Dictionary<string, ImageSource> artwork = new(StringComparer.Ordinal);

    /// <summary>The "no filter" row. Not a series anyone owns.</summary>
    private const string AllFilter = "All";

    public AmiiboPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private async void OnLoaded(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            adapters.Snapshot.Changed += OnStateChanged;
            adapters.Connection.Changed += OnStateChanged;
            library.Items.Changed += OnStateChanged;

            Render();
            ReportLibraryWarnings();

            // Fire and forget: the catalog is enrichment, so the page is fully
            // usable before it answers and simply gets better if it does. It
            // sends nothing about this user -- see AmiiboCatalog.
            _ = catalog.EnsureLoadedAsync().ContinueWith(
                _ => AppServices.OnUiThread(Render),
                TaskScheduler.Default);

            if (adapters.Connection.Value.Phase == ConnectionPhase.Connected)
            {
                // Marked before awaiting, so the state-change handler does not
                // fire a second read while this one is still in flight.
                amiiboRead = true;
                await SafeAsync(() => adapters.RefreshAmiiboAsync());
            }
        });

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Connection.Changed -= OnStateChanged;
        library.Items.Changed -= OnStateChanged;

        // A transfer outlives the page otherwise, and its progress callbacks
        // would touch controls that are gone.
        transferCancellation?.Cancel();
    }

    /// <summary>
    /// Repaint, and pick up an adapter that connected after the page opened.
    /// </summary>
    /// <remarks>
    /// Without the second half the page only ever read the adapter in
    /// <c>OnLoaded</c>, so arriving here first and connecting second left it
    /// stuck reporting that nothing had been read — with a Reload button as the
    /// only way out of a state the app could see for itself.
    /// </remarks>
    private void OnStateChanged() => AppServices.OnUiThread(() =>
    {
        Render();

        var connected = adapters.Connection.Value.Phase == ConnectionPhase.Connected;
        if (connected && !amiiboRead && transfer is null)
        {
            amiiboRead = true;
            _ = GuardAsync(() => SafeAsync(() => adapters.RefreshAmiiboAsync()));
        }
        else if (!connected)
        {
            // So a reconnect reads again rather than trusting a stale snapshot.
            amiiboRead = false;
        }
    });

    // ----------------------------------------------------------------- adapter

    private async void OnRefresh(object sender, RoutedEventArgs e) =>
        await GuardAsync(() => SafeAsync(() => adapters.RefreshAmiiboAsync()));

    private async void OnPresent(object sender, RoutedEventArgs e) =>
        await GuardAsync(() => SafeAsync(() => adapters.SetAmiiboPresentedAsync(true)));

    private async void OnEject(object sender, RoutedEventArgs e) =>
        await GuardAsync(() => SafeAsync(() => adapters.SetAmiiboPresentedAsync(false)));

    private async void OnUseOriginal(object sender, RoutedEventArgs e) =>
        await GuardAsync(() => SafeAsync(() => adapters.SelectAmiiboCopyAsync(false)));

    private async void OnUseConsoleCopy(object sender, RoutedEventArgs e) =>
        await GuardAsync(() => SafeAsync(() => adapters.SelectAmiiboCopyAsync(true)));

    /// <summary>
    /// Send the selected backup to the adapter.
    /// </summary>
    /// <remarks>
    /// Replacing what the adapter holds is refused while there are unsynced
    /// console writes — the view already disables the button, and the client
    /// refuses independently, so this cannot be the path that loses a save.
    /// </remarks>
    private async void OnUpload(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            var view = View();
            if (view.Selected is not { } item || !view.CanUpload)
            {
                return;
            }

            byte[] bytes;
            try
            {
                bytes = library.Bytes(item.Id);
            }
            catch (Exception error)
            {
                Report($"'{item.DisplayName}' could not be read: {error.Message}",
                       InfoBarSeverity.Error);
                return;
            }

            await RunTransferAsync(
                token => adapters.UploadAmiiboAsync(
                    bytes, useSave2: false, progress: OnTransferProgress, cancellationToken: token),
                $"Sending '{item.DisplayName}'…",
                $"'{item.DisplayName}' is on the adapter. Present it when the game asks.");
        });

    /// <summary>
    /// Read the adapter's tag back and store it, THEN acknowledge.
    /// </summary>
    /// <remarks>
    /// The order is the whole point. The adapter keeps its dirty flag until the
    /// companion confirms the bytes are stored, so acknowledging before the
    /// library write would clear it on data that had been saved nowhere — and a
    /// crash in between would lose the save silently.
    ///
    /// The acknowledge is generation- and CRC-guarded, so if the console wrote
    /// again during the read it is refused and the tag stays dirty. That is
    /// correct: the bytes just stored are real, they are simply no longer the
    /// latest, and the user can sync again.
    /// </remarks>
    private async void OnSync(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (!View().CanSync)
            {
                return;
            }

            AmiiboDownload? download = null;
            await RunTransferAsync(
                async token =>
                {
                    download = await adapters.DownloadAmiiboAsync(OnTransferProgress, token);
                    return download;
                },
                "Reading the tag from the adapter…",
                completion: null);

            if (download is null)
            {
                return;
            }

            var stored = library.UpdateFromAdapter(View().LoadedFromLibrary?.Id, download.Bytes);
            selectedId = stored.Id;

            try
            {
                await adapters.AcknowledgeAmiiboDownloadAsync(download);
                Report($"'{stored.DisplayName}' synced to your library.",
                       InfoBarSeverity.Success);
            }
            catch (Exception error)
            {
                // The bytes ARE saved; only the adapter's flag was not cleared.
                // Saying so precisely is the difference between a user retrying
                // and a user thinking they lost a save.
                Report(
                    $"'{stored.DisplayName}' was saved to your library, but the adapter " +
                    $"would not clear its changed flag: {ManagementErrorText.Summarize(error)} " +
                    "Sync again to pick up the newer state.",
                    InfoBarSeverity.Warning);
            }

            Render();
        });

    private async void OnClear(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (!View().CanClear)
            {
                return;
            }

            if (!await ConfirmAsync(
                    "Remove the Amiibo from the adapter?",
                    "The adapter stops holding this tag. Your library keeps its copy.",
                    "Clear"))
            {
                return;
            }

            await SafeAsync(() => adapters.ClearAmiiboAsync());
        });

    private void OnCancelTransfer(object sender, RoutedEventArgs e) =>
        transferCancellation?.Cancel();

    // ----------------------------------------------------------------- library

    /// <summary>
    /// Import any number of dumps and archives, in one go.
    /// </summary>
    /// <remarks>
    /// ONE button for every kind of file. The picker is multi-select and accepts
    /// dumps and zips together, and the library works out which is which — asking
    /// the user to first classify what they have, and then pick a matching menu
    /// item, is work the app can simply do.
    /// </remarks>
    private async void OnImport(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            var files = await PickFilesAsync(".bin", ".nfc", ".zip");
            if (files.Count == 0)
            {
                return;
            }

            var sources = new List<AmiiboImportSource>();
            foreach (var file in files)
            {
                sources.Add(new AmiiboImportSource(file.Name, await ReadFileAsync(file)));
            }

            ApplyBulkImport(library.ImportMany(sources));
        });

    /// <summary>
    /// Import everything under a folder, including subfolders.
    /// </summary>
    /// <remarks>
    /// How a collection actually arrives: a directory tree, usually one folder
    /// per series, usually with readmes and cover art mixed in. Those are skipped
    /// silently rather than reported as failures.
    /// </remarks>
    private async void OnImportFolder(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            var picker = new FolderPicker();
            picker.FileTypeFilter.Add("*");
            InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(App.Window));

            var folder = await picker.PickSingleFolderAsync();
            if (folder is null)
            {
                return;
            }

            // Enumerated with the plain filesystem API rather than through the
            // StorageFolder tree: it recurses in one call and does not cost a
            // WinRT round trip per file, which matters at a few hundred of them.
            var paths = Directory
                .EnumerateFiles(folder.Path, "*", SearchOption.AllDirectories)
                .Where(path => IsImportable(Path.GetExtension(path)))
                .Take(AmiiboArchive.MaxEntries)
                .ToList();

            if (paths.Count == 0)
            {
                Report("No Amiibo dumps or archives were found in that folder.",
                       InfoBarSeverity.Informational);
                return;
            }

            var sources = new List<AmiiboImportSource>();
            foreach (var path in paths)
            {
                try
                {
                    sources.Add(new AmiiboImportSource(
                        Path.GetFileName(path), await File.ReadAllBytesAsync(path)));
                }
                catch (IOException error)
                {
                    adapters.Diagnostics.Warn(
                        "amiibo", $"{Path.GetFileName(path)} could not be read: {error.Message}");
                }
            }

            ApplyBulkImport(library.ImportMany(sources));
        });

    private static bool IsImportable(string extension) =>
        extension.Equals(".bin", StringComparison.OrdinalIgnoreCase) ||
        extension.Equals(".nfc", StringComparison.OrdinalIgnoreCase) ||
        extension.Equals(".zip", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Report a bulk import as one line, and select the result when it is one tag.
    /// </summary>
    /// <remarks>
    /// The detail goes to diagnostics rather than to a wall of InfoBars: someone
    /// importing four hundred tags wants to know it worked, not to dismiss forty
    /// notices about files that were never tags.
    /// </remarks>
    private void ApplyBulkImport(AmiiboBulkImportResult result)
    {
        foreach (var problem in result.Problems)
        {
            adapters.Diagnostics.Warn("amiibo", problem);
        }

        // Selecting the single new tag is helpful; selecting one arbitrary tag
        // out of four hundred is noise.
        if (result.Imported.Count == 1)
        {
            selectedId = result.Imported[0].Id;
        }

        Report(
            result.Problems.Count == 0
                ? result.Summary
                : $"{result.Summary} See Diagnostics for what was skipped.",
            result.Imported.Count > 0
                ? InfoBarSeverity.Success
                : InfoBarSeverity.Informational);

        Render();
    }

    private async void OnExportArchive(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (library.Items.Value.Count == 0)
            {
                Report("There is nothing in the library to export.", InfoBarSeverity.Informational);
                return;
            }

            var bytes = library.ExportArchive(View().LoadedFromLibrary?.Id);
            await SaveFileAsync("PicoSwitch2 Amiibo library", ".zip", "amiibo-library", bytes);
        });

    private async void OnExportOne(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (View().Selected is not { } item)
            {
                return;
            }

            await SaveFileAsync("Amiibo backup", ".bin", item.DisplayName, library.Bytes(item.Id));
        });

    private async void OnRename(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (View().Selected is not { } item)
            {
                return;
            }

            var name = await PromptAsync("Rename", "Only the name changes.", item.DisplayName);
            if (string.IsNullOrWhiteSpace(name))
            {
                return;
            }

            library.Rename(item.Id, name);
            Render();
        });

    /// <summary>
    /// Delete a backup.
    /// </summary>
    /// <remarks>
    /// The confirmation says plainly that this is the copy, because it may be:
    /// the physical figure has moved on, and there is no undo.
    /// </remarks>
    private async void OnDelete(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (View().Selected is not { } item)
            {
                return;
            }

            if (!await ConfirmAsync(
                    $"Delete '{item.DisplayName}'?",
                    "This removes your only copy of this backup. The adapter is not affected.",
                    "Delete"))
            {
                return;
            }

            library.Delete(item.Id);
            selectedId = null;
            Render();
        });

    private void OnLibrarySelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressSelection)
        {
            return;
        }

        selectedId = LibraryGrid.SelectedItem is AmiiboTile tile ? tile.Id : null;
        Render();
    }

    /// <summary>
    /// Wipe a backup's owner, nickname and game data, and re-sign it.
    /// </summary>
    /// <remarks>
    /// The portal's Initialize, brought over. What it is FOR: a tag someone else
    /// registered, or one whose save you want to start over, becomes a blank
    /// figure again without losing the figure itself.
    ///
    /// Rewrites the stored backup in place, which is why the confirmation says
    /// so plainly — there is no undo, and for a tag the physical figure has
    /// moved past, this library copy may be the only record of that save. Export
    /// first is the honest advice and the dialog gives it.
    ///
    /// The crypto refuses to re-sign an image whose HMAC does not verify, and
    /// checks its own output really is empty before returning it, so a failure
    /// here leaves the stored bytes untouched.
    /// </remarks>
    private async void OnInitialize(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (View().Selected is not { } item)
            {
                return;
            }

            var retail = keys.Read();
            if (retail is null)
            {
                Report("Import your amiibo keys before initializing a tag.",
                       InfoBarSeverity.Warning);
                return;
            }

            if (!await ConfirmAsync(
                    $"Initialize '{item.DisplayName}'?",
                    "The owner, nickname and any game data are erased and the backup is " +
                    "re-signed in place. The figure itself is unchanged. This cannot be " +
                    "undone — export a copy first if you might want this save back.",
                    "Initialize"))
            {
                return;
            }

            var initialized = AmiiboCrypto.Initialize(library.Bytes(item.Id), retail);
            library.UpdateFromAdapter(item.Id, initialized);

            Report($"'{item.DisplayName}' is now a blank figure.", InfoBarSeverity.Success);
            Render();
        });

    // -------------------------------------------------------------------- keys

    private async void OnImportKeys(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            var file = await PickFileAsync(".bin");
            if (file is null)
            {
                return;
            }

            try
            {
                keys.Import(await ReadFileAsync(file));
                Report("Amiibo keys imported.", InfoBarSeverity.Success);
            }
            catch (ArgumentException error)
            {
                // Refused at import rather than at first use, so the user is
                // pointed at the key file instead of at their tags.
                Report($"That is not a usable amiibo key file: {error.Message}",
                       InfoBarSeverity.Error);
            }

            Render();
        });

    private async void OnForgetKeys(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (!keys.Exists)
            {
                return;
            }

            if (!await ConfirmAsync(
                    "Forget the Amiibo keys?",
                    "Nicknames, owners and game data stop being readable until you import " +
                    "them again. Your backups are not affected.",
                    "Forget"))
            {
                return;
            }

            keys.Clear();
            Render();
        });

    // ------------------------------------------------------------------ render


    private AmiiboView View() => AmiiboView.From(
        adapters.Snapshot.Value,
        adapters.Connection.Value.Phase == ConnectionPhase.Connected,
        library.Items.Value,
        keys.Exists,
        selectedId,
        SelectedDetails(),
        transfer);

    /// <summary>
    /// Decrypt the selected backup's register block, when keys allow it.
    /// </summary>
    /// <remarks>
    /// Recomputed on render rather than cached: the keys can be imported or
    /// forgotten while this page is open, and a cached "no keys" result would
    /// leave the detail card empty until the user navigated away and back.
    /// </remarks>
    private AmiiboDetails? SelectedDetails()
    {
        if (selectedId is null)
        {
            return null;
        }

        try
        {
            return AmiiboCrypto.ReadDetails(library.Bytes(selectedId), keys.Read());
        }
        catch (Exception)
        {
            // A missing or unreadable image is already reported by the library's
            // own recovery pass; the detail card simply has nothing to show.
            return null;
        }
    }

    private void Render()
    {
        var view = View();

        SlotHeadline.Text = view.Available ? view.SlotHeadline : "Adapter not available";
        SlotDetail.Text = view.UnavailableReason ?? view.SlotDetail;

        DirtyBar.IsOpen = view.NeedsSync;

        KeyBar.IsOpen = view.KeyNotice is not null;
        KeyBar.Message = view.KeyNotice ?? "";
        KeyStatus.Text = keys.Exists
            ? $"Stored and protected for this Windows account. {keys.Fingerprint()}"
            : "Not imported. Identity still works without them.";
        ImportKeysButton.IsEnabled = true;
        ForgetKeysButton.IsEnabled = keys.Exists;

        var busy = transfer is not null;
        RefreshButton.IsEnabled = view.Connected && !busy;
        PresentButton.IsEnabled = view.CanPresent && !busy;
        EjectButton.IsEnabled = view.CanEject && !busy;
        SyncButton.IsEnabled = view.CanSync && !busy;
        ClearButton.IsEnabled = view.CanClear && !busy;
        SendButton.IsEnabled = view.CanUpload && !busy;

        CopySection.Visibility = view.CanChooseCopy ? Visibility.Visible : Visibility.Collapsed;
        CopyLabel.Text = view.CopyLabel;
        UseOriginalButton.IsEnabled = view.CanChooseCopy && view.Status.UsingSave2 && !busy;
        UseConsoleCopyButton.IsEnabled = view.CanChooseCopy && !view.Status.UsingSave2 && !busy;

        RenderTransfer();
        RenderLibrary(view);
        RenderDetails(view);
    }

    private void RenderTransfer()
    {
        if (transfer is not { } progress || progress.Total <= 0)
        {
            TransferProgress.Visibility = Visibility.Collapsed;
            TransferLabel.Visibility = Visibility.Collapsed;
            CancelButton.Visibility = Visibility.Collapsed;
            return;
        }

        TransferProgress.Visibility = Visibility.Visible;
        TransferProgress.Value = 100.0 * progress.Completed / progress.Total;
        TransferLabel.Visibility = Visibility.Visible;
        TransferLabel.Text = $"{progress.Completed} of {progress.Total} bytes";
        CancelButton.Visibility = Visibility.Visible;
    }

    private void RenderLibrary(AmiiboView view)
    {
        var cards = AmiiboGallery.Build(
            view.Library, catalog.Find, view.LoadedFromLibrary?.Id, filters);

        var tiles = cards
            .Select(card => new AmiiboTile(
                card.Id, card.Title, card.Subtitle, card.Badge, Artwork(card.ImageUrl)))
            .ToList();

        // Rebuilt wholesale, so the selection is restored by ID rather than by
        // object identity: the tiles are new instances every render, and matching
        // on the instance would silently drop the selection.
        suppressSelection = true;
        LibraryGrid.ItemsSource = tiles;
        LibraryGrid.SelectedItem = tiles.FirstOrDefault(tile => tile.Id == selectedId);
        suppressSelection = false;

        RenderFilterOptions(view);

        var total = view.Library.Count;
        LibrarySummary.Text = total == 0
            ? "No backups yet."
            : cards.Count == total
                ? $"{total} backup{(total == 1 ? "" : "s")}."
                : $"{cards.Count} of {total} shown.";

        // Distinguishes "you have nothing" from "your filter matched nothing",
        // which need completely different actions from the user.
        EmptyLibraryText.Visibility = tiles.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        EmptyLibraryText.Text = total == 0
            ? "Import a .bin dump, a folder of them, or a library ZIP — including one " +
              "exported from the phone app or another tool."
            : "Nothing matches. Clear the search and filters to see the whole library.";

        var hasSelection = view.Selected is not null;
        RenameButton.IsEnabled = hasSelection;
        DeleteButton.IsEnabled = hasSelection;
        ExportButton.IsEnabled = hasSelection;
        // Initializing rewrites the tag, so it needs the keys as well.
        InitializeButton.IsEnabled = hasSelection && keys.Exists;
        ExportArchiveButton.IsEnabled = total > 0;
        ClearFiltersButton.IsEnabled = filters.Any;
    }

    /// <summary>
    /// Populate the filter dropdowns from what the user actually owns.
    /// </summary>
    /// <remarks>
    /// A dropdown listing every amiibo series in existence when the library holds
    /// three figures is a worse control than one listing those three's series.
    /// Rebuilt on render, so a filter whose last match was just deleted does not
    /// linger as a choice that shows nothing.
    /// </remarks>
    private void RenderFilterOptions(AmiiboView view)
    {
        var options = AmiiboGallery.Options(view.Library, catalog.Find);
        suppressSelection = true;
        Fill(GameSeriesBox, options.GameSeries, filters.GameSeries);
        Fill(AmiiboSeriesBox, options.AmiiboSeries, filters.AmiiboSeries);
        Fill(TypeBox, options.Types, filters.Type);
        suppressSelection = false;

        static void Fill(ComboBox box, ValueList<string> values, string selected)
        {
            var items = new List<string> { AllFilter };
            items.AddRange(values);

            // Only rebuilt when the choices actually changed, so a render does
            // not fight a dropdown the user has open.
            if (box.ItemsSource is not List<string> existing || !existing.SequenceEqual(items))
            {
                box.ItemsSource = items;
            }

            box.SelectedItem = selected.Length == 0 ? AllFilter : selected;
        }
    }

    /// <summary>
    /// Resolve a catalog image URL, once per address.
    /// </summary>
    /// <remarks>
    /// Cached so a re-render — which happens on every adapter status change —
    /// does not re-request forty images. <see cref="BitmapImage"/> fetches
    /// asynchronously and stays blank on failure, which is right for decoration:
    /// the tile falls back to its placeholder glyph and nothing else is affected.
    ///
    /// This is the one thing on the page that contacts a host per FIGURE rather
    /// than once for the whole catalog, so it does reveal which figures are in
    /// the library to whoever serves the artwork. The Android companion has
    /// always done this and the artwork is most of the point of the catalog; it
    /// is called out here so the trade is visible rather than accidental.
    /// </remarks>
    private ImageSource? Artwork(string url)
    {
        if (url.Length == 0)
        {
            return null;
        }

        if (artwork.TryGetValue(url, out var cached))
        {
            return cached;
        }

        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) ||
            (uri.Scheme != Uri.UriSchemeHttps && uri.Scheme != Uri.UriSchemeHttp))
        {
            return null;
        }

        var image = new BitmapImage(uri) { DecodePixelWidth = 160 };
        artwork[url] = image;
        return image;
    }

    // Search, sort and filter handlers. Each one only records the change and
    // re-renders: all the matching and ordering rules live in AmiiboGallery,
    // where they are testable without a page.

    private void OnSearchChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args)
    {
        if (args.Reason != AutoSuggestionBoxTextChangeReason.UserInput)
        {
            return;
        }

        filters = filters with { Search = sender.Text ?? "" };
        Render();
    }

    private void OnSortChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressSelection || SortBox.SelectedIndex < 0)
        {
            return;
        }

        filters = filters with { Sort = (AmiiboSort)SortBox.SelectedIndex };
        Render();
    }

    private void OnToggleSortDirection(object sender, RoutedEventArgs e)
    {
        filters = filters with { Descending = !filters.Descending };
        Render();
    }

    private void OnFilterChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressSelection || sender is not ComboBox box)
        {
            return;
        }

        // "All" is a sentinel row rather than a real value, so it maps back to
        // the empty string the filter model uses for "unset".
        var value = box.SelectedItem as string ?? AllFilter;
        var chosen = value == AllFilter ? "" : value;

        filters = box.Name switch
        {
            nameof(GameSeriesBox) => filters with { GameSeries = chosen },
            nameof(AmiiboSeriesBox) => filters with { AmiiboSeries = chosen },
            _ => filters with { Type = chosen },
        };

        Render();
    }

    private void OnClearFilters(object sender, RoutedEventArgs e)
    {
        // Sort survives: it is a preference, not a narrowing, and resetting it
        // would be an unasked-for change.
        filters = new AmiiboGalleryFilters
        {
            Sort = filters.Sort,
            Descending = filters.Descending,
        };

        suppressSelection = true;
        SearchBox.Text = "";
        suppressSelection = false;
        Render();
    }

    private void RenderDetails(AmiiboView view)
    {
        DetailHost.Children.Clear();
        if (view.Selected is not { } item)
        {
            DetailCard.Visibility = Visibility.Collapsed;
            return;
        }

        DetailCard.Visibility = Visibility.Visible;
        AddDetail("Name", item.DisplayName);

        // Public catalog metadata, when it is known. Absent offline, and the
        // rest of the card is unaffected.
        var known = catalog.Find(item.FigureId);
        if (known is not null)
        {
            AddDetail("Figure", known.Name.Length > 0 ? known.Name : known.Character);
            AddDetail("Series", known.GameSeries);
            AddDetail("Collection", known.AmiiboSeries);
            AddDetail("Released", known.ReleaseDate);
        }

        AddDetail("Figure id", item.FigureId);
        AddDetail("Tag", item.TagType == AmiiboTagType.FigureV3 ? "Figure v3" : "NTAG215");
        AddDetail("UID", item.Uid);
        AddDetail("Size", $"{item.Size} bytes");

        var details = view.SelectedDetails;
        if (details is null || details.Crypto != AmiiboCryptoState.Valid)
        {
            AddDetail(
                "Contents",
                details?.Crypto == AmiiboCryptoState.Invalid
                    ? "Could not be decrypted with the imported keys"
                    : "Import your amiibo keys to read this");
            return;
        }

        AddDetail("Set up", details.SetUp ? "Yes" : "No");
        if (details.SetUp)
        {
            AddDetail("Nickname", details.Nickname);
            AddDetail("Owner", details.Owner);
        }

        if (details.SetupDate is { } setup)
        {
            AddDetail("Registered", setup);
        }

        if (details.LastWriteDate is { } written)
        {
            AddDetail("Last written", written);
        }

        // The catalog can name the game the tag's app data belongs to, which the
        // firmware's small built-in table cannot.
        var game = details.TitleId.Length > 0 ? catalog.GameForTitleId(details.TitleId) : null;
        AddDetail("Game data", game ?? details.AppDataLabel);
    }

    private void AddDetail(string label, string value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return;
        }

        var row = new Grid { ColumnSpacing = 12 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(140) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var name = new TextBlock
        {
            Text = label,
            Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
        };
        Grid.SetColumn(name, 0);
        row.Children.Add(name);

        var text = new TextBlock { Text = value, TextWrapping = TextWrapping.Wrap };
        Grid.SetColumn(text, 1);
        row.Children.Add(text);

        DetailHost.Children.Add(row);
    }

    private void ReportLibraryWarnings()
    {
        foreach (var warning in library.Warnings.Value)
        {
            adapters.Diagnostics.Warn("amiibo", warning);
        }

        if (library.Warnings.Value.Count > 0)
        {
            Report(library.Warnings.Value[0], InfoBarSeverity.Warning);
        }
    }

    // --------------------------------------------------------------- plumbing

    /// <summary>
    /// Run a transfer with progress and a working Cancel.
    /// </summary>
    /// <remarks>
    /// The progress callback arrives on a pool thread — no WinRT callback may
    /// touch XAML — so it hops to the dispatcher before it renders anything.
    /// </remarks>
    private async Task<T?> RunTransferAsync<T>(
        Func<CancellationToken, Task<T>> operation,
        string message,
        string? completion)
        where T : class
    {
        using var cancellation = new CancellationTokenSource();
        transferCancellation = cancellation;
        transfer = (0, 1);
        Report(message, InfoBarSeverity.Informational);
        Render();

        try
        {
            return await operation(cancellation.Token);
        }
        catch (OperationCanceledException)
        {
            Report("Cancelled.", InfoBarSeverity.Informational);
            return null;
        }
        catch (Exception error)
        {
            Report(ManagementErrorText.Summarize(error), InfoBarSeverity.Error);
            adapters.Diagnostics.Error("amiibo", $"{error.GetType().Name}: {error.Message}");
            return null;
        }
        finally
        {
            transferCancellation = null;
            transfer = null;
            Render();

            if (completion is not null)
            {
                Report(completion, InfoBarSeverity.Success);
            }
        }
    }

    private void OnTransferProgress(int completed, int total) =>
        AppServices.OnUiThread(() =>
        {
            transfer = (completed, total);
            RenderTransfer();
        });

    private async Task<StorageFile?> PickFileAsync(params string[] extensions)
    {
        var picker = new FileOpenPicker();
        foreach (var extension in extensions)
        {
            picker.FileTypeFilter.Add(extension);
        }

        // WinUI 3 pickers are window-owned and throw without a parent handle.
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(App.Window));
        return await picker.PickSingleFileAsync();
    }

    private async Task<IReadOnlyList<StorageFile>> PickFilesAsync(params string[] extensions)
    {
        var picker = new FileOpenPicker();
        foreach (var extension in extensions)
        {
            picker.FileTypeFilter.Add(extension);
        }

        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(App.Window));
        return await picker.PickMultipleFilesAsync() ?? [];
    }

    private async Task SaveFileAsync(
        string description, string extension, string suggestedName, byte[] bytes)
    {
        var picker = new FileSavePicker { SuggestedFileName = Sanitize(suggestedName) };
        picker.FileTypeChoices.Add(description, [extension]);
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(App.Window));

        var file = await picker.PickSaveFileAsync();
        if (file is null)
        {
            return;
        }

        await FileIO.WriteBytesAsync(file, bytes);
        Report($"Saved to {file.Name}.", InfoBarSeverity.Success);
    }

    private static string Sanitize(string name)
    {
        var invalid = Path.GetInvalidFileNameChars();
        var cleaned = new string(name.Select(c => invalid.Contains(c) ? '_' : c).ToArray()).Trim();
        return cleaned.Length == 0 ? "amiibo" : cleaned;
    }

    private static async Task<byte[]> ReadFileAsync(StorageFile file)
    {
        var buffer = await FileIO.ReadBufferAsync(file);
        var bytes = new byte[buffer.Length];
        // Globally qualified: `Windows` resolves to PicoSwitch.Companion.Windows
        // inside this file, which is exactly the kind of collision that produces
        // a confusing error a long way from its cause.
        using var reader = global::Windows.Storage.Streams.DataReader.FromBuffer(buffer);
        reader.ReadBytes(bytes);
        return bytes;
    }

    /// <summary>
    /// THE exception boundary for every `async void` handler on this page.
    /// </summary>
    /// <remarks>
    /// An exception escaping an `async void` UI handler has no handler at all: it
    /// is rethrown on the dispatcher and terminates the process. The Keyboard and
    /// Mouse page shipped exactly that crash (WER 0xc000027b), and this page has
    /// the same shape, so it gets the same boundary from the start rather than
    /// after a bug report.
    ///
    /// This does not swallow: the failure is logged, shown, and the page is
    /// repainted into a coherent state.
    /// </remarks>
    private async Task GuardAsync(Func<Task> handler)
    {
        try
        {
            await handler();
        }
        catch (Exception error)
        {
            var message = ManagementErrorText.Summarize(error);
            adapters.Diagnostics.Error("ui", $"{message} ({error.GetType().Name})");
            Report(message, InfoBarSeverity.Error);
            Render();
        }
    }

    /// <summary>Run an adapter call, reporting failure rather than throwing.</summary>
    private async Task SafeAsync(Func<Task> operation)
    {
        try
        {
            await operation();
        }
        catch (Exception error)
        {
            Report(ManagementErrorText.Summarize(error), InfoBarSeverity.Error);
            adapters.Diagnostics.Error("amiibo", $"{error.GetType().Name}: {error.Message}");
        }

        Render();
    }

    /// <summary>
    /// Show one dialog at a time; WinUI throws on a second concurrent one, and
    /// from an `async void` handler that throw is a process kill.
    /// </summary>
    private async Task<ContentDialogResult> ShowDialogAsync(ContentDialog dialog)
    {
        if (dialogOpen)
        {
            return ContentDialogResult.None;
        }

        dialogOpen = true;
        try
        {
            dialog.XamlRoot = XamlRoot;
            return await dialog.ShowAsync();
        }
        finally
        {
            dialogOpen = false;
        }
    }

    private async Task<bool> ConfirmAsync(string title, string content, string action) =>
        await ShowDialogAsync(new ContentDialog
        {
            Title = title,
            Content = content,
            PrimaryButtonText = action,
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        }) == ContentDialogResult.Primary;

    private async Task<string?> PromptAsync(string title, string description, string initial)
    {
        var box = new TextBox
        {
            Text = initial,
            MaxLength = AmiiboArchive.MaxNameChars,
            SelectionStart = initial.Length,
        };

        var content = new StackPanel { Spacing = 8 };
        content.Children.Add(new TextBlock { Text = description, TextWrapping = TextWrapping.Wrap });
        content.Children.Add(box);

        var dialog = new ContentDialog
        {
            Title = title,
            Content = content,
            PrimaryButtonText = "OK",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        return await ShowDialogAsync(dialog) == ContentDialogResult.Primary
            ? box.Text.Trim()
            : null;
    }

    private void Report(string message, InfoBarSeverity severity)
    {
        OperationBar.Severity = severity;
        OperationBar.Message = message;
        OperationBar.IsOpen = true;
    }
}
