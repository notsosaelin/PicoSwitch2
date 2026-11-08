using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using PicoSwitch.Companion.Linux.App.Shell;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.App.Pages;

/// <summary>
/// Keyboard and mouse mapping: the profiles the adapter applies to a Bluetooth
/// keyboard and mouse paired to it. A port of the WinUI KeyboardMousePage,
/// which carries the reasoning for every rule kept here; the comments below
/// are the ones a maintainer of THIS file needs.
///
/// TWO STORES, KEPT APART. The library (<see cref="KbmLibraryRepository"/>) is
/// the user's and works with nothing connected: open, edit, save, new,
/// duplicate, rename and delete never send a management command. The adapter's
/// bank is a small working set; only Assign, Activate, On startup, Copy to
/// library, Remove and the switch keys reach it.
///
/// No policy lives here. What is enabled, what each key says and what a slider's
/// bounds are come from <see cref="KeyboardMouseView"/>, shared with Windows.
/// </summary>
internal sealed class KeyboardMousePage : CompanionPage
{
    /// <summary>One key unit, in pixels, before the keyboard is scaled to fit.</summary>
    private const double KeyUnit = 46;

    /// <summary>The smallest key the short-window fit will draw: Android's MinKeyUnit.</summary>
    private const double MinKeyUnit = 28;

    /// <summary>Taken OUT of each key rather than added between them, so 2u spans 2u.</summary>
    private const double KeyGap = 4;

    /// <summary>How long a slider must be still before its value is sent.</summary>
    private const int SliderSettleMillis = 250;

    /// <summary>Matches NS2_KBM_PROFILE_NAME_MAX minus its NUL.</summary>
    private const int KbmProfileNameMax = 19;

    private readonly AdapterConnectionService adapters = AppServices.Adapters;
    private readonly KbmLibraryRepository library = AppServices.KbmLibrary;

    private readonly Border notReady;
    private readonly TextBlock notReadyTitle = Ui.Heading();
    private readonly TextBlock notReadyDetail = Ui.Text(secondary: true);
    private readonly Button notReadyRetry;
    private readonly Control body;

    private readonly ProgressBar busyBar = new() { IsIndeterminate = true, Width = 100, IsVisible = false };
    private readonly Button refreshButton;

    private readonly TextBlock modeText = Ui.Text();
    private readonly TextBlock devicesText = Ui.Caption();
    private readonly TextBlock disabledReason = Ui.Caption();
    private readonly TextBlock modeSeparator = Ui.Text("·", secondary: true);
    private readonly WrapPanel modeLine;
    private readonly ComboBox modeBox = new();
    private readonly Button modeApply;

    private readonly ComboBox layoutBox = new();
    private readonly ComboBox profileSelector = new();
    private readonly Button saveButton;
    private readonly Button discardButton;
    private readonly Button newButton;
    private readonly Button duplicateButton;
    private readonly Button renameButton;
    private readonly Button deleteButton;
    private readonly Notice draftBar = new(dismissible: false);
    private readonly Notice inactiveProfileBar = new(dismissible: false);

    private readonly TextBlock mappingSummary = Ui.Caption();
    private readonly Button resetProfile;
    private readonly StackPanel keyboardHost = new() { Orientation = Orientation.Horizontal, Spacing = 20 };
    private readonly WrapPanel otherKeysHost = new() { ItemSpacing = 6, LineSpacing = 6 };
    private readonly WrapPanel mouseButtonHost = new() { ItemSpacing = 6, LineSpacing = 6 };
    private readonly Control mouseButtonSection;
    private readonly RowStack undrawnHost = new();
    private readonly Control undrawnSection;

    private readonly TextBlock bankSummary = Ui.Caption();
    private readonly Button assignButton;
    private readonly RowStack bankHost = new();
    private readonly RowStack switchHost = new();

    // Mouse tuning in two groups, side by side when the card is wide enough
    // for two sliders: stick feel, and direction (anti-deadzone and invert).
    private readonly StackPanel feelHost = new() { Spacing = 14 };
    private readonly StackPanel directionHost = new() { Spacing = 14 };
    private readonly WrapPanel invertChips = new() { ItemSpacing = 8, LineSpacing = 8 };
    private readonly Grid tuningGrid = new() { ColumnSpacing = 28, RowSpacing = 14 };
    private readonly TextBlock mouseButtonsNote = Ui.Caption("Mouse buttons are mapped in the Keyboard and mouse layout.");
    private readonly Button resetAll;

    private readonly Expander countersExpander;
    private readonly TextBlock countersText = Ui.Mono();

    /// <summary>Below this page width the Profiles tab is one column.</summary>
    private const double StackProfilesBelow = 1100;

    /// <summary>The tab last shown, kept across visits like Windows'.</summary>
    private static int selectedTab;

    private readonly TabBar tabs = new(("Mapping", Icons.Keyboard), ("Mouse", Icons.Mouse), ("Profiles", Icons.List));
    private readonly Control mappingPanel;
    private readonly Grid mousePanel = new() { ColumnSpacing = 12, RowSpacing = 12 };
    private readonly Control mouseButtonsCard;
    private readonly Control tuningCard;
    private readonly Grid profilesPanel = new() { ColumnSpacing = 12, RowSpacing = 12 };
    private readonly Control libraryCard;
    private readonly Control resetCard;
    private readonly Control bankCard;
    private readonly Control switchCard;
    private readonly ToggleButton moreKeys = new() { Content = "More keys" };
    private readonly Control otherKeysSection;
    private readonly Control keyboardSection;
    private readonly Viewbox keyboardBox;
    private readonly Control mappingHeader;

    // The short-window arrangement moves these between slots (see
    // OnShortWindowChanged); a control has one parent, so each slot is a
    // ContentControl that is emptied before another takes the control.
    private readonly TextBlock pageTitle = Ui.Title("Keyboard and mouse");
    private readonly ContentControl headerTabsSlot = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly ContentControl toolbarLeft = new();
    private readonly ContentControl toolbarRight = new();
    private readonly ContentControl cardActionsSlot = new();
    private readonly Control openProfile;
    private readonly Control mappingActions;

    /// <summary>A header text line beside the tabs on a short window, before it trims.</summary>
    private const double ShortHeaderTextWidth = 200;

    /// <summary>A profile or layout selector in the short-window toolbar.</summary>
    private const double ShortSelectorWidth = 136;

    /// <summary>Below this page width the Mouse tab is one column.</summary>
    private const double StackMouseBelow = 760;

    /// <summary>Two slider columns of about 220 px each, plus the gap: Android's TuningTwoColumnWidth.</summary>
    private const double TuningTwoColumnWidth = 460;

    private KbmLayout profile = KbmLayout.Keyboard;

    /// <summary>Until the user picks a layout, the page follows the one the adapter is resolving.</summary>
    private bool profileChosenByUser;

    /// <summary>Suppresses selection events while Render() populates the selectors.</summary>
    private bool populating;

    /// <summary>The rows realised in the picker; rebuilt only when what it DISPLAYS differs.</summary>
    private IReadOnlyList<KbmSelectableProfile> renderedProfileRows = [];

    /// <summary>The selected LOCAL profile id, or null for the built-in template.</summary>
    private string? selectedLocalId;

    /// <summary>The local, unsaved copy of the open profile. Every edit mutates this and nothing else.</summary>
    private KbmLocalDraft? draft;

    private DispatcherTimer? mouseCommit;
    private KbmMouseField? pendingMouseField;
    private int pendingMouseValue;
    private KeyboardMouseView? view;

    public KeyboardMousePage(MainWindow shell)
        : base(shell)
    {
        refreshButton = Ui.IconButton(Icons.Refresh, "Reload from the adapter", async (_, _) => await GuardAsync(() => RunAsync(() => adapters.RefreshKeyboardMouseAsync())));
        notReadyRetry = Ui.Button("Reload", async (_, _) => await GuardAsync(() => RunAsync(() => adapters.RefreshKeyboardMouseAsync())), icon: Icons.Refresh);
        notReady = Ui.EmptyState(Icons.Keyboard, notReadyTitle, notReadyDetail, notReadyRetry);

        modeBox.ItemsSource = new[] { "Automatic", "Controller only", "Keyboard", "Keyboard and mouse" };
        modeLine = Ui.Row(modeText, modeSeparator, devicesText);
        modeApply = Ui.Button("Set", async (_, _) => await GuardAsync(OnSetModeAsync));

        layoutBox.ItemsSource = new[] { "Keyboard", "Keyboard and mouse" };
        layoutBox.SelectionChanged += (_, _) => OnLayoutChanged();
        AutomationProperties.SetName(layoutBox, "Layout");
        profileSelector.SelectionChanged += async (_, _) => await GuardAsync(OnSelectedProfileChangedAsync);
        AutomationProperties.SetName(profileSelector, "Profile");

        saveButton = Ui.Button("Save", async (_, _) => await GuardAsync(OnSaveProfileAsync), accent: true);
        discardButton = Ui.Button("Discard", (_, _) =>
        {
            // Local only: the draft never left this process.
            draft = draft?.Discard();
            Render();
        });
        newButton = Ui.Button("New", async (_, _) => await GuardAsync(OnNewProfileAsync));
        duplicateButton = Ui.Button("Duplicate", async (_, _) => await GuardAsync(OnDuplicateProfileAsync));
        renameButton = Ui.Button("Rename", async (_, _) => await GuardAsync(OnRenameProfileAsync));
        deleteButton = Ui.Button("Delete", async (_, _) => await GuardAsync(OnDeleteProfileAsync));

        resetProfile = Ui.Button("Reset mapping", async (_, _) => await GuardAsync(OnResetProfileAsync));
        assignButton = Ui.Button("Assign open profile", async (_, _) => await GuardAsync(OnAssignToAdapterAsync));
        resetAll = Ui.Button("Reset", async (_, _) => await GuardAsync(OnResetAllAsync));

        countersExpander = new Expander
        {
            Header = Ui.Caption("Diagnostics: runtime counters"),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            Content = countersText,
            IsVisible = false,
        };

        // The keyboard is drawn at full size and SCALED DOWN to fit, never up and
        // never scrolled sideways: a whole keyboard is the point of this page, and
        // on a narrow window a slightly smaller one beats half of one.
        keyboardBox = new Viewbox
        {
            Stretch = Stretch.Uniform,
            StretchDirection = StretchDirection.DownOnly,
            HorizontalAlignment = HorizontalAlignment.Left,
            Child = keyboardHost,
        };

        mouseButtonSection = Ui.Stack(8, mouseButtonsNote, mouseButtonHost);
        undrawnSection = Ui.Stack(8, SubHeading("Keys not on this keyboard"), undrawnHost);

        // MORE KEYS SWITCHES THE CARD to the ISO and international keys and any
        // bound key this build does not draw, rather than stacking them under the
        // keyboard: on a short window there is no height to share. Few keyboards
        // have those keys, so they wait behind the toggle; a binding the user
        // cannot see is one they cannot remove, so the toggle counts the hidden
        // ones (RenderMoreKeys).
        otherKeysSection = Ui.Body(Ui.Stack(
            12,
            Ui.Stack(8, SubHeading("ISO and international keys"), otherKeysHost),
            undrawnSection));
        otherKeysSection.IsVisible = false;
        keyboardSection = Ui.Body(keyboardBox);
        moreKeys.IsCheckedChanged += (_, _) => ShowMoreKeys();
        mappingActions = Ui.Row(moreKeys, resetProfile);

        // THE TOOLBAR (shell spec, "Keyboard & Mouse"): tabs on the left, the
        // OPEN PROFILE on the right. It shows on every tab because Mapping and
        // Profiles both act on it, and an unsaved edit must never be hidden by
        // a tab switch. Saved or not is said by the Save button itself.
        openProfile = Ui.Row(profileSelector, layoutBox, saveButton, discardButton);
        // Side by side while both fit; on a narrow window the open profile moves
        // under the tabs instead of wrapping into a column beside them.
        var toolbar = new SettingRow(toolbarLeft, toolbarRight, minimumLabelWidth: 360);

        mappingHeader = Ui.Setting(Ui.Text("Key mapping"), mappingSummary, cardActionsSlot);
        mappingPanel = Ui.Card(Ui.Stack(
            0,
            mappingHeader,
            keyboardSection,
            otherKeysSection));

        // MOUSE: the button bindings beside the tuning, a third and two thirds,
        // and the tuning in two columns of its own when it has the width.
        mouseButtonsCard = Ui.Card(Ui.Body(Ui.Stack(10, SubHeading("Mouse buttons"), mouseButtonSection)));
        tuningGrid.Children.AddRange([feelHost, directionHost]);
        tuningCard = Ui.Card(Ui.Body(Ui.Stack(10, SubHeading("Mouse tuning"), tuningGrid)));
        tuningGrid.SizeChanged += (_, e) => LayTuning(e.NewSize.Width >= TuningTwoColumnWidth);
        LayTuning(twoColumns: true);
        mousePanel.ColumnDefinitions = new ColumnDefinitions("1*,2*");
        mousePanel.RowDefinitions = new RowDefinitions("Auto,Auto");
        mousePanel.Children.AddRange([mouseButtonsCard, tuningCard]);
        LayMouse(stacked: false);
        mousePanel.SizeChanged += (_, e) => LayMouse(e.NewSize.Width < StackMouseBelow);

        // PROFILES. Two columns on a wide window so the tab fits without
        // scrolling: what you have (library, adapter positions) on the left, how
        // you switch and reset on the right. LayProfiles stacks them when narrow.
        libraryCard = Ui.Section(
            (string?)null,
            null,
            null,
            Ui.Setting("Your profiles", "Saved on this computer. Choose which one to edit at the top.",
                newButton, duplicateButton, renameButton, deleteButton));
        resetCard = Ui.Section(
            (string?)null,
            null,
            null,
            Ui.Setting("Reset everything", "Both layouts and mouse tuning go back to the adapter's defaults.", resetAll));

        // The library is the user's collection; the adapter's positions are the
        // small working set it can use with nothing connected. A position holds
        // a COPY, and only the buttons here send anything.
        bankCard = Ui.Section(Ui.Heading("On the adapter"), bankSummary, assignButton, bankHost);
        switchCard = Ui.Section(
            Ui.Heading("Profile switch keys"),
            Ui.Caption("Change profile without the app, in either layout."),
            null,
            switchHost);

        profilesPanel.ColumnDefinitions = new ColumnDefinitions("3*,2*");
        profilesPanel.RowDefinitions = new RowDefinitions("Auto,Auto,Auto,Auto,Auto");
        profilesPanel.Children.AddRange([libraryCard, resetCard, bankCard, switchCard, countersExpander]);
        LayProfiles(stacked: false);
        profilesPanel.SizeChanged += (_, e) => LayProfiles(e.NewSize.Width < StackProfilesBelow);

        tabs.SelectionChanged += index =>
        {
            selectedTab = index;
            ShowTab(ready: true);
        };

        body = Ui.Stack(
            Ui.Gap,
            toolbar,
            new Border { Child = Ui.Stack(8, draftBar, inactiveProfileBar) },
            mappingPanel,
            mousePanel,
            profilesPanel);

        // The header carries the mode on every tab: what the adapter is doing
        // with a keyboard and mouse, and the selector to change it. On a short
        // window the tabs join it in place of the title (OnShortWindowChanged).
        var header = new SettingRow(
            HeaderLabel(),
            Ui.Row(busyBar, modeBox, modeApply, refreshButton),
            minimumLabelWidth: 280)
        { Margin = new Thickness(0, 4, 0, 4) };
        modeText.Bind(TextBlock.ForegroundProperty, Tokens.TextSecondary);
        AutomationProperties.SetName(modeBox, "Mode");

        Content = Ui.Page(
            Sizes.WidePageMaxWidth,
            header,
            Operation,
            notReady,
            body);
        SizeChanged += (_, _) => QueueFitKeyboard();
    }

    /// <summary>Show the selected tab. Without an editor there are no tabs to show.</summary>
    private void ShowTab(bool ready)
    {
        if (tabs.SelectedIndex != selectedTab)
        {
            tabs.Select(selectedTab, raise: false);
        }

        mappingPanel.IsVisible = ready && selectedTab == 0;
        mousePanel.IsVisible = ready && selectedTab == 1;
        profilesPanel.IsVisible = ready && selectedTab == 2;
        PlaceChrome();
    }

    protected override void OnShortWindowChanged(bool isShort)
    {
        PlaceChrome();

        // The slider descriptions follow the window (BuildSliders reads it too).
        if (view is { } current)
        {
            BuildSliders(current, current.Availability.Enabled);
            BuildMouseToggles(current, current.Availability.Enabled);
        }
    }

    /// <summary>The tabs slot, then the title and mode; the text column takes the width that is left so it wraps rather than running under the selector.</summary>
    private Grid HeaderLabel()
    {
        headerLabel = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
        var text = Ui.Stack(4, pageTitle, modeLine, new Border { Child = disabledReason });
        Grid.SetColumn(text, 1);
        headerLabel.Children.AddRange([headerTabsSlot, text]);
        return headerLabel;
    }

    private Grid headerLabel = null!;

    /// <summary>
    /// On a short window the keyboard is also scaled to the HEIGHT left under the
    /// chrome, so all six rows are on screen without scrolling, as on Android.
    /// Never below <see cref="MinKeyUnit"/> a key: under that the keys become
    /// mis-taps, and the page's scroller is the last resort instead.
    /// </summary>
    private void FitKeyboard()
    {
        if (!ShortWindow || Content is not ScrollViewer { Content: PageFrame frame } scroller ||
            !keyboardBox.IsEffectivelyVisible ||
            keyboardBox.TranslatePoint(default, frame) is not { } origin)
        {
            keyboardBox.MaxHeight = double.PositiveInfinity;
            return;
        }

        // Under the keyboard inside the page: the card body's bottom padding, the
        // card's border, and the page frame's own bottom padding.
        var room = scroller.Viewport.Height - origin.Y - 14 - 1 - frame.Padding.Bottom;
        var rows = keyboardHost.Bounds.Height / KeyUnit;
        keyboardBox.MaxHeight = Math.Max(room, rows * MinKeyUnit);
    }

    private void QueueFitKeyboard() => Dispatcher.UIThread.Post(FitKeyboard, DispatcherPriority.Background);

    /// <summary>
    /// The short-window arrangement (shell spec, "Keyboard &amp; Mouse"), as on
    /// Android: under <see cref="Sizes.ShortWindowHeight"/> the tabs move up into
    /// the header in place of the title, the open profile takes the toolbar's
    /// left, and the Mapping actions its right, so the keyboard keeps the
    /// height. Two rows of chrome above six key rows were what made the page
    /// scroll on a landscape handheld.
    /// </summary>
    private void PlaceChrome()
    {
        var isShort = ShortWindow;
        headerTabsSlot.Content = null;
        toolbarLeft.Content = null;
        toolbarRight.Content = null;
        cardActionsSlot.Content = null;

        pageTitle.IsVisible = !isShort;
        headerTabsSlot.IsVisible = isShort;
        mappingHeader.IsVisible = !isShort;
        // Narrower selectors on a short window so the open profile and the
        // Mapping actions share the toolbar's one line.
        profileSelector.MinWidth = layoutBox.MinWidth = isShort ? ShortSelectorWidth : Sizes.SelectorMinWidth;
        profileSelector.MaxWidth = layoutBox.MaxWidth = isShort ? ShortSelectorWidth : double.PositiveInfinity;
        if (isShort)
        {
            headerTabsSlot.Content = tabs;
            toolbarLeft.Content = openProfile;
            toolbarRight.Content = selectedTab == 0 ? mappingActions : null;
        }
        else
        {
            toolbarLeft.Content = tabs;
            toolbarRight.Content = openProfile;
            cardActionsSlot.Content = mappingActions;
        }

        modeBox.MinWidth = isShort ? ShortSelectorWidth : Sizes.SelectorMinWidth;
        modeBox.Width = isShort ? ShortSelectorWidth : double.NaN;
        // An empty, hidden tabs column must not keep its gap.
        headerLabel.ColumnSpacing = isShort ? 16 : 0;
        ShowHeaderText();
        QueueFitKeyboard();
    }

    /// <summary>
    /// Beside the tabs on a short window there is room for two short lines, as
    /// Android caps its header text at two: the mode and the devices each on one
    /// trimmed line (the whole sentence is the tooltip), and why the selector is
    /// unavailable moves into the selector's own tooltip.
    /// </summary>
    private void ShowHeaderText()
    {
        var isShort = ShortWindow;
        foreach (var line in new[] { modeText, devicesText })
        {
            line.TextWrapping = isShort ? TextWrapping.NoWrap : TextWrapping.Wrap;
            line.TextTrimming = isShort ? TextTrimming.CharacterEllipsis : TextTrimming.None;
            line.MaxWidth = isShort ? ShortHeaderTextWidth : double.PositiveInfinity;
        }

        modeSeparator.IsVisible = !isShort;
        ((Control)disabledReason.Parent!).IsVisible = !isShort && disabledReason.IsVisible;
    }

    /// <summary>The keyboard, or the keys that are not on it: one at a time.</summary>
    private void ShowMoreKeys()
    {
        var more = moreKeys.IsChecked == true;
        keyboardSection.IsVisible = !more;
        otherKeysSection.IsVisible = more;
        QueueFitKeyboard();
    }

    private void LayMouse(bool stacked)
    {
        Grid.SetRow(mouseButtonsCard, 0);
        Grid.SetColumn(mouseButtonsCard, 0);
        Grid.SetColumnSpan(mouseButtonsCard, stacked ? 2 : 1);
        Grid.SetRow(tuningCard, stacked ? 1 : 0);
        Grid.SetColumn(tuningCard, stacked ? 0 : 1);
        Grid.SetColumnSpan(tuningCard, stacked ? 2 : 1);
    }

    /// <summary>Two slider columns when the card is wide enough; stacked, it was taller than a short window.</summary>
    private void LayTuning(bool twoColumns)
    {
        tuningGrid.ColumnDefinitions = new ColumnDefinitions(twoColumns ? "*,*" : "*");
        tuningGrid.RowDefinitions = new RowDefinitions(twoColumns ? "Auto" : "Auto,Auto");
        Grid.SetColumn(directionHost, twoColumns ? 1 : 0);
        Grid.SetRow(directionHost, twoColumns ? 0 : 1);
    }

    private void LayProfiles(bool stacked)
    {
        Place(libraryCard, 0, 0);
        Place(resetCard, stacked ? 1 : 0, stacked ? 0 : 1);
        Place(bankCard, stacked ? 2 : 1, 0);
        Place(switchCard, stacked ? 3 : 1, stacked ? 0 : 1);
        Place(countersExpander, stacked ? 4 : 2, 0);
        Grid.SetColumnSpan(countersExpander, 2);
        foreach (var card in new[] { libraryCard, resetCard, bankCard, switchCard })
        {
            Grid.SetColumnSpan(card, stacked ? 2 : 1);
        }

        static void Place(Control control, int row, int column)
        {
            Grid.SetRow(control, row);
            Grid.SetColumn(control, column);
        }
    }

    protected override async void OnShown()
    {
        await GuardAsync(async () =>
        {
            adapters.KeyboardMouse.Changed += OnStateChanged;
            adapters.KeyboardMouseBusy.Changed += OnStateChanged;
            adapters.Connection.Changed += OnStateChanged;

            Render();

            // Read on arrival when there is a session, so the page is useful
            // without a manual step.
            if (adapters.Connection.Value.Connected && !adapters.KeyboardMouse.Value.Loaded)
            {
                await RunAsync(() => adapters.RefreshKeyboardMouseAsync());
            }
        });
    }

    protected override void OnHidden()
    {
        adapters.KeyboardMouse.Changed -= OnStateChanged;
        adapters.KeyboardMouseBusy.Changed -= OnStateChanged;
        adapters.Connection.Changed -= OnStateChanged;

        // A pending commit belongs to a page that no longer exists.
        mouseCommit?.Stop();
        pendingMouseField = null;
    }

    private void OnStateChanged() => AppServices.OnUiThread(Render);

    /* ------------------------------------------------------------- actions */

    private async Task OnSetModeAsync()
    {
        if (modeBox.SelectedIndex < 0)
        {
            return;
        }

        var mode = (KbmMode)modeBox.SelectedIndex;
        await RunAsync(async () =>
        {
            var status = await adapters.SetKeyboardMouseModeAsync(mode);

            // The adapter decides. A mode needing a device that is not connected
            // resolves elsewhere, and saying "done" would be untrue.
            if (status.Mode != mode && status.ModeOverride == mode)
            {
                Report(
                    $"Set to {KeyboardMouse.Label(mode)}. The adapter is still using " +
                    $"{KeyboardMouse.Label(status.Mode)} until the device it needs is connected.",
                    NoticeSeverity.Informational);
            }
        });
    }

    private IReadOnlyList<KbmSelectableProfile> LibraryRows() =>
    [
        new KbmSelectableProfile(string.Empty, "Default (built-in)"),
        .. library.Value.For(profile).Select(local => new KbmSelectableProfile(local.Id, local.Name)),
    ];

    /// <summary>Selecting a profile OPENS it. It never applies it.</summary>
    private async Task OnSelectedProfileChangedAsync()
    {
        var index = profileSelector.SelectedIndex;
        if (index < 0 || index >= renderedProfileRows.Count)
        {
            return;
        }

        var id = renderedProfileRows[index].Id;

        // Decided BEFORE anything is awaited.
        var action = KbmProfileSelection.Decide(id, draft?.ProfileId ?? string.Empty, draft?.Dirty == true, populating);
        switch (action)
        {
            case KbmSelectionAction.Ignore:
                return;

            case KbmSelectionAction.ConfirmDiscard
                when !await Dialogs.ConfirmAsync(
                    "Discard unsaved changes?",
                    "The changes to this profile have not been saved and will be lost.",
                    "Discard"):
                Render();
                return;
        }

        selectedLocalId = id.Length == 0 ? null : id;
        OpenProfile(id);
    }

    /// <summary>Open a LOCAL profile in the editor. No adapter involved.</summary>
    private void OpenProfile(string? id)
    {
        var local = string.IsNullOrEmpty(id) ? null : library.Value.Find(id);
        draft = local is null ? KbmLocalDraft.FromDefault(profile) : KbmLocalDraft.From(local);
        Render();
    }

    private async Task OnSaveProfileAsync()
    {
        if (draft is null)
        {
            return;
        }

        // LOCAL SAVE. Sends nothing to the adapter.
        if (draft.IsBuiltin)
        {
            // Default is a template, never written into.
            var name = await PromptAsync("Save as new profile", "Profile name", SuggestName("My mapping"));
            if (string.IsNullOrWhiteSpace(name))
            {
                return;
            }

            var created = library.Create(profile, name, draft.Overrides, draft.Mouse);
            draft = KbmLocalDraft.From(created);
            selectedLocalId = created.Id;
        }
        else
        {
            var saved = library.Save(draft.ProfileId, draft.Name, draft.Overrides, draft.Mouse);
            draft = draft.Rebased(saved);
        }

        Render();
    }

    private async Task OnNewProfileAsync()
    {
        var name = await PromptAsync(
            "New profile",
            "Starts from the built-in Default mapping. It stays in your library until you assign it to the adapter.",
            SuggestName("My mapping"));
        if (string.IsNullOrWhiteSpace(name))
        {
            return;
        }

        var fresh = KbmLocalDraft.FromDefault(profile);
        var created = library.Create(profile, name, fresh.Overrides, fresh.Mouse);
        selectedLocalId = created.Id;
        draft = KbmLocalDraft.From(created);
        Render();
    }

    private async Task OnDuplicateProfileAsync()
    {
        if (draft is null)
        {
            return;
        }

        var name = await PromptAsync("Duplicate profile", "Profile name", SuggestName($"{draft.Name} copy"));
        if (string.IsNullOrWhiteSpace(name))
        {
            return;
        }

        // The draft's CURRENT content: duplicating copies what the user sees.
        var created = library.Create(profile, name, draft.Overrides, draft.Mouse);
        selectedLocalId = created.Id;
        draft = KbmLocalDraft.From(created);
        Render();
    }

    private async Task OnRenameProfileAsync()
    {
        if (draft is null || draft.IsBuiltin)
        {
            return;
        }

        var name = await PromptAsync("Rename profile", "Profile name", draft.Name);
        if (string.IsNullOrWhiteSpace(name))
        {
            return;
        }

        var renamed = library.Rename(draft.ProfileId, name);
        if (renamed is not null)
        {
            draft = draft.WithName(name).Rebased(renamed);
        }

        Render();
    }

    private async Task OnDeleteProfileAsync()
    {
        if (draft is null || draft.IsBuiltin)
        {
            return;
        }

        // A resident copy is a SEPARATE snapshot the adapter owns; deleting the
        // library profile must not remove it, and the user is told so.
        var resident = adapters.KeyboardMouse.Value.Profiles.Profiles
            .FirstOrDefault(row => row.Layout == profile && row.Fingerprint == draft.Fingerprint);
        if (!await Dialogs.ConfirmAsync(
                $"Delete '{draft.Name}' from your library?",
                resident is null
                    ? "This removes it from this app. Nothing on the adapter changes."
                    : $"This removes it from this app. The copy on the adapter " +
                      $"({KbmPositions.Label(resident.Position)}) stays, and the console keeps working as it does now.",
                "Delete"))
        {
            return;
        }

        library.Delete(draft.ProfileId);
        selectedLocalId = null;
        OpenProfile(null);
    }

    /// <summary>
    /// ASSIGN the open profile into one of this layout's bank positions: the only
    /// path from the library to the adapter, and deliberately not an activation.
    /// </summary>
    private async Task OnAssignToAdapterAsync()
    {
        if (draft is null || view is null)
        {
            return;
        }

        var bank = KbmBankView.Bank(adapters.KeyboardMouse.Value, profile)
            .Where(slot => slot.Position != KbmPositions.Default)
            .ToList();

        var list = new ListBox
        {
            SelectionMode = SelectionMode.Single,
            ItemsSource = bank
                .Select(slot => slot.Empty
                    ? $"{slot.PositionLabel} — Empty"
                    : $"{slot.PositionLabel} — {slot.ResidentLabel} (replace)")
                .ToArray(),
        };

        // Preselect a free position: filling an empty slot is the common case.
        list.SelectedIndex = Math.Max(0, bank.FindIndex(slot => slot.Empty));

        var choice = await Dialogs.ShowAsync($"Assign '{draft.Name}' to", list, ["Assign", "Cancel"], primaryIndex: 0, defaultIndex: 0);
        if (choice != 0 || list.SelectedIndex < 0)
        {
            return;
        }

        var chosen = bank[list.SelectedIndex];
        var local = new KbmLocalProfile
        {
            Id = draft.ProfileId,
            Layout = profile,
            Name = draft.Name,

            // The SPARSE overrides, which is exactly what the adapter stores.
            Bindings = draft.Overrides,
            Mouse = draft.Mouse,
        };

        // Success is reported only if the upload actually completed: the
        // result-carrying overload returns null when the operation threw.
        var assigned = await RunAsync(() => adapters.AssignKbmPositionAsync(profile, chosen.Position, local));
        if (assigned is null)
        {
            return;
        }

        Report($"'{draft.Name}' is now {chosen.PositionLabel} on the adapter. Activate it to use it now.", NoticeSeverity.Success);
        Render();
    }

    /// <summary>Copy a resident profile into the local library, matched by content.</summary>
    private async Task CopyToLibraryAsync(KbmBankSlot slot)
    {
        if (slot.Resident is not { } resident)
        {
            return;
        }

        var mapping = await RunAsync(() => adapters.LoadKbmPositionAsync(profile, slot.Position));
        if (mapping is null)
        {
            return;
        }

        var before = library.Value.Profiles.Count;

        // Only the OVERRIDES: importing the effective mapping would store the
        // canonical table as if the user had chosen every binding in it.
        var overrides = mapping.Bindings.Where(binding => binding.Custom).ToList();
        var imported = library.Import(profile, resident.Name, overrides, adapters.KeyboardMouse.Value.Mouse);

        selectedLocalId = imported.Id;
        OpenProfile(imported.Id);
        Report(
            library.Value.Profiles.Count == before
                ? $"'{imported.Name}' is already in your library."
                : $"'{imported.Name}' copied to your library.",
            NoticeSeverity.Success);
    }

    private async Task RemoveFromAdapterAsync(KbmBankSlot slot)
    {
        var consequence = slot.IsRuntime || slot.IsBoot ? " This layout falls back to the built-in Default." : string.Empty;
        if (!await Dialogs.ConfirmAsync(
                $"Remove '{slot.ResidentLabel}' from {slot.PositionLabel}?",
                $"The adapter's copy is removed.{consequence} Your library keeps this profile.",
                "Remove"))
        {
            return;
        }

        await RunAsync(() => adapters.RemoveKbmPositionAsync(profile, slot.Position));
    }

    private async Task ChooseSwitchKeyAsync(int position)
    {
        // Function keys first because they are the obvious choice; nothing is reserved.
        var caps = KeyboardLayout.AllKeys
            .Concat(KeyboardLayout.Other.Keys)
            .OrderBy(cap => cap.Usage is >= 0x3A and <= 0x45 ? 0 : 1)
            .ThenBy(cap => cap.Usage)
            .ToList();

        var list = new ListBox { SelectionMode = SelectionMode.Single, MaxHeight = 380, ItemsSource = caps.Select(cap => cap.Label).ToArray() };
        var choice = await Dialogs.ShowAsync($"Key for {KbmPositions.Label(position)}", list, ["Assign", "Cancel"], primaryIndex: 0, defaultIndex: 0);
        if (choice != 0 || list.SelectedIndex < 0)
        {
            return;
        }

        var source = new KbmSource(KbmSourceKind.Key, caps[list.SelectedIndex].Usage);
        await RunAsync(() => adapters.BindKbmSwitchAsync(source, position));
    }

    private string SuggestName(string basis)
    {
        var taken = library.Value.For(profile).Select(p => p.Name).ToHashSet(StringComparer.CurrentCultureIgnoreCase);
        if (!taken.Contains(basis))
        {
            return basis;
        }

        for (var i = 2; i < 100; i++)
        {
            var candidate = $"{basis} {i}";
            if (!taken.Contains(candidate))
            {
                return candidate;
            }
        }

        return basis;
    }

    private void OnLayoutChanged()
    {
        if (populating || layoutBox.SelectedIndex < 0)
        {
            return;
        }

        profile = (KbmLayout)layoutBox.SelectedIndex;
        profileChosenByUser = true;
        Render();
    }

    private async Task OnResetProfileAsync()
    {
        if (await Dialogs.ConfirmAsync(
                "Reset this mapping?",
                "Every key goes back to the adapter's own default mapping. The other layout and the mouse tuning are not affected.",
                "Reset mapping"))
        {
            await RunAsync(() => adapters.ResetProfileAsync(profile));
        }
    }

    private async Task OnResetAllAsync()
    {
        if (await Dialogs.ConfirmAsync(
                "Reset everything?",
                "Both layouts and the mouse tuning go back to the adapter's own defaults. Every mapping you have changed is lost.",
                "Reset everything"))
        {
            await RunAsync(() => adapters.ResetAllKeyboardMouseAsync());
        }
    }

    /// <summary>
    /// Pick a destination for one source, clear it, or restore its default.
    /// Clearing (<c>none</c>) and restoring (<c>default</c>) are different
    /// operations, so the dialog offers both.
    /// </summary>
    private async Task EditAsync(KbmSource source, string label)
    {
        var destinations = Enum.GetValues<KbmDestination>();
        var list = new ListBox
        {
            SelectionMode = SelectionMode.Single,
            MaxHeight = 380,
            ItemsSource = destinations.Select(KeyboardMouseView.Describe).ToArray(),
            SelectedIndex = Array.IndexOf(destinations, CurrentDestination(source)),
        };

        var choice = await Dialogs.ShowAsync($"What should {label} do?", list, ["Set", "Restore default", "Cancel"], primaryIndex: 0, defaultIndex: 0);
        if (choice == 1)
        {
            await RestoreDefaultAsync(source);
        }
        else if (choice == 0 && list.SelectedIndex >= 0)
        {
            await ApplyDestinationAsync(source, destinations[list.SelectedIndex]);
        }
    }

    /// <summary>THE single path by which any binding changes. Edits the LOCAL draft and sends nothing.</summary>
    private async Task ApplyDestinationAsync(KbmSource source, KbmDestination destination)
    {
        if (draft is not null)
        {
            draft = draft.With(source, destination);
            Render();
            return;
        }

        await RunAsync(() => adapters.BindAsync(profile, source, destination));
    }

    private async Task RestoreDefaultAsync(KbmSource source)
    {
        if (draft is not null)
        {
            draft = draft.With(source, DefaultDestination(source));
            Render();
            return;
        }

        // A null destination is the protocol's `default`.
        await RunAsync(() => adapters.BindAsync(profile, source, destination: null));
    }

    private KbmDestination DefaultDestination(KbmSource source)
    {
        var cells = source.Kind == KbmSourceKind.MouseButton ? view?.MouseButtons : view?.Keys;
        var cell = cells?.FirstOrDefault(c => c.Cap.Usage == source.Code);
        return cell is { Overridden: false } ? cell.Destination : KbmDestination.None;
    }

    private KbmDestination CurrentDestination(KbmSource source)
    {
        if (view is null)
        {
            return KbmDestination.None;
        }

        var cells = source.Kind == KbmSourceKind.MouseButton ? view.MouseButtons : view.Keys;
        return cells.FirstOrDefault(cell => cell.Cap.Usage == source.Code)?.Destination
            ?? view.Undrawn.FirstOrDefault(cell => cell.Cap.Usage == source.Code)?.Destination
            ?? KbmDestination.None;
    }

    /* ------------------------------------------------------------- painting */

    private void Render()
    {
        var connected = adapters.Connection.Value.Connected;
        var state = adapters.KeyboardMouse.Value;

        // Follow the adapter until the user says otherwise.
        if (!profileChosenByUser && state.Loaded)
        {
            profile = state.Status.Profile;
        }

        // A draft belongs to one layout.
        if (draft is not null && draft.Layout != profile)
        {
            draft = null;
        }

        view = KeyboardMouse.Project(state, profile, connected, draft);
        var busy = adapters.KeyboardMouseBusy.Value;
        busyBar.IsVisible = busy;

        // Offline is not a failure: the library and editor need no adapter.
        // A read that actually FAILED replaces the page with its reason.
        var ready = view.ShowEditor;
        notReady.IsVisible = view.ShowNotReady;
        notReadyTitle.Text = view.NotReadyTitle;
        notReadyDetail.Text = view.NotReadyDetail;
        notReadyRetry.IsEnabled = view.Availability.Enabled && !busy;
        body.IsVisible = ready;
        ShowTab(ready);
        refreshButton.IsEnabled = notReadyRetry.IsEnabled;
        if (!ready)
        {
            return;
        }

        populating = true;
        try
        {
            modeBox.SelectedIndex = (int)view.ModeOverride;
            layoutBox.SelectedIndex = (int)profile;
        }
        finally
        {
            populating = false;
        }

        modeText.Text = view.ModeText;
        devicesText.Text = view.DevicesText;

        var enabled = view.Availability.Enabled;

        // Commands are gated on the busy flag as well as on the connection, so
        // an impatient click cannot queue a second full read. Keys and sliders
        // are not: live tuning stays usable.
        var interactive = enabled && !busy;
        modeBox.IsEnabled = interactive;
        modeApply.IsEnabled = interactive;
        refreshButton.IsEnabled = interactive;
        resetProfile.IsEnabled = interactive;
        resetAll.IsEnabled = interactive;
        Ui.SetOptional(disabledReason, view.Availability.DisabledReason);
        ToolTip.SetTip(modeBox, view.Availability.DisabledReason);
        ToolTip.SetTip(modeLine, $"{view.ModeText} · {view.DevicesText}");
        ShowHeaderText();

        mappingSummary.Text = view.Loaded ? $"{view.BoundCount} of {view.MappableCount} inputs mapped" : "Not read yet";

        countersExpander.IsVisible = view.Counters is not null;
        countersText.Text = view.Counters?.Text ?? string.Empty;

        RenderProfileSelector(view);

        if (view.Loaded && view.EditingInactiveProfile)
        {
            inactiveProfileBar.Show("Not the profile in use", view.InactiveProfileWarning ?? string.Empty, NoticeSeverity.Warning);
        }
        else
        {
            inactiveProfileBar.Hide();
        }

        ((Control)draftBar.Parent!.Parent!).IsVisible = draftBar.IsVisible || inactiveProfileBar.IsVisible;

        // The mapping grid follows the LOCAL draft and is editable offline; the
        // bank and switch keys are adapter-owned and are not.
        BuildKeyboard(view);
        BuildMouseButtons(view);
        BuildBank(view, view.AdapterAvailable && !busy);
        BuildSwitchKeys(view.AdapterAvailable && !busy);
        BuildSliders(view, enabled);
        BuildMouseToggles(view, enabled);
        BuildUndrawn(view);
    }

    private void RenderProfileSelector(KeyboardMouseView current)
    {
        // RECONCILE, NEVER REBUILD BLINDLY: identity is the local profile id, and
        // the items change only when what the control displays changes.
        var plan = KbmProfileSelection.Plan(renderedProfileRows, LibraryRows(), selectedLocalId ?? string.Empty);
        selectedLocalId = plan.SelectedId.Length == 0 ? null : plan.SelectedId;

        populating = true;
        try
        {
            if (plan.Rebuild && !profileSelector.IsDropDownOpen)
            {
                profileSelector.ItemsSource = plan.Rows.Select(row => row.Label).ToList();
                renderedProfileRows = [.. plan.Rows];
            }

            if (profileSelector.SelectedIndex != plan.SelectedIndex && plan.SelectedIndex < renderedProfileRows.Count)
            {
                profileSelector.SelectedIndex = plan.SelectedIndex;
            }
        }
        finally
        {
            populating = false;
        }

        saveButton.IsEnabled = current.CanSave;
        saveButton.Content = current.SaveLabel;
        ToolTip.SetTip(saveButton, current.StatusText);
        AutomationProperties.SetHelpText(saveButton, current.StatusText);
        discardButton.IsEnabled = current.Dirty;
        duplicateButton.IsEnabled = current.SelectedProfile is not null && current.DraftState != KbmDraftState.Disconnected;
        renameButton.IsEnabled = current.CanRename;
        deleteButton.IsEnabled = current.CanDelete;
        newButton.IsEnabled = current.ProfilesSupported && current.DraftState != KbmDraftState.Disconnected;

        if (current.StatusDetail is { } detail)
        {
            draftBar.Show(null, detail, current.DraftState switch
            {
                KbmDraftState.Conflict => NoticeSeverity.Error,
                KbmDraftState.SavedNotApplied => NoticeSeverity.Warning,
                _ => NoticeSeverity.Informational,
            });
        }
        else
        {
            draftBar.Hide();
        }
    }

    /// <summary>
    /// Each cluster on its own canvas, positioned from the layout's geometry:
    /// the layout has gaps, half-unit offsets and two-unit-tall keys that no
    /// row of buttons can express.
    /// </summary>
    private void BuildKeyboard(KeyboardMouseView current)
    {
        keyboardHost.Children.Clear();
        foreach (var cluster in current.Clusters)
        {
            var canvas = new Canvas { Width = cluster.Columns * KeyUnit, Height = cluster.Rows * KeyUnit };
            foreach (var cell in cluster.Cells)
            {
                var button = KeyButton(cell);
                button.Width = (cell.Cap.Width * KeyUnit) - KeyGap;
                button.Height = (cell.Cap.Height * KeyUnit) - KeyGap;
                Canvas.SetLeft(button, cell.Cap.Column * KeyUnit);
                Canvas.SetTop(button, cell.Cap.Row * KeyUnit);
                canvas.Children.Add(button);
            }

            keyboardHost.Children.Add(canvas);
        }

        otherKeysHost.Children.Clear();
        foreach (var cell in current.OtherKeys.Cells)
        {
            otherKeysHost.Children.Add(KeyButton(cell, fixedGeometry: false));
        }

        // Counted, because a binding the user cannot see is one they cannot remove.
        var hidden = current.OtherKeys.Cells.Count(cell => cell.Bound) + current.Undrawn.Count;
        moreKeys.Content = hidden > 0 ? $"More keys ({hidden})" : "More keys";
    }

    private void BuildMouseButtons(KeyboardMouseView current)
    {
        // The tiles are hidden in the keyboard-only layout: those bindings can
        // never fire there, and the card says where they are mapped instead.
        mouseButtonHost.IsVisible = current.ShowMouseButtons;
        mouseButtonsNote.IsVisible = !current.ShowMouseButtons;
        mouseButtonHost.Children.Clear();
        if (!current.ShowMouseButtons)
        {
            return;
        }

        foreach (var cell in current.MouseButtons)
        {
            // Tiles like the keys, as on Android: the binding is on the tile.
            var tile = KeyButton(cell, mouse: true, fixedGeometry: false);
            tile.MinWidth = 104;
            tile.Height = 48;
            mouseButtonHost.Children.Add(tile);
        }
    }

    private void BuildUndrawn(KeyboardMouseView current)
    {
        undrawnSection.IsVisible = current.Undrawn.Count > 0;
        undrawnHost.Children.Clear();
        foreach (var cell in current.Undrawn)
        {
            var source = new KbmSource(KbmSourceKind.Key, cell.Cap.Usage);
            var change = Ui.Button("Change", async (_, _) => await GuardAsync(() => EditAsync(source, KeyboardLayout.Describe(source))), $"Change {cell.AccessibleName}");
            undrawnHost.Children.Add(new SettingRow(Ui.Text(cell.AccessibleName), change) { Padding = new Thickness(0, 6) });
        }
    }

    /// <param name="fixedGeometry">
    /// True for the keyboard grid, where a key must occupy exactly its share of
    /// the layout. False for the ISO, international and mouse-button rows,
    /// which size to their content so "Katakana/Hiragana" is never clipped.
    /// </param>
    private Button KeyButton(KeyBindingCell cell, bool mouse = false, bool fixedGeometry = true)
    {
        var trimming = fixedGeometry ? TextTrimming.CharacterEllipsis : TextTrimming.None;
        var caption = new StackPanel { Spacing = 0, VerticalAlignment = VerticalAlignment.Center };
        // On the grid a long name on a one-unit key ("Home", "Pause", "Down")
        // SHRINKS to fit rather than trimming to "Ho...": an ellipsis on a key
        // cap hides which key it is (companion-shell-layout.md; Windows does the
        // same with a DownOnly Viewbox). The binding under it may still trim.
        var label = new TextBlock { Text = cell.Cap.Label, FontSize = 12, HorizontalAlignment = HorizontalAlignment.Center };
        caption.Children.Add(fixedGeometry
            ? new Viewbox
            {
                Child = label,
                Stretch = Stretch.Uniform,
                StretchDirection = StretchDirection.DownOnly,
                HorizontalAlignment = HorizontalAlignment.Center,
            }
            : label);

        // The binding is shown ON the key: hover-to-discover is a list with extra steps.
        if (cell.Bound)
        {
            caption.Children.Add(new TextBlock
            {
                Text = cell.DestinationLabel,
                FontSize = 10,
                FontWeight = FontWeight.SemiBold,
                HorizontalAlignment = HorizontalAlignment.Center,
                TextTrimming = trimming,
            });
        }

        var button = new Button
        {
            Content = caption,
            MinWidth = 0,
            MinHeight = 0,
            Height = KeyUnit - KeyGap,
            Padding = fixedGeometry ? new Thickness(2) : new Thickness(10, 2),
            VerticalAlignment = VerticalAlignment.Top,
        };

        if (!fixedGeometry)
        {
            button.MinWidth = (cell.Cap.Width * KeyUnit) - KeyGap;
        }

        // Colour alone cannot carry "changed": the accessible name carries it.
        AutomationProperties.SetName(button, cell.AccessibleName);
        ToolTip.SetTip(button, cell.Tooltip);
        if (cell.Overridden)
        {
            button.Classes.Add("accent");
        }

        var source = mouse
            ? new KbmSource(KbmSourceKind.MouseButton, cell.Cap.Usage)
            : new KbmSource(KbmSourceKind.Key, cell.Cap.Usage);
        button.Click += async (_, _) => await GuardAsync(() => EditAsync(source, cell.Cap.Label));

        // MIDDLE-CLICK CLEARS, through the same path as the dialog, and only when
        // it would change something. Tunnelling, because Button handles the press.
        if (cell.CanClear)
        {
            button.AddHandler(
                PointerPressedEvent,
                async (object? sender, PointerPressedEventArgs args) =>
                {
                    if (!args.GetCurrentPoint(button).Properties.IsMiddleButtonPressed)
                    {
                        return;
                    }

                    args.Handled = true;
                    await GuardAsync(() => ApplyDestinationAsync(source, KbmDestination.None));
                },
                RoutingStrategies.Tunnel);
        }

        return button;
    }

    /// <summary>The adapter's bank: one row per position, empty ones included.</summary>
    private void BuildBank(KeyboardMouseView current, bool enabled)
    {
        bankHost.Children.Clear();
        var bank = KbmBankView.Bank(adapters.KeyboardMouse.Value, profile);
        foreach (var slot in bank)
        {
            // Runtime and boot are separate facts and are labelled separately.
            var marks = new List<string>();
            if (slot.IsRuntime)
            {
                marks.Add("active now");
            }

            if (slot.IsBoot)
            {
                marks.Add("on startup");
            }

            if (slot.SwitchKey is { } key)
            {
                marks.Add($"key {KeyboardLayout.Describe(key)}");
            }

            var title = Ui.Text($"{slot.PositionLabel}: {slot.ResidentLabel}");
            var detail = Ui.Caption(marks.Count > 0 ? string.Join(" · ", marks) : slot.Empty ? "Empty" : null);
            Ui.SetOptional(detail, detail.Text);

            // Runtime activation is zero flash writes; On startup is the one
            // selection worth a write, which is why they are separate buttons.
            var activate = Ui.Button("Activate", async (_, _) => await GuardAsync(() => RunAsync(() => adapters.ActivateKbmPositionAsync(profile, slot.Position))), $"Activate {slot.PositionLabel}");
            activate.IsEnabled = enabled && !slot.Empty && !slot.IsRuntime;
            var boot = Ui.Button("On startup", async (_, _) => await GuardAsync(() => RunAsync(() => adapters.SetKbmBootPositionAsync(profile, slot.Position))), $"Use {slot.PositionLabel} on startup");
            boot.IsEnabled = enabled && !slot.Empty && !slot.IsBoot;

            var controls = new List<Control> { activate, boot };

            // Default is built in: it cannot be emptied or copied.
            if (slot.Position != KbmPositions.Default)
            {
                var more = Ui.MenuButton(
                    "More",
                    $"More actions for {slot.PositionLabel}",
                    ("Copy to library", () => _ = GuardAsync(() => CopyToLibraryAsync(slot))),
                    ("Remove from adapter…", () => _ = GuardAsync(() => RemoveFromAdapterAsync(slot))));
                more.IsEnabled = enabled && !slot.Empty;
                controls.Add(more);
            }

            bankHost.Children.Add(Ui.Setting(title, detail, [.. controls]));
        }

        var used = bank.Count(slot => !slot.Empty && slot.Position != KbmPositions.Default);
        bankSummary.Text = current.AdapterUnavailableReason ??
            $"{used} of {KbmLimits.PositionsPerLayout} positions used for " +
            $"{(profile == KbmLayout.Keyboard ? "keyboard" : "keyboard and mouse")}. These work with no app connected.";

        // Assign needs a live adapter AND something of the user's to send.
        assignButton.IsEnabled = enabled && draft is not null && !draft.IsBuiltin;
    }

    /// <summary>One row per switch action, bound or not: the unbound ones are what the user came to set up.</summary>
    private void BuildSwitchKeys(bool enabled)
    {
        switchHost.Children.Clear();
        foreach (var (position, key) in KbmBankView.SwitchActions(adapters.KeyboardMouse.Value))
        {
            var choose = Ui.Button(key is null ? "Assign key" : "Change", async (_, _) => await GuardAsync(() => ChooseSwitchKeyAsync(position)), $"Choose the key for {KbmPositions.Label(position)}");
            choose.IsEnabled = enabled;
            var controls = new List<Control> { choose };
            if (key is not null)
            {
                var clear = Ui.Button("Clear", async (_, _) => await GuardAsync(() => RunAsync(() => adapters.BindKbmSwitchAsync(key, position: null))), $"Clear the key for {KbmPositions.Label(position)}");
                clear.IsEnabled = enabled;
                controls.Add(clear);
            }

            switchHost.Children.Add(Ui.Setting(
                Ui.Text(KbmPositions.Label(position)),
                Ui.Caption(key is null ? "Not assigned" : KeyboardLayout.Describe(key)),
                [.. controls]));
        }
    }

    /// <summary>
    /// Name and value on one line, slider under it, description under that.
    /// Anti-deadzone goes to the direction group, above the invert chips; the
    /// rest are the stick's feel.
    /// </summary>
    private void BuildSliders(KeyboardMouseView current, bool enabled)
    {
        feelHost.Children.Clear();
        directionHost.Children.Clear();
        foreach (var model in current.MouseSliders)
        {
            // The NAME keeps its width and the value trims: in the two-column
            // tuning card "Not adjustable on this firmware" used to squeeze the
            // name to a letter a line.
            var heading = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*"), ColumnSpacing = 12 };
            heading.Children.Add(Ui.Text(model.Label));
            var value = Ui.Value(model.ValueText);
            value.TextWrapping = TextWrapping.NoWrap;
            value.TextTrimming = TextTrimming.CharacterEllipsis;
            value.TextAlignment = TextAlignment.Right;
            ToolTip.SetTip(value, model.ValueText);
            Grid.SetColumn(value, 1);
            heading.Children.Add(value);

            var maximum = Math.Max(model.Maximum, model.Minimum + 1);
            var slider = new Slider
            {
                Minimum = model.Minimum,
                Maximum = maximum,
                Value = Math.Clamp(model.Value, model.Minimum, maximum),
                IsEnabled = enabled && model.Available,
                HorizontalAlignment = HorizontalAlignment.Stretch,
            };
            AutomationProperties.SetName(slider, model.Label);

            // Debounced, not sent per tick: one settle timer covers drag,
            // keyboard and touch alike.
            var field = model.Field;
            slider.ValueChanged += (_, e) => ScheduleMouseCommit(field, (int)e.NewValue);

            var host = model.Field == KbmMouseField.AntiDeadzone ? directionHost : feelHost;
            // On a short window the description is the slider's tooltip rather
            // than a paragraph under it: three of them were taller than the page.
            ToolTip.SetTip(slider, model.Description);
            var description = Ui.Caption(model.Description);
            description.IsVisible = !ShortWindow;
            host.Children.Add(Ui.Stack(2, heading, slider, description));
        }
    }

    /// <summary>
    /// The two inverts as chips on one row, as on Android: each is a word, and a
    /// settings row apiece was a row of height each on a short window. What each
    /// does is the chip's tooltip.
    /// </summary>
    private void BuildMouseToggles(KeyboardMouseView current, bool enabled)
    {
        invertChips.Children.Clear();
        foreach (var model in current.MouseToggles)
        {
            var chip = new ToggleButton { Content = model.Label, IsChecked = model.Value, IsEnabled = enabled };
            AutomationProperties.SetName(chip, model.Label);
            ToolTip.SetTip(chip, model.Description);
            var field = model.Field;
            chip.Click += async (_, _) => await GuardAsync(() => RunAsync(() => adapters.SetMouseAsync(field, chip.IsChecked == true ? 1 : 0)));
            invertChips.Children.Add(chip);
        }

        directionHost.Children.Add(invertChips);
    }

    private void ScheduleMouseCommit(KbmMouseField field, int value)
    {
        pendingMouseField = field;
        pendingMouseValue = value;
        mouseCommit ??= CreateMouseCommitTimer();
        mouseCommit.Stop();
        mouseCommit.Start();
    }

    private DispatcherTimer CreateMouseCommitTimer()
    {
        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(SliderSettleMillis) };
        timer.Tick += async (_, _) =>
        {
            timer.Stop();
            if (pendingMouseField is { } field)
            {
                pendingMouseField = null;
                await GuardAsync(() => RunAsync(() => adapters.SetMouseAsync(field, pendingMouseValue)));
            }
        };

        return timer;
    }

    private static TextBlock SubHeading(string text)
    {
        var block = Ui.Text(text);
        block.FontWeight = FontWeight.SemiBold;
        return block;
    }

    /* --------------------------------------------------------------- plumbing */

    /// <summary>
    /// The exception boundary for every event handler on this page. An exception
    /// escaping an async void handler would take the process down; this logs it,
    /// shows it and repaints into a coherent state.
    /// </summary>
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

    /// <summary>The same, for an operation with a result: default on failure, so a flow can abandon.</summary>
    private async Task<T?> RunAsync<T>(Func<Task<T>> operation)
    {
        T? result = default;
        var failed = false;
        await SafeAsync(
            async () =>
            {
                try
                {
                    result = await operation();
                }
                catch
                {
                    failed = true;
                    throw;
                }
            },
            Render);
        return failed ? default : result;
    }

    /// <summary>Ask for a profile name, bounded to what the adapter can store.</summary>
    private async Task<string?> PromptAsync(string title, string description, string initial)
    {
        var box = new TextBox { Text = initial, MaxLength = KbmProfileNameMax, MinWidth = 320 };
        box.AttachedToVisualTree += (_, _) => Dispatcher.UIThread.Post(() =>
        {
            box.Focus();
            box.SelectAll();
        });
        var choice = await Dialogs.ShowAsync(title, Ui.Stack(Ui.Text(description, secondary: true), box), ["OK", "Cancel"], primaryIndex: 0, defaultIndex: 0);
        return choice == 0 ? box.Text?.Trim() : null;
    }
}
