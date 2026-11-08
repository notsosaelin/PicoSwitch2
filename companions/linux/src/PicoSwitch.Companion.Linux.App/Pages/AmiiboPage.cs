using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Shapes;
using Avalonia.Controls.Templates;
using Avalonia.Data;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Path = System.IO.Path;
using PicoSwitch.Companion.Linux.App.Shell;
using PicoSwitch.Companion.Linux.Storage;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.App.Pages;

/// <summary>
/// Amiibo: the user's collection, and the tag the adapter is holding. A port of
/// the WinUI AmiiboPage; that file carries the reasoning for each rule, and the
/// comments here are the ones a maintainer of THIS file needs.
/// </summary>
/// <remarks>
/// BROWSE, THEN INSPECT. The library is the surface; the inspector opens on
/// request (double click or Details), beside the browser when the window is
/// wide enough and over it when it is not.
///
/// TWO STORES. The library is the user's and works with nothing paired; only
/// Send, Present, Eject, Sync and Clear reach the adapter.
///
/// THE RULE THIS PAGE ENFORCES: a console write lives ONLY on the adapter until
/// synced, so sending another tag over it or clearing it is refused until then.
///
/// Rules live in <see cref="AmiiboView"/>, <see cref="AmiiboGallery"/>,
/// <see cref="AmiiboInspection"/> and <see cref="AmiiboInteraction"/>, shared
/// with Windows and Android; this file renders state and raises events.
/// </remarks>
internal sealed class AmiiboPage : CompanionPage
{
    /// <summary>Below this browser width the inspector overlays instead of sitting beside.</summary>
    private const double SideBySideMinimumWidth = 820;

    private const double InspectorWidth = 380;

    /// <summary>The "no filter" row of each filter. Named, so an unset filter still says what it filters.</summary>
    private const string AllGameSeries = "All game series";
    private const string AllCollections = "All Amiibo series";
    private const string AllTypes = "All types";

    private readonly AdapterConnectionService adapters = AppServices.Adapters;
    private readonly AmiiboLibrary library = AppServices.AmiiboLibrary;
    private readonly LinuxAmiiboKeyStore keys = AppServices.AmiiboKeys;
    private readonly AmiiboCatalog catalog = AppServices.AmiiboCatalog;

    private readonly TextBlock librarySummary = Ui.Text(secondary: true);
    private readonly MenuItem exportArchiveButton;
    private readonly Button keysButton;
    private readonly TextBlock keyStatus = Ui.Caption();
    private readonly Button forgetKeysButton;
    private readonly Notice dirtyBar = new(dismissible: false);
    private readonly Notice keyBar = new(dismissible: false);

    private readonly TextBox searchBox = new() { Watermark = "Search name, character, series or ID", MinWidth = 200 };
    private readonly ComboBox sortBox = new() { MinWidth = 160 };
    private readonly ComboBox viewBox = new() { MinWidth = 140 };
    private readonly ComboBox gameSeriesBox = new() { HorizontalAlignment = HorizontalAlignment.Stretch, PlaceholderText = "Game series" };
    private readonly ComboBox amiiboSeriesBox = new() { HorizontalAlignment = HorizontalAlignment.Stretch, PlaceholderText = "Amiibo series" };
    private readonly ComboBox typeBox = new() { HorizontalAlignment = HorizontalAlignment.Stretch, PlaceholderText = "Type" };
    private readonly Button clearFiltersButton;
    private readonly Button detailsButton;
    private readonly Button selectButton;

    private readonly Border selectionBar;
    private readonly TextBlock selectionCount = Ui.Text();

    private readonly Grid contentArea = new() { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
    private readonly ListBox browser = new() { SelectionMode = SelectionMode.Single, Background = Brushes.Transparent };
    private readonly TextBlock noMatchText = Ui.Text("Nothing matches. Clear the search and filters to see the whole library.", secondary: true);
    private readonly Border query;
    private readonly Border browserCard;
    private readonly Border emptyLibrary;

    private readonly Border inspector;
    private readonly StackPanel selectionEmpty;
    private readonly Grid heroPanel = new() { ColumnDefinitions = new ColumnDefinitions("Auto,*"), ColumnSpacing = 12 };
    private readonly Image selectedArtwork = new() { Width = 64, Height = 64, Stretch = Stretch.Uniform };
    private readonly PathIcon selectedPlaceholder = Ui.Icon(Icons.Image, 28);
    private readonly TextBlock selectedTitle = Ui.Heading();
    private readonly TextBlock selectedSubtitle = Ui.Caption();
    private readonly TextBlock selectedAdapterState = Ui.Caption();
    private readonly Button sendButton;
    private readonly TextBlock sendReason = Ui.Caption();
    private readonly StackPanel actionPanel;
    private readonly Button renameButton;
    private readonly Button exportButton;
    private readonly MenuItem initializeItem = new() { Header = "Initialize…" };
    private readonly ToggleButton overviewTab = new() { Content = "Overview" };
    private readonly ToggleButton tagTab = new() { Content = "Tag" };
    private readonly ToggleButton adapterTab = new() { Content = "Adapter" };
    private readonly StackPanel categoryBar;
    private readonly StackPanel detailHost = new() { Spacing = 2 };
    private readonly StackPanel adapterPanel = new() { Spacing = 10 };
    private readonly TextBlock slotHeadline = Ui.Text();
    private readonly TextBlock slotDetail = Ui.Caption();
    private readonly ProgressBar transferProgress = new() { Minimum = 0, Maximum = 100, IsVisible = false };
    private readonly TextBlock transferLabel = Ui.Caption();
    private readonly StackPanel adapterControls = new() { Spacing = 8 };
    private readonly Button presentButton;
    private readonly Button ejectButton;
    private readonly Button cancelButton;
    private readonly Button syncButton;
    private readonly Button clearButton;
    private readonly StackPanel copySection = new() { Spacing = 6 };
    private readonly TextBlock copyLabel = Ui.Caption();
    private readonly Button useOriginalButton;
    private readonly Button useConsoleCopyButton;
    private readonly Button refreshButton;

    /// <summary>Focus, inspection and bulk selection: three ideas, one shared state machine.</summary>
    private AmiiboInteractionState interaction = new();

    private AmiiboGalleryFilters filters = new();
    private IReadOnlyList<AmiiboTile> tiles = [];
    private string browserSignature = "<unset>";
    private InspectorCategory category = InspectorCategory.Overview;
    private bool suppress;
    private bool amiiboRead;
    private bool pointerDown;
    private bool sideBySide = true;
    private (int Completed, int Total)? transfer;
    private CancellationTokenSource? transferCancellation;

    public AmiiboPage(MainWindow shell)
        : base(shell)
    {
        exportArchiveButton = new MenuItem { Header = "Export all…" };
        exportArchiveButton.Click += async (_, _) => await GuardAsync(OnExportArchiveAsync);
        forgetKeysButton = Ui.Button("Forget", async (_, _) => await GuardAsync(OnForgetKeysAsync), "Forget the Amiibo keys");
        // Icon-only and square at the control height, as on Windows: the keys
        // are set once, so they do not earn a labelled button in the header.
        keysButton = new Button
        {
            Content = Ui.Icon(Icons.Key, 16),
            Width = Sizes.ControlHeight,
            Height = Sizes.ControlHeight,
            MinWidth = Sizes.ControlHeight,
            Padding = new Thickness(0),
            Flyout = new Flyout
            {
                Placement = PlacementMode.BottomEdgeAlignedRight,
                Content = new StackPanel
                {
                    Spacing = 10,
                    Width = 300,
                    Children =
                    {
                        Ui.Heading("Amiibo keys"),
                        keyStatus,
                        Ui.Row(Ui.Button("Import keys", async (_, _) => await GuardAsync(OnImportKeysAsync)), forgetKeysButton),
                    },
                },
            },
        };
        AutomationProperties.SetName(keysButton, "Amiibo keys");
        ToolTip.SetTip(keysButton, "Amiibo keys");

        clearFiltersButton = Ui.Button("Clear filters", (_, _) => OnClearFilters());
        detailsButton = Ui.Button("Details", (_, _) => OnShowDetails(), "Open details for the focused Amiibo");
        selectButton = Ui.Button("Select", (_, _) => OnStartSelection(), "Start selecting Amiibo");

        sortBox.ItemsSource = new[] { "Recently added", "Name", "Figure number", "Release date" };
        viewBox.ItemsSource = new[] { "Grid", "Carousel", "List" };
        sortBox.SelectionChanged += (_, _) => OnSortChanged();
        viewBox.SelectionChanged += (_, _) => OnViewModeChanged();
        searchBox.InnerLeftContent = new Border { Padding = new Thickness(10, 0, 0, 0), Child = Ui.Icon(Icons.Search, 16).Bind(PathIcon.ForegroundProperty, Tokens.TextSecondary) };
        searchBox.TextChanged += (_, _) =>
        {
            if (!suppress)
            {
                filters = filters with { Search = searchBox.Text ?? string.Empty };
                Render();
            }
        };
        foreach (var box in new[] { gameSeriesBox, amiiboSeriesBox, typeBox })
        {
            box.SelectionChanged += (sender, _) => OnFilterChanged((ComboBox)sender!);
        }

        var sortDirection = Ui.IconButton(Icons.SortDirection, "Reverse the order", (_, _) =>
        {
            filters = filters with { Descending = !filters.Descending };
            Render();
        });

        var bulkInitialize = Ui.Button("Initialize…", async (_, _) => await GuardAsync(OnBulkInitializeAsync), "Initialize the selected Amiibo");
        var bulkDelete = Ui.Button("Delete…", async (_, _) => await GuardAsync(OnBulkDeleteAsync), "Delete the selected Amiibo");
        var cancelSelection = Ui.IconButton(Icons.Close, "Exit selection mode", (_, _) => SetInteraction(AmiiboInteraction.ClearSelection(interaction)));
        selectionBar = new Border
        {
            BorderThickness = new Thickness(3, 0, 0, 0),
            CornerRadius = new CornerRadius(8),
            Padding = new Thickness(14, 6),
            IsVisible = false,
            Child = new SettingRow(selectionCount, Ui.Row(bulkInitialize, bulkDelete, cancelSelection), minimumLabelWidth: 160),
        }.Bind(Border.BackgroundProperty, Tokens.Card).Bind(Border.BorderBrushProperty, Tokens.Accent);

        // The query controls: search with sort and view beside it, then the
        // filters on their own row. Search keeps the room; sort and view move
        // under it when the window is narrow.
        var filterGrid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,*,*,Auto"), ColumnSpacing = 8 };
        filterGrid.Children.Add(gameSeriesBox);
        Grid.SetColumn(amiiboSeriesBox, 1);
        filterGrid.Children.Add(amiiboSeriesBox);
        Grid.SetColumn(typeBox, 2);
        filterGrid.Children.Add(typeBox);
        Grid.SetColumn(clearFiltersButton, 3);
        filterGrid.Children.Add(clearFiltersButton);
        query = Ui.Card(Ui.Stack(
            10,
            new SettingRow(searchBox, Ui.Row(sortBox, sortDirection, viewBox), minimumLabelWidth: 260),
            filterGrid));
        query.Padding = new Thickness(14, 12);

        browser.AddHandler(TappedEvent, OnBrowserTapped, RoutingStrategies.Bubble, handledEventsToo: true);
        browser.AddHandler(DoubleTappedEvent, OnBrowserDoubleTapped, RoutingStrategies.Bubble, handledEventsToo: true);
        browser.AddHandler(PointerPressedEvent, (_, _) => pointerDown = true, RoutingStrategies.Tunnel, handledEventsToo: true);
        browser.AddHandler(PointerCaptureLostEvent, (_, _) => pointerDown = false, RoutingStrategies.Bubble, handledEventsToo: true);
        browser.SelectionChanged += (_, _) => OnBrowserSelectionChanged();

        noMatchText.TextAlignment = TextAlignment.Center;
        noMatchText.MaxWidth = 440;
        noMatchText.HorizontalAlignment = HorizontalAlignment.Center;
        noMatchText.VerticalAlignment = VerticalAlignment.Center;
        browserCard = Ui.Card(new Panel { Children = { browser, noMatchText } });
        browserCard.Padding = new Thickness(6);

        // Small, not zero: on a short window the browser shrinks rather than
        // overlapping the query controls above it.
        browserCard.MinHeight = 160;
        contentArea.Children.Add(browserCard);

        // "You have nothing" gets the whole surface and the actions that fix it;
        // the query controls have nothing to query, so they go.
        emptyLibrary = Ui.EmptyState(
            Icons.Amiibo,
            Ui.Text("Your Amiibo library is empty"),
            Ui.Text("Import a .bin or .nfc dump, a folder of them, or a library ZIP, including one exported from the phone app or another tool. Your library works with no adapter paired.", secondary: true),
            Ui.Button("Import…", async (_, _) => await GuardAsync(OnImportAsync), accent: true, icon: Icons.Plus),
            Ui.Button("Import folder…", async (_, _) => await GuardAsync(OnImportFolderAsync), icon: Icons.Folder));
        emptyLibrary.VerticalAlignment = VerticalAlignment.Top;
        emptyLibrary.IsVisible = false;
        contentArea.Children.Add(emptyLibrary);
        Grid.SetColumnSpan(emptyLibrary, 2);

        /* the inspector */

        sendButton = Ui.Button("Send to adapter", async (_, _) => await GuardAsync(OnUploadAsync), accent: true, icon: Icons.Upload);
        sendButton.HorizontalAlignment = HorizontalAlignment.Stretch;
        renameButton = Ui.Button("Rename", async (_, _) => await GuardAsync(OnRenameAsync));
        exportButton = Ui.Button("Export", async (_, _) => await GuardAsync(OnExportOneAsync));
        initializeItem.Click += async (_, _) => await GuardAsync(OnInitializeAsync);
        var deleteItem = new MenuItem { Header = "Delete…" };
        deleteItem.Click += async (_, _) => await GuardAsync(OnDeleteAsync);
        var more = new Button { Content = "More", Flyout = new MenuFlyout { Items = { initializeItem, new Separator(), deleteItem } } };
        actionPanel = Ui.Stack(8, sendButton, sendReason, Ui.Row(renameButton, exportButton, more));

        overviewTab.Click += (_, _) => OnCategory(InspectorCategory.Overview);
        tagTab.Click += (_, _) => OnCategory(InspectorCategory.Tag);
        adapterTab.Click += (_, _) => OnCategory(InspectorCategory.Adapter);
        categoryBar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Children = { overviewTab, tagTab, adapterTab } };

        presentButton = Ui.Button("Present", async (_, _) => await GuardAsync(() => RunAsync(() => adapters.SetAmiiboPresentedAsync(true))));
        ejectButton = Ui.Button("Eject", async (_, _) => await GuardAsync(() => RunAsync(() => adapters.SetAmiiboPresentedAsync(false))));
        cancelButton = Ui.Button("Cancel", (_, _) => transferCancellation?.Cancel());
        syncButton = Ui.Button("Sync to library", async (_, _) => await GuardAsync(OnSyncAsync));
        clearButton = Ui.Button("Clear", async (_, _) => await GuardAsync(OnClearAsync));
        useOriginalButton = Ui.Button("Use original", async (_, _) => await GuardAsync(() => RunAsync(() => adapters.SelectAmiiboCopyAsync(false))));
        useConsoleCopyButton = Ui.Button("Console copy", async (_, _) => await GuardAsync(() => RunAsync(() => adapters.SelectAmiiboCopyAsync(true))));
        refreshButton = Ui.Button("Reload adapter state", async (_, _) => await GuardAsync(() => RunAsync(() => adapters.RefreshAmiiboAsync())), icon: Icons.Refresh);
        refreshButton.HorizontalAlignment = HorizontalAlignment.Stretch;
        copySection.Children.AddRange([copyLabel, Ui.Row(useOriginalButton, useConsoleCopyButton)]);
        adapterControls.Children.AddRange([Ui.Row(presentButton, ejectButton, cancelButton), Ui.Row(syncButton, clearButton), copySection]);
        adapterPanel.Children.AddRange([slotHeadline, slotDetail, transferProgress, transferLabel, adapterControls, refreshButton]);

        var heroText = Ui.Stack(2, selectedTitle, selectedSubtitle, selectedAdapterState);
        heroText.VerticalAlignment = VerticalAlignment.Center;
        var art = new Panel { Width = 64, Height = 64, Children = { selectedPlaceholder, selectedArtwork } };
        selectedPlaceholder.Bind(PathIcon.ForegroundProperty, Tokens.TextTertiary);
        heroPanel.Children.Add(art);
        Grid.SetColumn(heroText, 1);
        heroPanel.Children.Add(heroText);

        selectionEmpty = Ui.Stack(4, Ui.Text("Nothing selected"), Ui.Caption("Pick an Amiibo to see its details and what you can do with it."));
        var close = Ui.IconButton(Icons.Close, "Close details", (_, _) =>
        {
            SetInteraction(AmiiboInteraction.CloseInspector(interaction));
            FocusFocusedTile();
        });
        close.HorizontalAlignment = HorizontalAlignment.Right;
        close.Background = Brushes.Transparent;

        inspector = Ui.Card(new DockPanel
        {
            Margin = new Thickness(16, 12, 16, 16),
            Children =
            {
                Top(Ui.Stack(12, close, selectionEmpty, heroPanel, actionPanel, categoryBar)),
                new ScrollViewer { Margin = new Thickness(0, 12, 0, 0), Content = Ui.Stack(10, detailHost, adapterPanel) },
            },
        });
        inspector.Width = InspectorWidth;
        inspector.IsVisible = false;
        inspector.HorizontalAlignment = HorizontalAlignment.Right;
        Grid.SetColumn(inspector, 1);
        contentArea.Children.Add(inspector);
        contentArea.SizeChanged += (_, e) =>
        {
            var wide = e.NewSize.Width >= SideBySideMinimumWidth;
            if (wide != sideBySide)
            {
                sideBySide = wide;
                ApplyLayout();
            }
        };

        dirtyBar.Show(
            "The console changed this Amiibo",
            "The change exists only on the adapter. Sync it to your library before sending another tag or clearing this one.",
            NoticeSeverity.Warning);

        // Not a scrolling page: the browser scrolls inside its card, so the
        // query controls and the inspector stay put.
        // The header matches Windows: Details and Select act on the grid; the
        // library's own file commands sit together behind one Library menu.
        var importItem = new MenuItem { Header = "Import…" };
        importItem.Click += async (_, _) => await GuardAsync(OnImportAsync);
        var importFolderItem = new MenuItem { Header = "Import folder…" };
        importFolderItem.Click += async (_, _) => await GuardAsync(OnImportFolderAsync);
        var libraryMenu = new Button
        {
            Content = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 8,
                Children = { new TextBlock { Text = "Library", VerticalAlignment = VerticalAlignment.Center }, Ui.Icon(Icons.ChevronDown, 14) },
            },
            Flyout = new MenuFlyout
            {
                Placement = PlacementMode.BottomEdgeAlignedRight,
                Items = { importItem, importFolderItem, exportArchiveButton },
            },
        };
        AutomationProperties.SetName(libraryMenu, "Library");

        var header = Ui.PageHeader(
            Ui.Title("Amiibo"),
            librarySummary,
            detailsButton,
            selectButton,
            libraryMenu,
            keysButton);
        var column = new DockPanel { MaxWidth = 1400 };
        foreach (var top in new Control[] { header, Operation, dirtyBar, keyBar, query, selectionBar })
        {
            column.Children.Add(Top(top, 12));
        }

        column.Children.Add(contentArea);
        Content = new PageFill(column);
        KeyDown += OnPageKeyDown;
    }

    private enum InspectorCategory
    {
        Overview,
        Tag,
        Adapter,
    }

    protected override async void OnShown()
    {
        await GuardAsync(async () =>
        {
            filters = filters with { View = AppServices.AmiiboViewMode };
            adapters.Snapshot.Changed += OnStateChanged;
            adapters.Connection.Changed += OnStateChanged;
            library.Items.Changed += OnStateChanged;

            suppress = true;
            sortBox.SelectedIndex = (int)filters.Sort;
            viewBox.SelectedIndex = (int)filters.View;
            suppress = false;

            ApplyLayout();
            Render();
            ReportLibraryWarnings();

            // Enrichment: the page is fully usable before the catalog answers.
            _ = catalog.EnsureLoadedAsync().ContinueWith(_ => AppServices.OnUiThread(Render), TaskScheduler.Default);

            if (adapters.Connection.Value.Connected)
            {
                amiiboRead = true;
                await RunAsync(() => adapters.RefreshAmiiboAsync());
            }
        });
    }

    protected override void OnHidden()
    {
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Connection.Changed -= OnStateChanged;
        library.Items.Changed -= OnStateChanged;

        // A transfer outlives the page otherwise.
        transferCancellation?.Cancel();
    }

    /// <summary>Repaint, and read an adapter that connected after the page opened.</summary>
    private void OnStateChanged() => AppServices.OnUiThread(() =>
    {
        Render();
        var connected = adapters.Connection.Value.Connected;
        if (connected && !amiiboRead && transfer is null)
        {
            amiiboRead = true;
            _ = GuardAsync(() => RunAsync(() => adapters.RefreshAmiiboAsync()));
        }
        else if (!connected)
        {
            amiiboRead = false;
        }
    });

    /* ------------------------------------------------------------ rendering */

    private void Render()
    {
        var view = View();
        RenderAdapter(view);
        RenderBrowser(view);
        RenderSelection(view);
    }

    private AmiiboView View() => AmiiboView.From(
        adapters.Snapshot.Value,
        adapters.Connection.Value.Connected,
        library.Items.Value,
        keys.Exists,
        interaction.FocusedId,
        transfer: transfer);

    private void RenderAdapter(AmiiboView view)
    {
        slotHeadline.Text = view.Available ? view.SlotHeadline : "Not connected";
        slotDetail.Text = view.UnavailableReason ?? view.SlotDetail;
        dirtyBar.IsVisible = view.NeedsSync;
        if (view.KeyNotice is { } notice)
        {
            keyBar.Show(null, notice, NoticeSeverity.Informational);
        }
        else
        {
            keyBar.Hide();
        }

        keyStatus.Text = keys.Exists
            ? "Stored for this user account only (owner-readable file)."
            : "Not imported. Names, series and artwork still work without them.";
        forgetKeysButton.IsEnabled = keys.Exists;

        var busy = transfer is not null;
        refreshButton.IsEnabled = view.Connected && !busy;

        // The deeper adapter controls appear only when they mean something.
        adapterControls.IsVisible = view.Available;
        presentButton.IsEnabled = view.CanPresent && !busy;
        ejectButton.IsEnabled = view.CanEject && !busy;
        syncButton.IsEnabled = view.CanSync && !busy;
        clearButton.IsEnabled = view.CanClear && !busy;
        copySection.IsVisible = view.CanChooseCopy;
        copyLabel.Text = view.CopyLabel;
        useOriginalButton.IsEnabled = view.CanChooseCopy && view.Status.UsingSave2 && !busy;
        useConsoleCopyButton.IsEnabled = view.CanChooseCopy && !view.Status.UsingSave2 && !busy;
        RenderTransfer();
    }

    private void RenderTransfer()
    {
        var active = transfer is { Total: > 0 };
        transferProgress.IsVisible = active;
        transferLabel.IsVisible = active;
        cancelButton.IsVisible = active;
        if (transfer is { Total: > 0 } progress)
        {
            transferProgress.Value = 100.0 * progress.Completed / progress.Total;
            transferLabel.Text = $"{progress.Completed} of {progress.Total} bytes";
        }
    }

    /// <summary>
    /// The browser's collection is replaced ONLY when the projected result
    /// changed (identity, badge and the catalog fields shown), so status ticks
    /// and selection never reset the scroll position.
    /// </summary>
    private void RenderBrowser(AmiiboView view)
    {
        var cards = AmiiboGallery.Build(view.Library, catalog.Find, view.LoadedFromLibrary?.Id, filters);
        var signature = string.Join("|", cards.Select(card => $"{card.Id}:{card.Badge}:{card.Title}:{card.ImageUrl}"));
        if (signature != browserSignature)
        {
            browserSignature = signature;
            tiles = [.. cards.Select(card => new AmiiboTile(
                card.Id, card.Title, card.Subtitle, card.Badge, card.ImageUrl,
                series: card.GameSeries, collection: card.AmiiboSeries, released: card.ReleaseDate, figureId: card.FigureId))];
            suppress = true;
            browser.ItemsSource = tiles;
            suppress = false;
            RestoreSelection();

            // Fresh tiles are unticked; the set itself was never lost.
            RenderTicks();
        }

        RenderViewMode();
        RenderFilterOptions(view);

        var total = view.Library.Count;
        librarySummary.Text = total == 0 ? "No Amiibo yet" : cards.Count == total ? $"{total} Amiibo" : $"{cards.Count} of {total} Amiibo";

        // "You have nothing" and "your filter matched nothing" need different actions.
        emptyLibrary.IsVisible = total == 0;
        query.IsVisible = total > 0;
        browserCard.IsVisible = total > 0;
        noMatchText.IsVisible = total > 0 && tiles.Count == 0;
        exportArchiveButton.IsEnabled = total > 0;
        clearFiltersButton.IsEnabled = filters.Any;
    }

    /// <summary>Presentation only: the panel and template change, the collection does not.</summary>
    private void RenderViewMode()
    {
        if (browser.Tag is AmiiboViewMode shown && shown == filters.View)
        {
            return;
        }

        browser.Tag = filters.View;
        switch (filters.View)
        {
            case AmiiboViewMode.List:
                browser.ItemsPanel = new FuncTemplate<Panel?>(() => new VirtualizingStackPanel());
                browser.ItemTemplate = new FuncDataTemplate<AmiiboTile>((tile, _) => ListRow());
                ScrollViewer.SetHorizontalScrollBarVisibility(browser, ScrollBarVisibility.Disabled);
                ScrollViewer.SetVerticalScrollBarVisibility(browser, ScrollBarVisibility.Auto);
                break;
            case AmiiboViewMode.Carousel:
                browser.ItemsPanel = new FuncTemplate<Panel?>(() => new VirtualizingStackPanel { Orientation = Orientation.Horizontal });
                browser.ItemTemplate = new FuncDataTemplate<AmiiboTile>((tile, _) => Tile(240, 300, 18));
                ScrollViewer.SetHorizontalScrollBarVisibility(browser, ScrollBarVisibility.Auto);
                ScrollViewer.SetVerticalScrollBarVisibility(browser, ScrollBarVisibility.Disabled);
                break;
            default:
                // Centred, like Windows' grid: the leftover width is shared on both sides
                // rather than left as a gap on the right.
                browser.ItemsPanel = new FuncTemplate<Panel?>(() => new WrapPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center });
                browser.ItemTemplate = new FuncDataTemplate<AmiiboTile>((tile, _) => Tile(150, 196, 13));
                ScrollViewer.SetHorizontalScrollBarVisibility(browser, ScrollBarVisibility.Disabled);
                ScrollViewer.SetVerticalScrollBarVisibility(browser, ScrollBarVisibility.Auto);
                break;
        }

        if (viewBox.SelectedIndex != (int)filters.View)
        {
            suppress = true;
            viewBox.SelectedIndex = (int)filters.View;
            suppress = false;
        }
    }

    /// <summary>A grid or carousel tile: artwork leads, then identity, then one badge.</summary>
    private static Control Tile(double width, double height, double titleSize)
    {
        var image = new Image { Stretch = Stretch.Uniform, Margin = new Thickness(8) };
        image.Bind(Image.SourceProperty, new Binding(nameof(AmiiboTile.Image)));
        var placeholder = Ui.Icon(Icons.Image, width / 4).Bind(PathIcon.ForegroundProperty, Tokens.TextTertiary);
        placeholder.Bind(IsVisibleProperty, new Binding(nameof(AmiiboTile.HasImage)) { Converter = Avalonia.Data.Converters.BoolConverters.Not });

        var badge = new Border
        {
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(7, 2),
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Top,
            Child = new TextBlock { FontSize = 10, Foreground = Brushes.White, [!TextBlock.TextProperty] = new Binding(nameof(AmiiboTile.Badge)) },
            [!IsVisibleProperty] = new Binding(nameof(AmiiboTile.HasBadge)),
        }.Bind(Border.BackgroundProperty, Tokens.Accent);

        var title = new TextBlock
        {
            FontSize = titleSize,
            FontWeight = FontWeight.SemiBold,
            TextAlignment = TextAlignment.Center,
            TextWrapping = TextWrapping.Wrap,
            TextTrimming = TextTrimming.CharacterEllipsis,
            MaxLines = 2,
            [!TextBlock.TextProperty] = new Binding(nameof(AmiiboTile.Title)),
        };
        var subtitle = Ui.Caption();
        subtitle.TextAlignment = TextAlignment.Center;
        subtitle.TextWrapping = TextWrapping.NoWrap;
        subtitle.TextTrimming = TextTrimming.CharacterEllipsis;
        subtitle.Bind(TextBlock.TextProperty, new Binding(nameof(AmiiboTile.Subtitle)));

        var art = new Border
        {
            CornerRadius = new CornerRadius(8),
            Margin = new Thickness(0, 0, 0, 8),
            Child = new Panel { Children = { placeholder, image } },
        }.Bind(Border.BackgroundProperty, Tokens.Subtle);
        var grid = new Grid
        {
            RowDefinitions = new RowDefinitions("*,Auto,Auto"),
            Children = { art, badge, Tick(20), },
        };
        Grid.SetRow(title, 1);
        Grid.SetRow(subtitle, 2);
        grid.Children.Add(title);
        grid.Children.Add(subtitle);
        var tile = new Border
        {
            Width = width,
            Height = height,
            Padding = new Thickness(8),
            Child = grid,
        };
        tile.Bind(AutomationProperties.NameProperty, new Binding(nameof(AmiiboTile.AccessibleName)));
        return tile;
    }

    /// <summary>The dense list row: thumbnail and aligned columns.</summary>
    private static Control ListRow()
    {
        var image = new Image { Width = 30, Height = 30, Stretch = Stretch.Uniform };
        image.Bind(Image.SourceProperty, new Binding(nameof(AmiiboTile.Image)));
        var thumb = new Panel { Width = 34, Height = 34, Children = { image, Tick(18) } };

        TextBlock Column(string property, bool strong = false)
        {
            var text = strong ? Ui.Text() : Ui.Caption();
            text.TextWrapping = TextWrapping.NoWrap;
            text.TextTrimming = TextTrimming.CharacterEllipsis;
            text.VerticalAlignment = VerticalAlignment.Center;
            text.Bind(TextBlock.TextProperty, new Binding(property));
            return text;
        }

        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,2*,1.4*,1.2*,Auto"), ColumnSpacing = 12, Height = 40 };
        grid.Children.Add(thumb);
        var cells = new[] { Column(nameof(AmiiboTile.Title), strong: true), Column(nameof(AmiiboTile.Series)), Column(nameof(AmiiboTile.Collection)), Column(nameof(AmiiboTile.FigureId)) };
        cells[^1].FontFamily = Ui.MonoFont;
        for (var i = 0; i < cells.Length; i++)
        {
            Grid.SetColumn(cells[i], i + 1);
            grid.Children.Add(cells[i]);
        }

        grid.Bind(AutomationProperties.NameProperty, new Binding(nameof(AmiiboTile.AccessibleName)));
        return grid;
    }

    /// <summary>Membership of the bulk set, distinct from focus. Not hit-testable: the whole tile is the target.</summary>
    private static Control Tick(double size)
    {
        var filled = new Panel
        {
            Children =
            {
                new Ellipse().Bind(Shape.FillProperty, Tokens.Accent),
                new PathIcon { Data = Geometry.Parse(Icons.Check), Width = size * 0.55, Height = size * 0.55, Foreground = Brushes.White },
            },
            [!IsVisibleProperty] = new Binding(nameof(AmiiboTile.ShowTicked)),
        };
        return new Panel
        {
            Width = size,
            Height = size,
            Margin = new Thickness(4),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Top,
            IsHitTestVisible = false,
            Children = { new Ellipse { StrokeThickness = 1.5 }.Bind(Shape.FillProperty, Tokens.Card).Bind(Shape.StrokeProperty, Tokens.TextSecondary), filled },
            [!IsVisibleProperty] = new Binding(nameof(AmiiboTile.Selectable)),
        };
    }

    private void RestoreSelection()
    {
        suppress = true;
        browser.SelectedItem = tiles.FirstOrDefault(tile => tile.Id == interaction.FocusedId);
        suppress = false;
    }

    private void RenderTicks()
    {
        foreach (var tile in tiles)
        {
            tile.Selectable = interaction.Selecting;
            tile.Ticked = interaction.Selecting && interaction.IsSelected(tile.Id);
        }
    }

    /// <summary>Filter choices come from what the user owns, not every series in existence.</summary>
    private void RenderFilterOptions(AmiiboView view)
    {
        var options = AmiiboGallery.Options(view.Library, catalog.Find);
        suppress = true;
        Fill(gameSeriesBox, AllGameSeries, options.GameSeries, filters.GameSeries);
        Fill(amiiboSeriesBox, AllCollections, options.AmiiboSeries, filters.AmiiboSeries);
        Fill(typeBox, AllTypes, options.Types, filters.Type);
        suppress = false;

        static void Fill(ComboBox box, string all, IEnumerable<string> values, string selected)
        {
            var items = new List<string> { all };
            items.AddRange(values);
            if (box.ItemsSource is not List<string> existing || !existing.SequenceEqual(items))
            {
                box.ItemsSource = items;
            }

            box.SelectedItem = selected.Length == 0 ? all : selected;
        }
    }

    /// <summary>The inspector: who this is, what is on the tag, and what can be done.</summary>
    private void RenderSelection(AmiiboView view)
    {
        var item = view.Selected;
        var chosen = item is not null;
        selectionEmpty.IsVisible = !chosen;
        heroPanel.IsVisible = chosen;
        actionPanel.IsVisible = chosen;
        categoryBar.IsVisible = chosen;
        overviewTab.IsChecked = category == InspectorCategory.Overview;
        tagTab.IsChecked = category == InspectorCategory.Tag;
        adapterTab.IsChecked = category == InspectorCategory.Adapter;
        adapterPanel.IsVisible = category == InspectorCategory.Adapter;
        detailHost.Children.Clear();
        if (item is null)
        {
            return;
        }

        var entry = catalog.Find(item.FigureId);
        var card = tiles.FirstOrDefault(tile => tile.Id == item.Id);
        selectedTitle.Text = card?.Title ?? item.DisplayName;
        selectedSubtitle.Text = card?.Subtitle ?? item.FigureId;
        var onAdapter = view.LoadedFromLibrary?.Id == item.Id;
        selectedAdapterState.Text = onAdapter
            ? view.NeedsSync ? "On the adapter · changed by the console" : "On the adapter"
            : "Not on the adapter";

        selectedArtwork.Source = null;
        selectedPlaceholder.IsVisible = true;
        if (entry?.ImageUrl is { Length: > 0 } url)
        {
            _ = ShowArtworkAsync(item.Id, url);
        }

        var busy = transfer is not null;
        sendButton.IsEnabled = view.CanUpload && !busy;

        // The primary action stays visible-but-disabled WITH its reason.
        Ui.SetOptional(sendReason, view.CanUpload ? null : view.NeedsSync ? "Sync the adapter's changed Amiibo first." : view.UnavailableReason);
        initializeItem.IsEnabled = keys.Exists;

        if (category == InspectorCategory.Adapter)
        {
            return;
        }

        var groups = AmiiboInspection.Build(item, entry, SelectedDetails(item), onAdapter: onAdapter, adapterChanged: view.NeedsSync && onAdapter);
        string[] wanted = category == InspectorCategory.Overview ? ["Identity"] : ["Tag", "Registration", "Game data"];
        var shown = groups.Where(group => wanted.Contains(group.Title)).ToList();
        foreach (var group in shown)
        {
            if (shown.Count > 1)
            {
                var heading = Ui.Caption(group.Title.ToUpperInvariant());
                heading.FontWeight = FontWeight.SemiBold;
                heading.Margin = new Thickness(0, 8, 0, 2);
                detailHost.Children.Add(heading);
            }

            foreach (var row in group.Rows)
            {
                var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("104,*"), ColumnSpacing = 12, Margin = new Thickness(0, 3) };
                grid.Children.Add(Ui.Caption(row.Label));
                var value = Ui.Text(row.Value);
                value.FontSize = 13;
                if (row.Monospace)
                {
                    value.FontFamily = Ui.MonoFont;
                }

                Grid.SetColumn(value, 1);
                grid.Children.Add(value);
                detailHost.Children.Add(grid);
            }
        }
    }

    private async Task ShowArtworkAsync(string id, string url)
    {
        var bitmap = await AmiiboArtwork.LoadAsync(url);
        if (bitmap is not null && interaction.FocusedId == id)
        {
            selectedArtwork.Source = bitmap;
            selectedPlaceholder.IsVisible = false;
        }
    }

    /// <summary>Recomputed, not cached: keys can be imported or forgotten while the page is open.</summary>
    private AmiiboDetails? SelectedDetails(AmiiboLibraryItem item)
    {
        try
        {
            return AmiiboCrypto.ReadDetails(library.Bytes(item.Id), keys.Read());
        }
        catch (Exception)
        {
            return null;
        }
    }

    /// <summary>Width decides WHERE the inspector goes, never WHETHER it is there.</summary>
    private void ApplyLayout()
    {
        var open = interaction.InspectorOpen;
        inspector.IsVisible = open;
        if (open)
        {
            Grid.SetColumn(inspector, sideBySide ? 1 : 0);
            Grid.SetColumnSpan(inspector, sideBySide ? 1 : 2);
            inspector.Width = sideBySide ? InspectorWidth : double.NaN;
            inspector.Margin = sideBySide ? new Thickness(12, 0, 0, 0) : default;
            inspector.MaxWidth = sideBySide ? InspectorWidth : 460;
            inspector.BoxShadow = sideBySide ? default : BoxShadows.Parse("0 8 32 0 #50000000");
        }

        detailsButton.IsVisible = !open && !interaction.Selecting;
        detailsButton.IsEnabled = interaction.FocusedId is not null;
        selectButton.IsVisible = !interaction.Selecting;
        selectButton.IsEnabled = interaction.FocusedId is not null;

        selectionBar.IsVisible = interaction.Selecting;
        if (interaction.Selecting)
        {
            var hidden = interaction.HiddenSelectedCount(tiles.Select(tile => tile.Id));
            selectionCount.Text = hidden == 0
                ? $"{interaction.SelectedCount} selected"
                : $"{interaction.SelectedCount} selected ({hidden} hidden by filters)";
        }
    }

    /// <summary>THE one place the UI learns about interaction state. Never reassigns the items.</summary>
    private void SetInteraction(AmiiboInteractionState next)
    {
        var focusChanged = next.FocusedId != interaction.FocusedId;
        interaction = next;
        RestoreSelection();
        RenderTicks();
        ApplyLayout();
        if (focusChanged)
        {
            amiiboRead = false;
        }

        RenderSelection(View());
    }

    /* -------------------------------------------------------------- browsing */

    private static AmiiboTile? TileUnder(object? source) =>
        (source as Visual)?.GetSelfAndVisualAncestors().OfType<Control>().Select(c => c.DataContext).OfType<AmiiboTile>().FirstOrDefault();

    /// <summary>Keyboard and programmatic focus moves. A pointer is handled by Tapped, never both.</summary>
    private void OnBrowserSelectionChanged()
    {
        if (suppress || pointerDown)
        {
            return;
        }

        SetInteraction(browser.SelectedItem is AmiiboTile tile
            ? AmiiboInteraction.Activate(interaction, tile.Id)
            : interaction with { FocusedId = null, InspectedId = null });
    }

    /// <summary>Every tap, including on the already-focused tile. Ctrl+click is the desktop long press.</summary>
    private void OnBrowserTapped(object? sender, TappedEventArgs e)
    {
        pointerDown = false;
        if (TileUnder(e.Source) is not { } tile)
        {
            return;
        }

        var control = e.KeyModifiers.HasFlag(KeyModifiers.Control) || e.KeyModifiers.HasFlag(KeyModifiers.Meta);
        SetInteraction(control
            ? AmiiboInteraction.ToggleSelection(interaction, tile.Id)
            : AmiiboInteraction.Activate(interaction, tile.Id));
    }

    private void OnBrowserDoubleTapped(object? sender, TappedEventArgs e)
    {
        if (TileUnder(e.Source) is { } tile)
        {
            e.Handled = true;
            SetInteraction(AmiiboInteraction.OpenInspector(interaction, tile.Id));
        }
    }

    private void OnShowDetails()
    {
        if (interaction.FocusedId is { } id)
        {
            SetInteraction(AmiiboInteraction.OpenInspector(interaction, id));
        }
    }

    private void OnStartSelection()
    {
        if (interaction.FocusedId is { } id)
        {
            SetInteraction(AmiiboInteraction.EnterSelection(interaction, id));
        }
    }

    /// <summary>
    /// Enter on the highlighted card opens its details and moves focus into
    /// them (companion-shell-layout.md, "What lives where": under a controller
    /// the focused card IS the highlighted one, and A opens it). Escape reaches
    /// the page as the shell's Back (<see cref="HandleBack"/>).
    /// </summary>
    private void OnPageKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key is not (Key.Enter or Key.Space) || interaction.Selecting ||
            e.Source is not Visual source || !browser.IsVisualAncestorOf(source) ||
            browser.SelectedItem is not AmiiboTile tile)
        {
            return;
        }

        e.Handled = true;
        SetInteraction(AmiiboInteraction.OpenInspector(interaction, tile.Id));
        FocusInspector();
    }

    /// <summary>Back cancels a selection, then closes the details (focus returns to the card); otherwise the shell's.</summary>
    public override bool HandleBack()
    {
        var wasOpen = interaction.InspectorOpen;
        var next = AmiiboInteraction.Escape(interaction);
        if (next == interaction)
        {
            return false;
        }

        SetInteraction(next);
        if (wasOpen && !interaction.InspectorOpen)
        {
            FocusFocusedTile();
        }

        return true;
    }

    /// <summary>Under a controller, the library's first stop is the highlighted card.</summary>
    public override InputElement? PreferredFocus =>
        (browser.SelectedItem ?? tiles.FirstOrDefault()) is { } item ? browser.ContainerFromItem(item) : null;

    /// <summary>Play's Amiibo tile: land on this figure, highlighted and in view.</summary>
    public void FocusOn(string id)
    {
        interaction = AmiiboInteraction.Focus(interaction, id);
        if (browser.GetVisualRoot() is not null)
        {
            SetInteraction(interaction);
        }

        Dispatcher.UIThread.Post(() =>
        {
            if (browser.SelectedItem is { } selected)
            {
                browser.ScrollIntoView(selected);
            }
        }, DispatcherPriority.Loaded);
    }

    private void FocusInspector() => Dispatcher.UIThread.Post(() =>
    {
        if (KeyboardNavigationHandler.GetNext(inspector, NavigationDirection.Next) is InputElement first &&
            inspector.IsVisualAncestorOf(first))
        {
            first.Focus(NavigationMethod.Directional);
        }
    }, DispatcherPriority.Loaded);

    private void FocusFocusedTile() => Dispatcher.UIThread.Post(() =>
    {
        if (browser.SelectedItem is { } item && browser.ContainerFromItem(item) is { } container)
        {
            container.Focus(NavigationMethod.Directional);
        }
    }, DispatcherPriority.Loaded);

    private void OnCategory(InspectorCategory chosen)
    {
        category = chosen;
        RenderSelection(View());
    }

    private void OnSortChanged()
    {
        if (!suppress && sortBox.SelectedIndex >= 0)
        {
            filters = filters with { Sort = (AmiiboSort)sortBox.SelectedIndex };
            Render();
        }
    }

    /// <summary>How the library is shown, never what it contains. Persisted.</summary>
    private void OnViewModeChanged()
    {
        if (suppress || viewBox.SelectedIndex < 0)
        {
            return;
        }

        filters = filters with { View = (AmiiboViewMode)viewBox.SelectedIndex };
        AppServices.AmiiboViewMode = filters.View;
        RenderViewMode();
        RestoreSelection();
        if (browser.SelectedItem is { } selected)
        {
            browser.ScrollIntoView(selected);
        }
    }

    private void OnFilterChanged(ComboBox box)
    {
        if (suppress)
        {
            return;
        }

        // The sentinel is always row 0.
        var chosen = box.SelectedIndex <= 0 ? string.Empty : box.SelectedItem as string ?? string.Empty;
        filters = ReferenceEquals(box, gameSeriesBox) ? filters with { GameSeries = chosen }
            : ReferenceEquals(box, amiiboSeriesBox) ? filters with { AmiiboSeries = chosen }
            : filters with { Type = chosen };
        Render();
    }

    private void OnClearFilters()
    {
        // Sort and view are preferences, not narrowing: they survive.
        filters = new AmiiboGalleryFilters { Sort = filters.Sort, Descending = filters.Descending, View = filters.View };
        suppress = true;
        searchBox.Text = string.Empty;
        suppress = false;
        Render();
    }

    /* --------------------------------------------------------------- adapter */

    /// <summary>Send the selected backup. Refused while the adapter holds unsynced console writes.</summary>
    private async Task OnUploadAsync()
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
            Report($"'{item.DisplayName}' could not be read: {error.Message}", NoticeSeverity.Error);
            return;
        }

        await RunTransferAsync(
            token => adapters.UploadAmiiboAsync(bytes, useSave2: false, progress: OnTransferProgress, cancellationToken: token),
            $"Sending '{item.DisplayName}'…",
            $"'{item.DisplayName}' is on the adapter. Present it when the game asks.");
    }

    /// <summary>Read the tag back and store it, THEN acknowledge: the order is the whole point.</summary>
    private async Task OnSyncAsync()
    {
        if (!View().CanSync)
        {
            return;
        }

        var download = await RunTransferAsync(
            token => adapters.DownloadAmiiboAsync(OnTransferProgress, token),
            "Reading the tag from the adapter…",
            completion: null);
        if (download is null)
        {
            return;
        }

        var stored = library.UpdateFromAdapter(View().LoadedFromLibrary?.Id, download.Bytes);
        interaction = AmiiboInteraction.Focus(interaction, stored.Id);
        try
        {
            await adapters.AcknowledgeAmiiboDownloadAsync(download);
            Report($"'{stored.DisplayName}' synced to your library.", NoticeSeverity.Success);
        }
        catch (Exception error)
        {
            // The bytes ARE saved; only the adapter's flag was not cleared.
            Report(
                $"'{stored.DisplayName}' was saved to your library, but the adapter would not clear its changed flag: " +
                $"{ManagementErrorText.Summarize(error)} Sync again to pick up the newer state.",
                NoticeSeverity.Warning);
        }

        Render();
    }

    private async Task OnClearAsync()
    {
        if (View().CanClear && await Dialogs.ConfirmAsync(
                "Remove the Amiibo from the adapter?",
                "The adapter stops holding this tag. Your library keeps its copy.",
                "Clear"))
        {
            await RunAsync(() => adapters.ClearAmiiboAsync());
        }
    }

    /* --------------------------------------------------------------- library */

    private static readonly FilePickerFileType AmiiboFiles = new("Amiibo dumps and archives")
    {
        Patterns = ["*.bin", "*.nfc", "*.zip"],
    };

    private IStorageProvider? Storage => TopLevel.GetTopLevel(this)?.StorageProvider;

    private async Task OnImportAsync()
    {
        if (Storage is not { } storage)
        {
            return;
        }

        var files = await storage.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Import Amiibo",
            AllowMultiple = true,
            FileTypeFilter = [AmiiboFiles, FilePickerFileTypes.All],
        });
        if (files.Count == 0)
        {
            return;
        }

        var sources = new List<AmiiboImportSource>();
        foreach (var file in files)
        {
            sources.Add(new AmiiboImportSource(file.Name, await ReadAsync(file)));
        }

        ApplyBulkImport(library.ImportMany(sources));
    }

    private async Task OnImportFolderAsync()
    {
        if (Storage is not { } storage)
        {
            return;
        }

        var folders = await storage.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Import a folder of Amiibo" });
        if (folders.Count == 0 || folders[0].TryGetLocalPath() is not { } root)
        {
            return;
        }

        var paths = Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
            .Where(path => Path.GetExtension(path).ToLowerInvariant() is ".bin" or ".nfc" or ".zip")
            .Take(AmiiboArchive.MaxEntries)
            .ToList();
        if (paths.Count == 0)
        {
            Report("No Amiibo dumps or archives were found in that folder.", NoticeSeverity.Informational);
            return;
        }

        var sources = new List<AmiiboImportSource>();
        foreach (var path in paths)
        {
            try
            {
                sources.Add(new AmiiboImportSource(Path.GetFileName(path), await File.ReadAllBytesAsync(path)));
            }
            catch (IOException error)
            {
                adapters.Diagnostics.Warn("amiibo", $"{Path.GetFileName(path)} could not be read: {error.Message}");
            }
        }

        ApplyBulkImport(library.ImportMany(sources));
    }

    private void ApplyBulkImport(AmiiboBulkImportResult result)
    {
        foreach (var problem in result.Problems)
        {
            adapters.Diagnostics.Warn("amiibo", problem);
        }

        if (result.Imported.Count == 1)
        {
            interaction = AmiiboInteraction.Focus(interaction, result.Imported[0].Id);
        }

        Report(
            result.Problems.Count == 0 ? result.Summary : $"{result.Summary} See Diagnostics for what was skipped.",
            result.Imported.Count > 0 ? NoticeSeverity.Success : NoticeSeverity.Informational);
        Render();
    }

    private async Task OnExportArchiveAsync()
    {
        if (library.Items.Value.Count == 0)
        {
            Report("There is nothing in the library to export.", NoticeSeverity.Informational);
            return;
        }

        await SaveAsync("PicoSwitch2 Amiibo library", "zip", "amiibo-library", library.ExportArchive(View().LoadedFromLibrary?.Id));
    }

    private async Task OnExportOneAsync()
    {
        if (View().Selected is { } item)
        {
            await SaveAsync("Amiibo backup", "bin", item.DisplayName, library.Bytes(item.Id));
        }
    }

    private async Task OnRenameAsync()
    {
        if (View().Selected is not { } item)
        {
            return;
        }

        var box = new TextBox { Text = item.DisplayName, MaxLength = AmiiboArchive.MaxNameChars, MinWidth = 320 };
        if (await Dialogs.ShowAsync("Rename", Ui.Stack(Ui.Text("Only the name changes.", secondary: true), box), ["OK", "Cancel"], primaryIndex: 0, defaultIndex: 0) == 0 &&
            !string.IsNullOrWhiteSpace(box.Text))
        {
            library.Rename(item.Id, box.Text.Trim());
            Render();
        }
    }

    private async Task OnDeleteAsync()
    {
        if (View().Selected is not { } item ||
            !await Dialogs.ConfirmAsync($"Delete '{item.DisplayName}'?", "This removes your only copy of this backup. The adapter is not affected.", "Delete"))
        {
            return;
        }

        // The neighbour that takes its place keeps the user where they were.
        interaction = AmiiboInteraction.AfterRemoval(interaction, [.. tiles.Select(tile => tile.Id)], [item.Id]);
        library.Delete(item.Id);
        Render();
    }

    /// <summary>Wipe owner, nickname and game data and re-sign. Rewrites the stored backup in place.</summary>
    private async Task OnInitializeAsync()
    {
        if (View().Selected is not { } item)
        {
            return;
        }

        if (keys.Read() is not { } retail)
        {
            Report("Import your Amiibo keys before initializing a tag.", NoticeSeverity.Warning);
            return;
        }

        if (!await Dialogs.ConfirmAsync(
                $"Initialize '{item.DisplayName}'?",
                "The owner, nickname, registration dates and any game data are erased, and the backup is re-signed in place. " +
                "The figure itself is unchanged. This cannot be undone; export a copy first if you might want this save back.",
                "Initialize"))
        {
            return;
        }

        library.UpdateFromAdapter(item.Id, AmiiboCrypto.Initialize(library.Bytes(item.Id), retail));
        Report($"'{item.DisplayName}' is now a blank figure.", NoticeSeverity.Success);
        Render();
    }

    /* ------------------------------------------------------------------ bulk */

    private async Task OnBulkDeleteAsync()
    {
        var chosen = interaction.Selection.ToArray();
        if (chosen.Length == 0 ||
            !await Dialogs.ConfirmAsync(
                $"Delete {chosen.Length} Amiibo from your library?",
                HiddenNote(chosen) + "This removes your only copy of these backups. It cannot be undone, and the Amiibo currently on the adapter is not affected.",
                $"Delete {chosen.Length}"))
        {
            return;
        }

        var outcome = Batch(chosen, id => library.Delete(id));
        interaction = AmiiboInteraction.AfterRemoval(interaction, [.. tiles.Select(tile => tile.Id)], outcome.Succeeded);
        Report(outcome.Summary("deleted") + "; the adapter was not changed", outcome.AnyFailed ? NoticeSeverity.Warning : NoticeSeverity.Success);
        Render();
    }

    private async Task OnBulkInitializeAsync()
    {
        var chosen = interaction.Selection.ToArray();
        if (chosen.Length == 0)
        {
            return;
        }

        if (keys.Read() is not { } retail)
        {
            Report("Import your Amiibo keys before initializing tags.", NoticeSeverity.Warning);
            return;
        }

        if (!await Dialogs.ConfirmAsync(
                $"Initialize {chosen.Length} Amiibo?",
                HiddenNote(chosen) +
                "The owner, nickname, registration dates and any game data are erased from the selected backups, and each is re-signed in place. " +
                "The figures themselves are unchanged, and so is the adapter. This cannot be undone.",
                $"Initialize {chosen.Length}"))
        {
            return;
        }

        var outcome = Batch(chosen, id => library.UpdateFromAdapter(id, AmiiboCrypto.Initialize(library.Bytes(id), retail)));
        interaction = AmiiboInteraction.ClearSelection(interaction);
        Report(outcome.Summary("initialized"), outcome.AnyFailed ? NoticeSeverity.Warning : NoticeSeverity.Success);
        Render();
    }

    /// <summary>No transaction, no rollback: partial progress is the honest outcome, so it is the reported one.</summary>
    private AmiiboBulkOutcome Batch(IReadOnlyList<string> ids, Action<string> operation)
    {
        var done = new List<string>();
        var failed = new List<AmiiboBulkFailure>();
        foreach (var id in ids)
        {
            var name = library.Items.Value.FirstOrDefault(item => item.Id == id)?.DisplayName ?? id;
            try
            {
                operation(id);
                done.Add(id);
            }
            catch (Exception error) when (error is not OutOfMemoryException)
            {
                failed.Add(new AmiiboBulkFailure(id, name, error.Message));
                adapters.Diagnostics.Warn("amiibo", $"{name}: {error.Message}");
            }
        }

        return new AmiiboBulkOutcome(done, failed);
    }

    /// <summary>Says how many of the doomed entries are hidden by the current filters.</summary>
    private string HiddenNote(IReadOnlyList<string> chosen)
    {
        var visible = tiles.Select(tile => tile.Id).ToHashSet(StringComparer.Ordinal);
        var hidden = chosen.Count(id => !visible.Contains(id));
        return hidden == 0 ? string.Empty : $"{hidden} of these are hidden by the current search or filters. ";
    }

    /* ------------------------------------------------------------------ keys */

    private async Task OnImportKeysAsync()
    {
        if (Storage is not { } storage)
        {
            return;
        }

        var files = await storage.OpenFilePickerAsync(new FilePickerOpenOptions { Title = "Import Amiibo keys", AllowMultiple = false });
        if (files.Count == 0)
        {
            return;
        }

        try
        {
            keys.Import(await ReadAsync(files[0]));
            Report("Amiibo keys imported.", NoticeSeverity.Success);
        }
        catch (ArgumentException error)
        {
            // Refused at import, so the user is pointed at the key file, not their tags.
            Report($"That is not a usable Amiibo key file: {error.Message}", NoticeSeverity.Error);
        }

        Render();
    }

    private async Task OnForgetKeysAsync()
    {
        if (keys.Exists && await Dialogs.ConfirmAsync(
                "Forget the Amiibo keys?",
                "Nicknames, owners and game data stop being readable until you import them again. Your backups are not affected.",
                "Forget"))
        {
            keys.Clear();
            Render();
        }
    }

    private void ReportLibraryWarnings()
    {
        foreach (var warning in library.Warnings.Value)
        {
            adapters.Diagnostics.Warn("amiibo", warning);
        }

        if (library.Warnings.Value.Count > 0)
        {
            Report(library.Warnings.Value[0], NoticeSeverity.Warning);
        }
    }

    /* -------------------------------------------------------------- plumbing */

    private async Task<T?> RunTransferAsync<T>(Func<CancellationToken, Task<T>> operation, string message, string? completion)
        where T : class
    {
        using var cancellation = new CancellationTokenSource();
        transferCancellation = cancellation;
        transfer = (0, 1);
        Report(message, NoticeSeverity.Informational);
        Render();
        var succeeded = false;
        try
        {
            var result = await operation(cancellation.Token);
            succeeded = true;
            return result;
        }
        catch (OperationCanceledException)
        {
            Report("Cancelled.", NoticeSeverity.Informational);
            return null;
        }
        catch (Exception error)
        {
            Report(ManagementErrorText.Summarize(error), NoticeSeverity.Error);
            adapters.Diagnostics.Error("amiibo", $"{error.GetType().Name}: {error.Message}");
            return null;
        }
        finally
        {
            transferCancellation = null;
            transfer = null;
            Render();
            if (succeeded && completion is not null)
            {
                Report(completion, NoticeSeverity.Success);
            }
        }
    }

    /// <summary>Progress arrives on a pool thread; hop to the UI thread before touching anything.</summary>
    private void OnTransferProgress(int completed, int total) => AppServices.OnUiThread(() =>
    {
        transfer = (completed, total);
        RenderTransfer();
    });

    private async Task SaveAsync(string description, string extension, string suggestedName, byte[] bytes)
    {
        if (Storage is not { } storage)
        {
            return;
        }

        var invalid = Path.GetInvalidFileNameChars();
        var cleaned = new string(suggestedName.Select(c => invalid.Contains(c) ? '_' : c).ToArray()).Trim();
        var file = await storage.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            Title = description,
            SuggestedFileName = (cleaned.Length == 0 ? "amiibo" : cleaned) + "." + extension,
            DefaultExtension = extension,
            FileTypeChoices = [new FilePickerFileType(description) { Patterns = [$"*.{extension}"] }],
        });
        if (file is null)
        {
            return;
        }

        await using (var stream = await file.OpenWriteAsync())
        {
            await stream.WriteAsync(bytes);
        }

        Report($"Saved to {file.Name}.", NoticeSeverity.Success);
    }

    private static async Task<byte[]> ReadAsync(IStorageFile file)
    {
        await using var stream = await file.OpenReadAsync();
        using var memory = new MemoryStream();
        await stream.CopyToAsync(memory);
        return memory.ToArray();
    }

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
            Report(message, NoticeSeverity.Error);
            Render();
        }
    }

    private Task RunAsync(Func<Task> operation) => SafeAsync(operation, Render);

    private static Control Top(Control control, double gap = 0)
    {
        DockPanel.SetDock(control, Dock.Top);
        control.Margin = new Thickness(0, 0, 0, gap);
        return control;
    }
}

/// <summary>
/// The page frame for a page that fills the window instead of scrolling (the
/// Amiibo browser scrolls inside its card). Same padding rule as
/// <see cref="PageFrame"/>, so the two kinds of page line up.
/// </summary>
/// <remarks>
/// Fill, until the window is too short to fill: below <see cref="MinimumFill"/>
/// the page keeps that height and scrolls as a whole, so the query controls are
/// never squeezed under the browser. Deterministic (no feedback from the
/// content's own size), so it cannot oscillate.
/// </remarks>
internal sealed class PageFill : Decorator
{
    public const double MinimumFill = 600;

    private readonly FillHost host = new();

    public PageFill(Control content)
    {
        host.Child = content;
        Child = new ScrollViewer
        {
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = host,
        };
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        var padding = availableSize.Width < 640
            ? new Thickness(14)
            : availableSize.Width < 1000 ? new Thickness(24, 20) : new Thickness(36, 28);
        if (host.Padding != padding)
        {
            host.Padding = padding;
        }

        if (host.ViewportHeight != availableSize.Height)
        {
            host.ViewportHeight = availableSize.Height;
            host.InvalidateMeasure();
        }

        return base.MeasureOverride(availableSize);
    }

    private sealed class FillHost : Decorator
    {
        public double ViewportHeight { get; set; } = double.PositiveInfinity;

        protected override Size MeasureOverride(Size availableSize)
        {
            var height = double.IsFinite(ViewportHeight) ? Math.Max(ViewportHeight, MinimumFill) : MinimumFill;
            var inner = new Size(
                Math.Max(0, availableSize.Width - Padding.Left - Padding.Right),
                Math.Max(0, height - Padding.Top - Padding.Bottom));
            Child?.Measure(inner);
            return new Size(availableSize.Width, height);
        }

        protected override Size ArrangeOverride(Size finalSize)
        {
            Child?.Arrange(new Rect(finalSize).Deflate(Padding));
            return finalSize;
        }
    }
}
