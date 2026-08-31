using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
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
/// One row of the Amiibo library list.
/// </summary>
/// <remarks>
/// Top-level and public because the XAML item template binds to it: the markup
/// compiler cannot reach a nested type through <c>x:DataType</c>, and generates
/// code that does not compile if asked to.
///
/// Two fields because the row needs two lines. The detail carries the tag
/// family, the figure id, and whether this is the backup the adapter is holding
/// — that last one being what someone scanning the list is usually looking for.
/// </remarks>
public sealed record AmiiboLibraryRow(string Id, string Title, string Detail);

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

    private string? selectedId;
    private bool suppressSelection;
    private bool dialogOpen;
    private (int Completed, int Total)? transfer;
    private CancellationTokenSource? transferCancellation;

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

            if (adapters.Connection.Value.Phase == ConnectionPhase.Connected)
            {
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

    private void OnStateChanged() => AppServices.OnUiThread(Render);

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

    private async void OnImport(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            var file = await PickFileAsync(".bin", ".nfc");
            if (file is null)
            {
                return;
            }

            var bytes = await ReadFileAsync(file);
            var result = library.Import(
                Path.GetFileNameWithoutExtension(file.Name), file.Name, bytes);

            selectedId = result.Item.Id;
            Report(
                result.Duplicate
                    ? $"'{result.Item.DisplayName}' is already in your library."
                    : $"'{result.Item.DisplayName}' added to your library.",
                InfoBarSeverity.Success);
            Render();
        });

    private async void OnImportArchive(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            var file = await PickFileAsync(".zip");
            if (file is null)
            {
                return;
            }

            var result = library.ImportArchive(await ReadFileAsync(file));
            var parts = new List<string>();
            if (result.Imported.Count > 0)
            {
                parts.Add($"{result.Imported.Count} added");
            }

            if (result.Duplicates > 0)
            {
                parts.Add($"{result.Duplicates} already present");
            }

            Report(
                parts.Count == 0 ? "Nothing to import." : string.Join(", ", parts) + ".",
                result.Warnings.Count > 0 ? InfoBarSeverity.Warning : InfoBarSeverity.Success);

            foreach (var warning in result.Warnings)
            {
                adapters.Diagnostics.Warn("amiibo", warning);
            }

            Render();
        });

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

        selectedId = LibraryList.SelectedItem is AmiiboLibraryRow row ? row.Id : null;
        Render();
    }

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
        var loadedId = view.LoadedFromLibrary?.Id;
        var rows = view.Library
            .OrderBy(item => item.DisplayName, StringComparer.CurrentCultureIgnoreCase)
            .Select(item => new AmiiboLibraryRow(
                item.Id,
                item.DisplayName,
                Describe(item, item.Id == loadedId)))
            .ToList();

        // Rebuilt wholesale, so the selection is restored by ID rather than by
        // object identity: the rows are new instances every render, and matching
        // on the instance would silently drop the selection.
        suppressSelection = true;
        LibraryList.ItemsSource = rows;
        LibraryList.SelectedItem = rows.FirstOrDefault(row => row.Id == selectedId);
        suppressSelection = false;

        LibrarySummary.Text = rows.Count == 0
            ? "No backups yet. Import a .bin dump, or a library backup exported from the phone app."
            : $"{rows.Count} backup{(rows.Count == 1 ? "" : "s")}.";

        var hasSelection = view.Selected is not null;
        RenameButton.IsEnabled = hasSelection;
        DeleteButton.IsEnabled = hasSelection;
        ExportButton.IsEnabled = hasSelection;
        ExportArchiveButton.IsEnabled = rows.Count > 0;
    }

    private static string Describe(AmiiboLibraryItem item, bool onAdapter)
    {
        var parts = new List<string>
        {
            item.TagType == AmiiboTagType.FigureV3 ? "Figure v3" : "NTAG215",
            item.FigureId,
        };

        if (onAdapter)
        {
            parts.Add("on the adapter");
        }

        if (item.DirtyFromAdapter)
        {
            parts.Add("changed on the adapter");
        }

        return string.Join(" · ", parts);
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
        AddDetail("Figure", item.FigureId);
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

        AddDetail("Game data", details.AppDataLabel);
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
