using System.ComponentModel;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
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
/// One item in the library browser, in whichever view is showing.
/// </summary>
/// <remarks>
/// Top-level and public because the XAML templates bind to it: the markup
/// compiler cannot reach a nested type through <c>x:DataType</c>.
///
/// ONE TYPE FOR ALL THREE VIEWS. Grid and Carousel use the identity fields; the
/// detailed list additionally shows the columns. A separate type per view would
/// mean three collections, and three collections is how a "shared query state"
/// quietly stops being shared.
///
/// Carries a resolved <see cref="Image"/> rather than a URL, so the templates
/// need no converter and no binding that can fail silently on a bad address.
/// </remarks>
public sealed class AmiiboTile(
    string id,
    string title,
    string subtitle,
    string badge,
    Func<ImageSource?> image,
    string series = "",
    string collection = "",
    string released = "",
    string figureId = "") : INotifyPropertyChanged
{
    public string Id { get; } = id;

    public string Title { get; } = title;

    public string Subtitle { get; } = subtitle;

    public string Badge { get; } = badge;

    private ImageSource? resolvedImage;
    private bool imageResolved;

    /// <summary>
    /// The artwork, fetched the first time a container actually asks for it.
    /// </summary>
    /// <remarks>
    /// RESOLVED LAZILY, AND THAT IS THE WHOLE POINT. A BitmapImage begins
    /// downloading the moment it is constructed, so building one per card
    /// started a thousand HTTP requests every time the browser was rebuilt —
    /// for a library of which perhaps twenty tiles were on screen. The rest were
    /// bandwidth and decode work spent on pictures nobody was looking at, and it
    /// competed with the adapter transfer happening at the same time.
    ///
    /// Only a REALISED container evaluates this binding, so the grid now fetches
    /// what it draws and nothing else.
    /// </remarks>
    public ImageSource? Image
    {
        get
        {
            if (!imageResolved)
            {
                imageResolved = true;
                resolvedImage = image();
            }

            return resolvedImage;
        }
    }

    public string Series { get; } = series;

    public string Collection { get; } = collection;

    public string Released { get; } = released;

    public string FigureId { get; } = figureId;

    public Visibility BadgeVisibility =>
        Badge.Length > 0 ? Visibility.Visible : Visibility.Collapsed;

    /// <summary>Shown when there is no artwork, so a tile is never a gap.</summary>
    public Visibility PlaceholderVisibility =>
        Image is null ? Visibility.Visible : Visibility.Collapsed;

    /// <summary>
    /// What a screen reader says for this tile.
    /// </summary>
    /// <remarks>
    /// Without it the automation tree announces the type name — every one of a
    /// thousand tiles reading out as "AmiiboTile", which is no more useful than
    /// silence. Deliberately static: the tick state belongs to the CheckBox in
    /// the template, which announces its own checked state, and duplicating it
    /// here would have it read out twice.
    /// </remarks>
    public string AccessibleName =>
        Subtitle.Length > 0 ? $"{Title}, {Subtitle}" : Title;

    private bool selectable;
    private bool ticked;

    /// <summary>
    /// THE CONTENT ABOVE IS IMMUTABLE; THESE TWO ARE NOT, AND THAT IS THE POINT.
    /// </summary>
    /// <remarks>
    /// Bulk selection changes on every tap, and a thousand-item library cannot
    /// afford to rebuild its projection each time — reassigning a WinUI
    /// ItemsSource discards the container generation and the scroll offset,
    /// which is the exact defect the query model was restructured to remove.
    ///
    /// So the tick is an OBSERVABLE PROPERTY mutated in place. Only realised
    /// containers respond, the collection identity never changes, and the
    /// browser keeps its place while the user works down a long list.
    ///
    /// Deliberately separate from the host's own selection, which draws FOCUS.
    /// Two different ideas need two different presentations, and a user must be
    /// able to tell "where I am" from "what I have ticked" at a glance.
    /// </remarks>
    public bool Selectable
    {
        get => selectable;
        set => Set(ref selectable, value, nameof(Selectable), nameof(TickVisibility));
    }

    public bool Ticked
    {
        get => ticked;
        set => Set(ref ticked, value, nameof(Ticked), nameof(TickedVisibility));
    }

    /// <summary>The empty marker, shown on every tile while selecting.</summary>
    public Visibility TickVisibility =>
        Selectable ? Visibility.Visible : Visibility.Collapsed;

    /// <summary>The filled marker, shown only on the ones that are in the set.</summary>
    public Visibility TickedVisibility =>
        Selectable && Ticked ? Visibility.Visible : Visibility.Collapsed;

    public event PropertyChangedEventHandler? PropertyChanged;

    private void Set(ref bool field, bool value, params string[] names)
    {
        if (field == value)
        {
            return;
        }

        field = value;
        foreach (var name in names)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }
    }
}

/// <summary>
/// Amiibo: the user's collection, and the tag the adapter is holding.
/// </summary>
/// <remarks>
/// BROWSE ON THE LEFT, INSPECT ON THE RIGHT. The library is the primary surface;
/// the selected Amiibo gets a contextual pane that carries its details AND the
/// actions that act on it. Library-wide actions (import, export all) stay in the
/// header with the library. Actions that act on one Amiibo used to float under
/// the whole grid, which said nothing about what they would act on.
///
/// TWO STORES. The library is the user's and works with nothing paired — browse,
/// search, sort, filter, import, rename, initialize, export and delete are all
/// local. The adapter holds exactly one tag, and only Send, Present, Eject, Sync
/// and Clear reach it. Adapter-only controls are disabled with a reason rather
/// than hidden.
///
/// THE RULE THIS PAGE ENFORCES: when a game writes to a tag, the change lives
/// ONLY on the adapter. Sending another tag over it, or clearing it, would
/// destroy the only copy of a save, so both are refused until the user syncs.
///
/// Rules live in <see cref="AmiiboView"/>, <see cref="AmiiboGallery"/> and
/// <see cref="AmiiboInspection"/>; this file renders state and raises events.
/// Every `async void` handler goes through <see cref="GuardAsync"/>: an exception
/// escaping one has no handler at all and terminates the process.
/// </remarks>
public sealed partial class AmiiboPage : Page
{
    private readonly AdapterConnectionService adapters = AppServices.Adapters;
    private readonly AmiiboLibrary library = AppServices.AmiiboLibrary;
    private readonly WindowsAmiiboKeyStore keys = AppServices.AmiiboKeys;
    private readonly AmiiboCatalog catalog = AppServices.AmiiboCatalog;

    /// <summary>
    /// Focus, inspection and bulk selection — three ideas, one place.
    /// </summary>
    /// <remarks>
    /// The page used to hold a bare <c>selectedId</c> plus an
    /// <c>inspectorOpen</c> flag, and those two between them had to mean
    /// "highlighted", "being described" and "about to be deleted". Every
    /// transition now goes through <see cref="AmiiboInteraction"/>, which Android
    /// also uses, so the two clients cannot drift.
    /// </remarks>
    private AmiiboInteractionState interaction = new();

    private string? selectedId => interaction.FocusedId;

    private bool suppressSelection;
    private bool dialogOpen;
    private bool amiiboRead;
    private (int Completed, int Total)? transfer;
    private CancellationTokenSource? transferCancellation;

    /// <summary>
    /// Search, filters, sort and view. Deliberately carries no selection.
    /// </summary>
    private AmiiboGalleryFilters filters = new();

    /// <summary>The one collection all three views share.</summary>
    private IReadOnlyList<AmiiboTile> tiles = [];

    /// <summary>
    /// What the last projected result was, so an unchanged one is not rebuilt.
    /// </summary>
    private string browserSignature = "<unset>";

    /// <summary>Resolved artwork, keyed by URL so a re-render costs nothing.</summary>
    private readonly Dictionary<string, ImageSource> artwork = new(StringComparer.Ordinal);

    /// <summary>
    /// How many decoded images the page will hold. Several screenfuls in any
    /// view, which is what makes scrolling back cost nothing.
    /// </summary>
    private const int MaximumCachedArtwork = 400;

    /// <summary>The "no filter" row. Not a series anyone owns.</summary>
    private const string AllFilter = "All";

    /// <summary>Which inspector category is showing.</summary>
    /// <remarks>
    /// Page state rather than query state: it changes what is DESCRIBED, never
    /// what the library contains, so it is deliberately not part of the filters
    /// and cannot trigger a browser rebuild.
    /// </remarks>
    private enum InspectorCategory
    {
        Overview,
        Tag,
        Adapter,
    }

    private InspectorCategory category = InspectorCategory.Overview;

    /// <summary>
    /// Below this content width the inspector overlays instead of sitting beside
    /// the browser. 420 of inspector plus a browser worth looking at needs about
    /// this much before the two stop competing.
    /// </summary>
    private const double SideBySideMinimumWidth = 860;

    private bool sideBySide = true;
    private bool initialisedLayout;

    /// <summary>See <see cref="OnLibraryPointerPressed"/>.</summary>
    private bool pointerDown;

    public AmiiboPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private async void OnLoaded(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            filters = filters with { View = AppServices.AmiiboViewMode };

            HookPointerEvents();

            adapters.Snapshot.Changed += OnStateChanged;
            adapters.Connection.Changed += OnStateChanged;
            library.Items.Changed += OnStateChanged;

            suppressSelection = true;
            SortBox.SelectedIndex = (int)filters.Sort;
            ViewSelector.SelectedIndex = (int)filters.View;
            suppressSelection = false;

            Render();
            ReportLibraryWarnings();

            // Fire and forget: the catalog is enrichment, so the page is fully
            // usable before it answers and simply gets better if it does. It
            // sends nothing about this user — see AmiiboCatalog.
            _ = catalog.EnsureLoadedAsync().ContinueWith(
                _ => AppServices.OnUiThread(Render), TaskScheduler.Default);

            if (adapters.Connection.Value.Phase == ConnectionPhase.Connected)
            {
                amiiboRead = true;
                await SafeAsync(() => adapters.RefreshAmiiboAsync());
            }
        });

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Connection.Changed -= OnStateChanged;
        library.Items.Changed -= OnStateChanged;

        UnhookPointerEvents();

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
    /// reporting that nothing had been read, with Reload as the only way out of
    /// a state the app could see for itself.
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

    // ------------------------------------------------------------- rendering

    /// <summary>
    /// Repaint everything. The browser collection is rebuilt only when the
    /// query result actually changed — see <see cref="RenderBrowser"/>.
    /// </summary>
    private void Render()
    {
        var view = View();
        RenderAdapter(view);
        RenderBrowser(view);
        RenderSelection(view);
    }

    /// <summary>
    /// Selection changed, and nothing else.
    /// </summary>
    /// <remarks>
    /// THE FIX FOR THE SCROLL-TO-TOP DEFECT. Clicking an item used to call the
    /// whole of <see cref="Render"/>, which reassigned the browser's ItemsSource;
    /// WinUI discards its container generation and scroll offset when that
    /// happens, so every click sent a 1000-item library back to the top.
    ///
    /// Selection cannot affect which items are shown or their order — the query
    /// record does not even carry a selection — so the browser is left untouched
    /// here and only the inspection pane is redrawn.
    /// </remarks>
    private void RenderAfterSelection()
    {
        var view = View();
        RenderSelection(view);
    }

    private void RenderAdapter(AmiiboView view)
    {
        SlotHeadline.Text = view.Available ? view.SlotHeadline : "Not connected";
        SlotDetail.Text = view.UnavailableReason ?? view.SlotDetail;

        DirtyBar.IsOpen = view.NeedsSync;
        KeyBar.IsOpen = view.KeyNotice is not null;
        KeyBar.Message = view.KeyNotice ?? "";

        KeyStatus.Text = keys.Exists
            ? "Stored and protected for this Windows account."
            : "Not imported. Names, series and artwork still work without them.";
        ForgetKeysButton.IsEnabled = keys.Exists;

        var busy = transfer is not null;
        RefreshButton.IsEnabled = view.Connected && !busy;

        // THE DEEPER CONTROLS APPEAR ONLY WHEN THEY MEAN SOMETHING. With nothing
        // connected the Adapter category says so in two lines and offers Reload;
        // it does not present five dead buttons. Visible-but-disabled is the
        // right treatment for a control the user is reaching for, not for every
        // command that happens to exist.
        AdapterControls.Visibility = view.Available ? Visibility.Visible : Visibility.Collapsed;

        PresentButton.IsEnabled = view.CanPresent && !busy;
        EjectButton.IsEnabled = view.CanEject && !busy;
        SyncButton.IsEnabled = view.CanSync && !busy;
        ClearButton.IsEnabled = view.CanClear && !busy;

        CopySection.Visibility = view.CanChooseCopy ? Visibility.Visible : Visibility.Collapsed;
        CopyLabel.Text = view.CopyLabel;
        UseOriginalButton.IsEnabled = view.CanChooseCopy && view.Status.UsingSave2 && !busy;
        UseConsoleCopyButton.IsEnabled = view.CanChooseCopy && !view.Status.UsingSave2 && !busy;

        RenderTransfer();
    }

    /// <summary>
    /// Choose between the side-by-side and overlay layouts.
    /// </summary>
    /// <remarks>
    /// A FIXED-WIDTH INSPECTOR DOES NOT FIT EVERY WINDOW. At 200% scaling on a
    /// 1080p display this page gets roughly 640 logical points of content width,
    /// and 420 of inspector plus the browser's minimum simply pushed the
    /// inspector — and the last filter with it — off the right-hand edge.
    ///
    /// Wide enough for both: an OPEN inspector sits beside the browser. Too
    /// narrow: it overlays instead, so the library underneath keeps its full
    /// width rather than being squeezed to nothing.
    ///
    /// WIDTH DECIDES WHERE THE INSPECTOR GOES, NEVER WHETHER IT IS THERE. It
    /// used to be permanently present on any window past the threshold, which
    /// reserved 420 points for a pane describing whatever happened to be
    /// highlighted, whether or not anyone had asked to see it. Browsing gets the
    /// whole surface until the user explicitly inspects something.
    /// </remarks>
    private void OnContentSizeChanged(object sender, SizeChangedEventArgs e)
    {
        var wide = e.NewSize.Width >= SideBySideMinimumWidth;
        if (wide == sideBySide && initialisedLayout)
        {
            return;
        }

        initialisedLayout = true;
        sideBySide = wide;
        ApplyLayout();
    }

    private void ApplyLayout()
    {
        var open = interaction.InspectorOpen;

        // Closed is the resting state, and it costs the browser nothing: a
        // zero-width column, not a hidden pane still holding its space.
        if (!open)
        {
            InspectorColumn.Width = new GridLength(0);
            Inspector.Visibility = Visibility.Collapsed;
        }
        else if (sideBySide)
        {
            Grid.SetColumn(Inspector, 1);
            Grid.SetColumnSpan(Inspector, 1);
            InspectorColumn.Width = GridLength.Auto;
            Inspector.Visibility = Visibility.Visible;
        }
        else
        {
            // Overlay: spans both columns and hugs the right edge.
            Grid.SetColumn(Inspector, 0);
            Grid.SetColumnSpan(Inspector, 2);
            InspectorColumn.Width = new GridLength(0);
            Inspector.Visibility = Visibility.Visible;
        }

        // Always closable, at every width. A pane with no way out is a pane that
        // is pinned for the rest of the session.
        CloseInspectorButton.Visibility = Visibility.Visible;

        // The discoverable route to details, and the accessible one: not
        // everybody knows a double click opens something, and a keyboard or
        // screen-reader user has no double click to give.
        DetailsButton.Visibility = open || interaction.Selecting
            ? Visibility.Collapsed
            : Visibility.Visible;
        DetailsButton.IsEnabled = interaction.FocusedId is not null;

        SelectButton.Visibility = interaction.Selecting
            ? Visibility.Collapsed
            : Visibility.Visible;
        SelectButton.IsEnabled = interaction.FocusedId is not null;

        RenderSelectionBar();
    }

    /// <summary>
    /// The bulk-action bar: what is selected, and the two things that act on it.
    /// </summary>
    /// <remarks>
    /// Deliberately two commands. Multi-selection existing is not a reason to
    /// invent a dozen batch operations, and a crowded bar makes the destructive
    /// pair harder to aim at rather than easier.
    /// </remarks>
    private void RenderSelectionBar()
    {
        SelectionBar.Visibility = interaction.Selecting
            ? Visibility.Visible
            : Visibility.Collapsed;

        if (!interaction.Selecting)
        {
            return;
        }

        var hidden = interaction.HiddenSelectedCount(tiles.Select(tile => tile.Id));

        // States the hidden ones up front rather than only in the confirmation:
        // a count that silently disagrees with what is on screen is how somebody
        // deletes more than they meant to.
        SelectionCount.Text = hidden == 0
            ? $"{interaction.SelectedCount} selected"
            : $"{interaction.SelectedCount} selected ({hidden} hidden by filters)";

        BulkInitializeButton.IsEnabled = !dialogOpen;
        BulkDeleteButton.IsEnabled = !dialogOpen;
    }

    /// <summary>
    /// Apply an interaction transition and reflect it in the controls.
    /// </summary>
    /// <remarks>
    /// THE ONE PLACE THE UI LEARNS ABOUT INTERACTION STATE. Every gesture handler
    /// computes a next state with <see cref="AmiiboInteraction"/> and hands it
    /// here; none of them touch a control directly. That is what keeps the
    /// behaviour identical to Android's and testable without a window.
    ///
    /// Nothing here reassigns an ItemsSource, so no transition can move the
    /// browser's scroll position.
    /// </remarks>
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

        RenderAfterSelection();
    }

    /// <summary>Push the bulk set onto the tiles, in place.</summary>
    private void RenderTicks()
    {
        var selecting = interaction.Selecting;
        foreach (var tile in tiles)
        {
            tile.Selectable = selecting;
            tile.Ticked = selecting && interaction.IsSelected(tile.Id);
        }
    }

    private void OnCloseInspector(object sender, RoutedEventArgs e) =>
        SetInteraction(AmiiboInteraction.CloseInspector(interaction));

    /// <summary>
    /// The explicit "Details" command, and the accessible route to inspection.
    /// </summary>
    private void OnShowDetails(object sender, RoutedEventArgs e)
    {
        if (interaction.FocusedId is { } id)
        {
            SetInteraction(AmiiboInteraction.OpenInspector(interaction, id));
        }
    }

    /// <summary>
    /// The explicit "Select" command: multi-selection without knowing a gesture.
    /// </summary>
    /// <remarks>
    /// Ctrl+click and a touch long press are both invisible affordances. Neither
    /// is available to somebody driving the page from the keyboard or a screen
    /// reader, and neither is discoverable by somebody who has not been told, so
    /// the same transition gets a button.
    /// </remarks>
    private void OnStartSelection(object sender, RoutedEventArgs e)
    {
        if (interaction.FocusedId is { } id)
        {
            SetInteraction(AmiiboInteraction.EnterSelection(interaction, id));
        }
    }

    private void OnCancelSelection(object sender, RoutedEventArgs e) =>
        SetInteraction(AmiiboInteraction.ClearSelection(interaction));

    /// <summary>
    /// A double click asks to inspect. A single one never does.
    /// </summary>
    /// <remarks>
    /// The gesture arrives after the click that focused the item, so the target
    /// is already the focused one. Refused during selection mode by the domain,
    /// so a double tap while ticking cannot both toggle and open something.
    /// </remarks>
    private void OnLibraryDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if (TileUnder(e.OriginalSource) is not { } tile)
        {
            return;
        }

        e.Handled = true;
        SetInteraction(AmiiboInteraction.OpenInspector(interaction, tile.Id));
    }

    /// <summary>
    /// Touch long press: the canonical way into multi-selection.
    /// </summary>
    /// <remarks>
    /// Windows raises Holding for touch and pen only — a held mouse button does
    /// not produce it — which is exactly the split wanted here. Mouse users get
    /// Ctrl+click and the Select button; touch users get the gesture they expect
    /// from every other gallery on the platform.
    /// </remarks>
    private void OnLibraryHolding(object sender, HoldingRoutedEventArgs e)
    {
        if (e.HoldingState != Microsoft.UI.Input.HoldingState.Started ||
            TileUnder(e.OriginalSource) is not { } tile)
        {
            return;
        }

        e.Handled = true;
        SetInteraction(AmiiboInteraction.EnterSelection(interaction, tile.Id));
    }

    /// <summary>
    /// Escape: cancel a selection, or close the inspector, in that order.
    /// </summary>
    /// <remarks>
    /// Left unhandled when neither is open, so the key keeps whatever meaning
    /// the surrounding shell gives it rather than being silently swallowed.
    /// </remarks>
    private void OnPageKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != global::Windows.System.VirtualKey.Escape)
        {
            return;
        }

        var next = AmiiboInteraction.Escape(interaction);
        if (next == interaction)
        {
            return;
        }

        e.Handled = true;
        SetInteraction(next);
    }

    /// <summary>The tile a pointer event landed on, if it landed on one.</summary>
    private static AmiiboTile? TileUnder(object? source) =>
        (source as FrameworkElement)?.DataContext as AmiiboTile;

    /// <summary>Whether Ctrl is down right now.</summary>
    /// <remarks>
    /// Read at the moment the selection changes rather than tracked, because a
    /// key can be released while a dialog or another window has focus and a
    /// tracked flag would then be stuck on.
    /// </remarks>
    private static bool ControlHeld() =>
        InputKeyboardSource
            .GetKeyStateForCurrentThread(global::Windows.System.VirtualKey.Control)
            .HasFlag(global::Windows.UI.Core.CoreVirtualKeyStates.Down);

    /// <summary>Switch inspector category. Touches no query state.</summary>
    private void OnCategoryChanged(object sender, RoutedEventArgs e)
    {
        category = sender switch
        {
            var button when ReferenceEquals(button, TagTab) => InspectorCategory.Tag,
            var button when ReferenceEquals(button, AdapterTab) => InspectorCategory.Adapter,
            _ => InspectorCategory.Overview,
        };

        RenderSelection(View());
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

    /// <summary>
    /// The library browser: the item collection, the three views, the summary.
    /// </summary>
    /// <remarks>
    /// THE ITEMS SOURCE IS REPLACED ONLY WHEN THE RESULT ACTUALLY CHANGED. Every
    /// adapter status tick calls Render, and reassigning on each one would
    /// discard the scroll position just as the old selection path did. The
    /// projected sequence is compared against the last, and an unchanged result
    /// leaves the controls alone entirely.
    ///
    /// The signature covers identity AND badge, so "this tag is now on the
    /// adapter" still repaints while a mere re-render does not.
    /// </remarks>
    private void RenderBrowser(AmiiboView view)
    {
        var cards = AmiiboGallery.Build(
            view.Library, catalog.Find, view.LoadedFromLibrary?.Id, filters);

        // Identity, badge AND the catalog-derived fields the tiles display.
        //
        // Id and badge alone were not enough, and the failure was visible: the
        // catalog arrives asynchronously, so the first projection has no names
        // and no artwork. When it landed the sequence was unchanged, the rebuild
        // was skipped, and every tile kept its placeholder while the inspector —
        // which reads the catalog directly — showed the real image.
        var signature = string.Join(
            "|", cards.Select(card =>
                $"{card.Id}:{card.Badge}:{card.Title}:{card.ImageUrl}"));

        if (signature != browserSignature)
        {
            browserSignature = signature;
            tiles = [.. cards.Select(card => new AmiiboTile(
                card.Id,
                card.Title,
                card.Subtitle,
                card.Badge,
                () => Artwork(card.ImageUrl),
                series: card.GameSeries,
                collection: card.AmiiboSeries,
                released: card.ReleaseDate,
                figureId: card.FigureId))];

            suppressSelection = true;
            foreach (var host in Hosts)
            {
                host.ItemsSource = tiles;
            }

            suppressSelection = false;
            RestoreSelection();

            // The tiles are NEW OBJECTS, and a fresh tile is unticked. Without
            // this, a rebuild silently emptied the visible selection while the
            // count in the bar still said four — and rebuilds happen on their
            // own, whenever the catalog fetch lands and changes the signature.
            // The set itself was never lost; only its rendering was, which is
            // the more dangerous of the two failures because the user is looking
            // at the rendering when they press Delete.
            RenderTicks();
        }

        RenderViewMode();
        RenderFilterOptions(view);

        var total = view.Library.Count;
        LibrarySummary.Text = total == 0
            ? "No Amiibo yet"
            : cards.Count == total
                ? $"{total} Amiibo"
                : $"{cards.Count} of {total} Amiibo";

        // Distinguishes "you have nothing" from "your filter matched nothing",
        // which need completely different actions from the user.
        EmptyLibraryText.Visibility = tiles.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        EmptyLibraryText.Text = total == 0
            ? "Import a .bin dump, a folder of them, or a library ZIP — including one " +
              "exported from the phone app or another tool."
            : "Nothing matches. Clear the search and filters to see the whole library.";

        ExportArchiveButton.IsEnabled = total > 0;
        ClearFiltersButton.IsEnabled = filters.Any;
    }

    /// <summary>Every control that can host the library. One items source.</summary>
    private IEnumerable<ListViewBase> Hosts => [LibraryGrid, LibraryList, LibraryCarousel];

    private ListViewBase ActiveHost => filters.View switch
    {
        AmiiboViewMode.Carousel => LibraryCarousel,
        AmiiboViewMode.List => LibraryList,
        _ => LibraryGrid,
    };

    /// <summary>
    /// Show the chosen view, hide the other two.
    /// </summary>
    /// <remarks>
    /// Collapsed rather than unloaded, and all three share one items source, so
    /// switching view neither re-runs the query nor loses the selection. Only
    /// the visible one realises containers, so the hidden two cost nothing —
    /// which is what keeps a 1000-item carousel affordable.
    /// </remarks>
    private void RenderViewMode()
    {
        foreach (var host in Hosts)
        {
            host.Visibility = ReferenceEquals(host, ActiveHost)
                ? Visibility.Visible
                : Visibility.Collapsed;
        }

        if (ViewSelector.SelectedIndex != (int)filters.View)
        {
            suppressSelection = true;
            ViewSelector.SelectedIndex = (int)filters.View;
            suppressSelection = false;
        }
    }

    /// <summary>
    /// Point every host at the selected id, without raising selection events.
    /// </summary>
    /// <remarks>
    /// All three are kept in step so switching view lands on the same Amiibo
    /// rather than on whatever the other control last highlighted.
    /// </remarks>
    private void RestoreSelection()
    {
        var tile = tiles.FirstOrDefault(candidate => candidate.Id == selectedId);
        suppressSelection = true;
        foreach (var host in Hosts)
        {
            host.SelectedItem = tile;
        }

        suppressSelection = false;
    }

    /// <summary>
    /// Populate the filter dropdowns from what the user actually owns.
    /// </summary>
    /// <remarks>
    /// A dropdown listing every Amiibo series in existence when the library holds
    /// three figures is a worse control than one listing those three's series.
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
    /// The inspection pane: who this is, what is on the tag, and what can be done.
    /// </summary>
    private void RenderSelection(AmiiboView view)
    {
        var item = view.Selected;
        var chosen = item is not null;

        SelectionEmpty.Visibility = chosen ? Visibility.Collapsed : Visibility.Visible;
        HeroPanel.Visibility = chosen ? Visibility.Visible : Visibility.Collapsed;
        PrimaryActionPanel.Visibility = chosen ? Visibility.Visible : Visibility.Collapsed;
        SecondaryActionPanel.Visibility = chosen ? Visibility.Visible : Visibility.Collapsed;
        CategoryBar.Visibility = chosen ? Visibility.Visible : Visibility.Collapsed;

        // The Adapter category is about the adapter, not the selection, so it
        // stays reachable with nothing selected.
        OverviewTab.IsChecked = category == InspectorCategory.Overview;
        TagTab.IsChecked = category == InspectorCategory.Tag;
        AdapterTab.IsChecked = category == InspectorCategory.Adapter;
        AdapterPanel.Visibility =
            category == InspectorCategory.Adapter ? Visibility.Visible : Visibility.Collapsed;

        if (!chosen)
        {
            DetailHost.Children.Clear();
            return;
        }

        var entry = catalog.Find(item!.FigureId);
        var card = tiles.FirstOrDefault(tile => tile.Id == item.Id);

        SelectedTitle.Text = card?.Title ?? item.DisplayName;
        SelectedSubtitle.Text = card?.Subtitle ?? item.FigureId;
        SelectedAdapterState.Text = view.LoadedFromLibrary?.Id == item.Id
            ? view.NeedsSync ? "On the adapter · changed by the console" : "On the adapter"
            : "Not on the adapter";

        var image = Artwork(entry?.ImageUrl ?? "");
        SelectedArtwork.Source = image;
        SelectedArtworkPlaceholder.Visibility =
            image is null ? Visibility.Visible : Visibility.Collapsed;

        var busy = transfer is not null;
        SendButton.IsEnabled = view.CanUpload && !busy;

        // The primary action stays visible-but-disabled with its reason, because
        // it is the control the user is reaching for. That is different from the
        // deeper adapter commands, which are hidden until they mean something.
        SendReason.Visibility = view.CanUpload || view.UnavailableReason is null
            ? Visibility.Collapsed
            : Visibility.Visible;
        SendReason.Text = view.NeedsSync
            ? "Sync the adapter's changed Amiibo first."
            : view.UnavailableReason ?? "";

        // Local actions are never gated on a connection.
        RenameButton.IsEnabled = true;
        ExportButton.IsEnabled = true;
        DeleteItem.IsEnabled = true;
        InitializeItem.IsEnabled = keys.Exists;

        RenderDetailGroups(item, entry, view);
    }

    /// <summary>
    /// The rows for the selected category, as a dense aligned grid.
    /// </summary>
    /// <remarks>
    /// ONE CATEGORY AT A TIME. Rendering all of them into a single column is
    /// what turned this pane into a scrolling form; Overview and Tag now hold
    /// only their own rows, and the Adapter category is a separate panel.
    ///
    /// Group headings are dropped when the category IS the group — repeating
    /// "Identity" under an "Overview" tab that contains nothing else is noise.
    /// </remarks>
    private void RenderDetailGroups(
        AmiiboLibraryItem item, AmiiboCatalogEntry? entry, AmiiboView view)
    {
        DetailHost.Children.Clear();
        if (category == InspectorCategory.Adapter)
        {
            return;
        }

        var groups = AmiiboInspection.Build(
            item,
            entry,
            SelectedDetails(item),
            onAdapter: view.LoadedFromLibrary?.Id == item.Id,
            adapterChanged: view.NeedsSync && view.LoadedFromLibrary?.Id == item.Id);

        var wanted = category == InspectorCategory.Overview
            ? new[] { "Identity" }
            : ["Tag", "Registration", "Game data"];

        var shown = groups.Where(group => wanted.Contains(group.Title)).ToList();
        var headings = shown.Count > 1;

        foreach (var group in shown)
        {
            if (headings)
            {
                DetailHost.Children.Add(new TextBlock
                {
                    Text = group.Title,
                    Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
                    Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
                    Margin = new Thickness(0, 8, 0, 2),
                });
            }

            foreach (var row in group.Rows)
            {
                DetailHost.Children.Add(DetailRow(row));
            }
        }
    }

    /// <summary>One aligned label/value line.</summary>
    /// <remarks>
    /// A fixed label column so every value starts at the same x — the previous
    /// flat list let each row find its own alignment, which is most of why it
    /// read as a slab.
    /// </remarks>
    private static Grid DetailRow(AmiiboDetailRow row)
    {
        // Two columns, a fixed label width, and tight vertical padding. The
        // previous version gave every field the height of a settings row, which
        // is most of why six facts filled a screen.
        var grid = new Grid { ColumnSpacing = 12, Padding = new Thickness(0, 3, 0, 3) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(96) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var label = new TextBlock
        {
            Text = row.Label,
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
            VerticalAlignment = VerticalAlignment.Top,
        };
        Grid.SetColumn(label, 0);
        grid.Children.Add(label);

        var value = new TextBlock
        {
            Text = row.Value,
            TextWrapping = TextWrapping.Wrap,
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
        };

        if (row.Monospace)
        {
            // Identifiers a person compares character by character.
            value.FontFamily = new FontFamily("Consolas");
        }

        Grid.SetColumn(value, 1);
        grid.Children.Add(value);
        return grid;
    }

    /// <summary>
    /// Decrypt the selected backup's register block, when keys allow it.
    /// </summary>
    /// <remarks>
    /// Recomputed rather than cached: the keys can be imported or forgotten while
    /// this page is open, and a cached "no keys" result would leave the pane
    /// empty until the user navigated away and back.
    /// </remarks>
    private AmiiboDetails? SelectedDetails(AmiiboLibraryItem item)
    {
        try
        {
            return AmiiboCrypto.ReadDetails(library.Bytes(item.Id), keys.Read());
        }
        catch (Exception)
        {
            // A missing or unreadable image is already reported by the library's
            // own recovery pass; the pane simply has nothing decoded to show.
            return null;
        }
    }

    /// <summary>
    /// Resolve a catalog image URL, once per address.
    /// </summary>
    /// <remarks>
    /// Cached so a re-render does not re-request hundreds of images.
    /// <see cref="BitmapImage"/> fetches asynchronously and stays blank on
    /// failure, which is right for decoration: the tile falls back to its
    /// placeholder glyph, and no artwork failure can block the library, the
    /// selection, an import or any adapter action.
    ///
    /// Decoded at tile width rather than full resolution, which is what keeps a
    /// thousand-item library affordable.
    ///
    /// This is the one thing on the page that contacts a host per FIGURE rather
    /// than once for the whole catalog, so it does reveal which figures are in
    /// the library to whoever serves the artwork. Routed through here precisely
    /// so a future "load online artwork" setting can switch it off in one place
    /// without touching the page.
    /// </remarks>
    private ImageSource? Artwork(string url)
    {
        if (url.Length == 0 || !AppServices.OnlineArtworkEnabled)
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

        // Bounded, because a library of a thousand figures browsed end to end
        // would otherwise hold a thousand decoded bitmaps for the life of the
        // page. Clearing wholesale rather than evicting least-recently-used: the
        // bytes are still in the HTTP cache, so a rebuild is cheap, and a real
        // LRU here would be machinery guarding something that costs nothing to
        // recreate.
        if (artwork.Count >= MaximumCachedArtwork)
        {
            artwork.Clear();
        }

        var image = new BitmapImage(uri) { DecodePixelWidth = 160 };
        artwork[url] = image;
        return image;
    }

    private AmiiboView View() => AmiiboView.From(
        adapters.Snapshot.Value,
        adapters.Connection.Value.Phase == ConnectionPhase.Connected,
        library.Items.Value,
        keys.Exists,
        selectedId,
        transfer: transfer);

    // ---------------------------------------------------------------- browsing

    private void OnLibrarySelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressSelection || sender is not ListViewBase host)
        {
            return;
        }

        // Only the visible view drives the selection. The other two are kept in
        // step by RestoreSelection and must not answer back.
        if (!ReferenceEquals(host, ActiveHost))
        {
            return;
        }

        // A POINTER IS HANDLED BY Tapped, NOT HERE, and the two must not both
        // act: in selection mode each would toggle the same item, cancelling
        // out. SelectionChanged is left to do the job Tapped cannot — keyboard
        // and programmatic focus moves, which raise no tap at all.
        if (pointerDown)
        {
            return;
        }

        if (host.SelectedItem is not AmiiboTile tile)
        {
            // The host cleared its own selection — a filter hid the focused row,
            // say. Focus follows, but nothing opens and nothing is ticked.
            SetInteraction(interaction with { FocusedId = null, InspectedId = null });
            return;
        }

        SetInteraction(AmiiboInteraction.Activate(interaction, tile.Id));
    }

    /// <summary>
    /// Every pointer interaction with a tile, including the ones the host does
    /// not consider a change of selection.
    /// </summary>
    /// <remarks>
    /// THE DEFECT THIS FIXES. Selection used to be driven from SelectionChanged,
    /// which WinUI raises only when the host's OWN selection actually moves.
    /// Clicking the tile that was already highlighted raised nothing, so in
    /// selection mode that one tile could be ticked but never un-ticked, and
    /// Ctrl+clicking the highlighted tile could not start a selection either.
    /// Tapped fires for every tap, whether or not the highlight moves.
    ///
    /// CTRL+CLICK IS THE DESKTOP EQUIVALENT OF A LONG PRESS: it starts a
    /// selection from nothing and toggles thereafter. A plain tap is Activate,
    /// which browses in normal mode and toggles once a selection exists — the
    /// same single rule Android's tap follows.
    /// </remarks>
    private void OnLibraryTapped(object sender, TappedRoutedEventArgs e)
    {
        pointerDown = false;

        if (TileUnder(e.OriginalSource) is not { } tile)
        {
            return;
        }

        SetInteraction(ControlHeld()
            ? AmiiboInteraction.ToggleSelection(interaction, tile.Id)
            : AmiiboInteraction.Activate(interaction, tile.Id));
    }

    /// <summary>
    /// Subscribe to the pointer events the item containers swallow.
    /// </summary>
    /// <remarks>
    /// THE SECOND HALF OF THE SAME DEFECT, and it cannot be expressed in XAML.
    /// A GridViewItem marks Tapped and PointerPressed as HANDLED when the tap
    /// changes its own selection, and a handler attached the ordinary way — an
    /// attribute in the markup — never sees a handled event. So a tap on a tile
    /// that moved the highlight did nothing at all, while a tap on the selection
    /// marker (which is not part of the item's selection chrome, so nothing
    /// handled it) worked. Selecting appeared to require hitting a 20-point
    /// circle.
    ///
    /// AddHandler with handledEventsToo is the only way to see them, and it has
    /// to be code because XAML attribute syntax cannot pass that flag.
    /// </remarks>
    private void HookPointerEvents()
    {
        foreach (var host in Hosts)
        {
            host.AddHandler(TappedEvent, new TappedEventHandler(OnLibraryTapped), true);
            host.AddHandler(
                DoubleTappedEvent, new DoubleTappedEventHandler(OnLibraryDoubleTapped), true);
            host.AddHandler(
                PointerPressedEvent, new PointerEventHandler(OnLibraryPointerPressed), true);
            host.AddHandler(
                PointerCaptureLostEvent, new PointerEventHandler(OnLibraryPointerLost), true);
        }
    }

    private void UnhookPointerEvents()
    {
        foreach (var host in Hosts)
        {
            host.RemoveHandler(TappedEvent, (TappedEventHandler)OnLibraryTapped);
            host.RemoveHandler(DoubleTappedEvent, (DoubleTappedEventHandler)OnLibraryDoubleTapped);
            host.RemoveHandler(PointerPressedEvent, (PointerEventHandler)OnLibraryPointerPressed);
            host.RemoveHandler(
                PointerCaptureLostEvent, (PointerEventHandler)OnLibraryPointerLost);
        }
    }

    /// <summary>
    /// Marks the window between a press and its tap, during which the host's own
    /// SelectionChanged must stay out of the way.
    /// </summary>
    private void OnLibraryPointerPressed(object sender, PointerRoutedEventArgs e) =>
        pointerDown = true;

    /// <summary>
    /// A press that never became a tap — a drag, or a capture stolen by a
    /// scroll. Without this the flag would stick on and the keyboard would stop
    /// moving focus.
    /// </summary>
    private void OnLibraryPointerLost(object sender, PointerRoutedEventArgs e) =>
        pointerDown = false;

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

    /// <summary>
    /// Change how the library is presented. Never what it contains.
    /// </summary>
    /// <remarks>
    /// The query is untouched, so search, filters, sort and selection all
    /// survive. Persisted, because which view someone prefers is a stable
    /// preference and re-choosing it on every launch is friction.
    /// </remarks>
    private void OnViewModeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressSelection || ViewSelector.SelectedIndex < 0)
        {
            return;
        }

        filters = filters with { View = (AmiiboViewMode)ViewSelector.SelectedIndex };
        AppServices.AmiiboViewMode = filters.View;

        RenderViewMode();
        RestoreSelection();

        // Land on the selected item in the mode just switched to, which matters
        // most for the carousel.
        if (tiles.FirstOrDefault(tile => tile.Id == selectedId) is { } tile)
        {
            ActiveHost.ScrollIntoView(tile);
        }
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
        // Sort and view survive: they are preferences, not narrowing, and
        // resetting them would be an unasked-for change.
        filters = new AmiiboGalleryFilters
        {
            Sort = filters.Sort,
            Descending = filters.Descending,
            View = filters.View,
        };

        suppressSelection = true;
        SearchBox.Text = "";
        suppressSelection = false;
        Render();
    }

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
    /// Refused while the adapter holds unsynced console writes — the view
    /// disables the button and the client refuses independently — so this cannot
    /// be the path that loses a save.
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
    /// library write would clear it on data saved nowhere — and a crash in
    /// between would lose the save silently.
    ///
    /// The acknowledge is generation- and CRC-guarded, so if the console wrote
    /// again during the read it is refused and the tag stays dirty. That is
    /// correct: the bytes just stored are real, they are simply no longer the
    /// latest.
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
            interaction = AmiiboInteraction.Focus(interaction, stored.Id);

            try
            {
                await adapters.AcknowledgeAmiiboDownloadAsync(download);
                Report($"'{stored.DisplayName}' synced to your library.", InfoBarSeverity.Success);
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
    /// One control for every kind of file: the picker is multi-select and takes
    /// dumps and ZIPs together, and the library works out which is which.
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

    /// <summary>Import everything under a folder, including subfolders.</summary>
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

            // The plain filesystem API rather than the StorageFolder tree: it
            // recurses in one call and does not cost a WinRT round trip per
            // file, which matters at a few hundred of them.
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
            interaction = AmiiboInteraction.Focus(interaction, result.Imported[0].Id);
        }

        Report(
            result.Problems.Count == 0
                ? result.Summary
                : $"{result.Summary} See Diagnostics for what was skipped.",
            result.Imported.Count > 0 ? InfoBarSeverity.Success : InfoBarSeverity.Informational);

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
    /// Delete a backup. Destructive, and confirmed.
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

            // The neighbour that took its place keeps the user where they were.
            interaction = AmiiboInteraction.AfterRemoval(
                interaction, [.. tiles.Select(tile => tile.Id)], [item.Id]);
            library.Delete(item.Id);
            Render();
        });

    // ------------------------------------------------------------------ bulk

    /// <summary>
    /// Delete every selected backup, behind ONE confirmation.
    /// </summary>
    /// <remarks>
    /// Twelve individual dialogs is not twelve times the safety; it is a prompt
    /// the user learns to dismiss without reading, which is strictly worse than
    /// one they actually stop at.
    ///
    /// LOCAL ONLY, exactly like the single-item Delete. The adapter is a separate
    /// place with a separate command, and a batch that quietly cleared the
    /// resident tag as well would be doing something nobody asked for.
    /// </remarks>
    private async void OnBulkDelete(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            var chosen = interaction.Selection.ToArray();
            if (chosen.Length == 0)
            {
                return;
            }

            if (!await ConfirmAsync(
                    $"Delete {chosen.Length} Amiibo from your library?",
                    HiddenNote(chosen) +
                    "This removes your only copy of these backups. It cannot be undone, " +
                    "and the Amiibo currently on the adapter is not affected.",
                    $"Delete {chosen.Length}"))
            {
                return;
            }

            var outcome = Batch(chosen, id => library.Delete(id));

            // Settle BEFORE the library reload lands, so the focus lands on the
            // neighbour of what was removed rather than wherever a rebuild puts
            // it.
            interaction = AmiiboInteraction.AfterRemoval(
                interaction, [.. tiles.Select(tile => tile.Id)], outcome.Succeeded);

            Report(
                outcome.Summary("deleted") + "; the adapter was not changed",
                outcome.AnyFailed ? InfoBarSeverity.Warning : InfoBarSeverity.Success);
            ReportFailures(outcome);
            Render();
        });

    /// <summary>
    /// Initialize every selected backup, behind ONE confirmation.
    /// </summary>
    /// <remarks>
    /// ENTIRELY LOCAL, and deliberately so: the single-item Initialize rewrites
    /// the stored bytes and never touches the adapter, and a batch has no reason
    /// to behave differently. No adapter traffic is introduced here.
    ///
    /// Per-item failures are expected rather than exceptional — a dump the
    /// imported key cannot verify will refuse to re-sign — so each entry is
    /// attempted independently and the ones that worked are kept. Reporting the
    /// batch as a whole success would hide precisely the tags that still carry
    /// somebody else's registration.
    /// </remarks>
    private async void OnBulkInitialize(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            var chosen = interaction.Selection.ToArray();
            if (chosen.Length == 0)
            {
                return;
            }

            var retail = keys.Read();
            if (retail is null)
            {
                Report("Import your Amiibo keys before initializing tags.",
                       InfoBarSeverity.Warning);
                return;
            }

            if (!await ConfirmAsync(
                    $"Initialize {chosen.Length} Amiibo?",
                    HiddenNote(chosen) +
                    "The owner, nickname, registration dates and any game data are erased " +
                    "from the selected backups, and each is re-signed in place. The figures " +
                    "themselves are unchanged, and so is the adapter. This cannot be undone — " +
                    "export a copy first if you might want those saves back.",
                    $"Initialize {chosen.Length}"))
            {
                return;
            }

            var outcome = Batch(chosen, id =>
                library.UpdateFromAdapter(id, AmiiboCrypto.Initialize(library.Bytes(id), retail)));

            interaction = AmiiboInteraction.ClearSelection(interaction);

            Report(
                outcome.Summary("initialized"),
                outcome.AnyFailed ? InfoBarSeverity.Warning : InfoBarSeverity.Success);
            ReportFailures(outcome);
            Render();
        });

    /// <summary>
    /// Run one operation over a set, keeping what worked and recording what did
    /// not.
    /// </summary>
    /// <remarks>
    /// No transaction and no rollback: undoing a successful initialize would
    /// mean restoring bytes that have already been overwritten, and undoing a
    /// delete means the same. Partial progress is the honest outcome, so it is
    /// the reported one.
    /// </remarks>
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

    /// <summary>Name the failures, so "3 failed" is actionable.</summary>
    private void ReportFailures(AmiiboBulkOutcome outcome)
    {
        foreach (var failure in outcome.Failed.Take(3))
        {
            adapters.Diagnostics.Warn("amiibo", $"{failure.Name}: {failure.Reason}");
        }
    }

    /// <summary>
    /// Says out loud how many of the doomed entries are not on screen.
    /// </summary>
    /// <remarks>
    /// Selection survives a filter change on purpose, so the set can legitimately
    /// contain entries the current query hides. A confirmation that named only
    /// the visible ones would be understating what is about to happen.
    /// </remarks>
    private string HiddenNote(IReadOnlyList<string> chosen)
    {
        var visible = tiles.Select(tile => tile.Id).ToHashSet(StringComparer.Ordinal);
        var hidden = chosen.Count(id => !visible.Contains(id));

        return hidden == 0
            ? ""
            : $"{hidden} of these are hidden by the current search or filters. ";
    }

    /// <summary>
    /// Wipe a backup's owner, nickname and game data, and re-sign it.
    /// </summary>
    /// <remarks>
    /// Materially destructive, and confirmed with what will actually be removed
    /// rather than a generic "are you sure". A tag someone else registered, or
    /// one whose save you want to start over, becomes a blank figure again.
    ///
    /// Rewrites the stored backup in place: there is no undo, and for a tag the
    /// physical figure has moved past, this copy may be the only record of that
    /// save. Export first is the honest advice and the dialog gives it.
    ///
    /// The crypto refuses to re-sign an image whose HMAC does not verify, and
    /// checks its own output really is empty before returning it, so a failure
    /// leaves the stored bytes untouched.
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
                Report("Import your Amiibo keys before initializing a tag.",
                       InfoBarSeverity.Warning);
                return;
            }

            if (!await ConfirmAsync(
                    $"Initialize '{item.DisplayName}'?",
                    "The owner, nickname, registration dates and any game data are erased, " +
                    "and the backup is re-signed in place. The figure itself is unchanged. " +
                    "This cannot be undone — export a copy first if you might want this " +
                    "save back.",
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
                Report($"That is not a usable Amiibo key file: {error.Message}",
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
    /// touch XAML — so it hops to the dispatcher before rendering anything.
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
    /// the same shape.
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
            Content = new TextBlock { Text = content, TextWrapping = TextWrapping.Wrap },
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
