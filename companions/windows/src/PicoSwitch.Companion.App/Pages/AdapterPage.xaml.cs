using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Windows.UI;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// The adapter dashboard (§16.1). Phase 3's MVP surface.
///
/// The page paints <see cref="AdapterDashboardView"/> and calls
/// <see cref="AdapterConnectionService"/>. It makes no decisions of its own: every
/// "is this enabled", "what does this say" and "should this warn" is decided in
/// <see cref="AdapterDashboard"/>, where it is unit-tested. A rule that appears in
/// this file is a rule in the wrong place — the Phase 2 version of this page grew
/// its logic inline and none of it could be tested.
///
/// It calls the service and nothing below it — no GATT object, no management
/// command string.
/// </summary>
public sealed partial class AdapterPage : Page
{
    private readonly AdapterConnectionService adapters = AppServices.Adapters;

    /// <summary>Input source ids, positionally matched to <c>InputBox</c>.</summary>
    private readonly List<long> inputIds = [];

    private readonly List<Personality> personalities = [];

    private AdapterDashboardView? view;

    public AdapterPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        SizeChanged += (_, _) => ApplyShortWindow();
    }

    /// <summary>
    /// Under this window height the page takes its short-window arrangement: the
    /// same 560 as Android's LayoutTokens.ShortWindowHeight, measured on the window.
    /// </summary>
    private const double ShortWindowHeight = 560;

    private bool? shortWindow;

    /// <summary>
    /// One row of chrome instead of three on a short window, as on Android and
    /// Linux: the tabs join the header and the adapter's name, which the title
    /// bar already shows, is dropped.
    /// </summary>
    private void ApplyShortWindow()
    {
        var isShort = (XamlRoot?.Size.Height ?? double.PositiveInfinity) < ShortWindowHeight;
        if (shortWindow == isShort)
        {
            return;
        }

        shortWindow = isShort;
        HeaderTabsSlot.Child = null;
        PageTabsSlot.Child = null;
        AdapterTitle.Visibility = isShort ? Visibility.Collapsed : Visibility.Visible;
        HeaderTabsSlot.Visibility = isShort ? Visibility.Visible : Visibility.Collapsed;
        PageTabsSlot.Visibility = isShort ? Visibility.Collapsed : Visibility.Visible;
        Tabs.Margin = isShort ? new Thickness(0) : new Thickness(0, 8, 0, 4);
        PageGrid.Padding = isShort ? new Thickness(32, 4, 32, 8) : new Thickness(32, 12, 32, 32);
        if (isShort)
        {
            HeaderTabsSlot.Child = Tabs;
        }
        else
        {
            PageTabsSlot.Child = Tabs;
        }
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        adapters.Connection.Changed += OnStateChanged;
        adapters.Snapshot.Changed += OnStateChanged;
        adapters.Registry.Changed += OnStateChanged;
        adapters.Relationship.Changed += OnStateChanged;
        adapters.Radio.Changed += OnStateChanged;

        // The dashboard projection takes Controller Link streaming as an input,
        // so a change in it must repaint this page too.
        AppServices.ControllerLink.View.Changed += OnStateChanged;

        ShowTab();
        Render();
        await SafeAsync(() => adapters.ProbeRadioAsync());
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        // Page-scoped subscriptions end with the page. The SESSION does not:
        // navigating away must never tear down a live BLE connection.
        adapters.Connection.Changed -= OnStateChanged;
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Registry.Changed -= OnStateChanged;
        adapters.Relationship.Changed -= OnStateChanged;
        adapters.Radio.Changed -= OnStateChanged;
        AppServices.ControllerLink.View.Changed -= OnStateChanged;
    }

    /// <summary>The tab last shown this session; null until the page first opens.</summary>
    private static int? selectedTab;

    private void OnTabChanged(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        // The markup's IsSelected raises this during InitializeComponent, before
        // the panels exist and before the first-visit rule has run.
        if (!IsLoaded)
        {
            return;
        }

        selectedTab = sender.Items.IndexOf(sender.SelectedItem);
        ShowTab();
    }

    private void ShowTab()
    {
        // First visit with nothing remembered: the Adapters tab, where adding
        // one is. After that, wherever the user last was.
        selectedTab ??= adapters.Registry.Value.Records.Count == 0 ? 2 : 0;
        if (Tabs.Items.IndexOf(Tabs.SelectedItem) != selectedTab)
        {
            Tabs.SelectedItem = Tabs.Items[selectedTab.Value];
        }

        ConsolePanel.Visibility = selectedTab == 0 ? Visibility.Visible : Visibility.Collapsed;
        ControllersPanel.Visibility = selectedTab == 1 ? Visibility.Visible : Visibility.Collapsed;
        AdaptersPanel.Visibility = selectedTab == 2 ? Visibility.Visible : Visibility.Collapsed;
    }

    // Every observable this page watches is written from a WinRT pool thread.
    private void OnStateChanged() => AppServices.OnUiThread(Render);

    /* ------------------------------------------------------------- actions */

    private async void OnConnect(object sender, RoutedEventArgs e)
    {
        if (SelectedAdapter() is not { } record)
        {
            return;
        }

        await SafeAsync(() => adapters.ConnectAsync(record.Id));
    }

    private async void OnDisconnect(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.DisconnectAsync());

    private async void OnRefresh(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.RefreshAsync());

    private async void OnSwitchPersonality(object sender, RoutedEventArgs e)
    {
        if (PersonalityBox.SelectedIndex < 0 || PersonalityBox.SelectedIndex >= personalities.Count)
        {
            return;
        }

        var personality = personalities[PersonalityBox.SelectedIndex];
        await SafeAsync(async () =>
        {
            var outcome = await adapters.SetPersonalityAsync(personality);

            // Three outcomes, three different things to say. "Switched, but the
            // console has not seen it" needs its own instruction: the user is
            // looking at a console that still shows the old controller.
            if (outcome.Unchanged)
            {
                Report("The adapter was already in that mode.", InfoBarSeverity.Informational);
            }
            else if (outcome.Reenumerated)
            {
                Report(
                    $"Switched to {ControllerModeSection.Label(outcome.Personality)}.",
                    InfoBarSeverity.Success);
            }
            else
            {
                Report(
                    $"The adapter switched to {ControllerModeSection.Label(outcome.Personality)}, " +
                    "but the console has not seen the change yet. Unplug the adapter and plug it " +
                    "back in.",
                    InfoBarSeverity.Warning);
            }
        });
    }

    private async void OnSetInput(object sender, RoutedEventArgs e)
    {
        if (InputBox.SelectedIndex < 0 || InputBox.SelectedIndex >= inputIds.Count)
        {
            return;
        }

        var id = inputIds[InputBox.SelectedIndex];
        await SafeAsync(() => adapters.SetActiveInputAsync(id));
    }

    /* ------------------------------------------------------------- adapters */

    private async void OnPair(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.PairNewAdapterAsync());

    private async void OnConnectRow(object sender, RoutedEventArgs e)
    {
        if (RowId(sender) is { } id)
        {
            await SafeAsync(() => adapters.ConnectAsync(id));
        }
    }

    private async void OnRenameRow(object sender, RoutedEventArgs e)
    {
        if (RowId(sender) is not { } id ||
            adapters.Registry.Value.Record(id) is not { } record)
        {
            return;
        }

        var input = new TextBox
        {
            Text = record.UserAlias ?? string.Empty,
            PlaceholderText = record.LastKnownName,
            MaxLength = 64,
        };

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Rename adapter",
            Content = input,
            PrimaryButtonText = "Save",
            SecondaryButtonText = "Clear name",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        var result = await dialog.ShowAsync();
        if (result == ContentDialogResult.None)
        {
            return;
        }

        // A local nickname only. It never reaches the adapter and is never part of
        // its identity, which stays the Bluetooth address.
        var alias = result == ContentDialogResult.Primary ? input.Text : null;
        await SafeAsync(() => adapters.RenameAsync(id, alias));
    }

    private async void OnRepairRow(object sender, RoutedEventArgs e)
    {
        if (RowId(sender) is not { } id)
        {
            return;
        }

        // Unpairing destroys a trust relationship, so it is never automatic and
        // the confirmation names the consequence.
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Repair pairing?",
            Content =
                "Windows drops its pairing with this adapter, then you pair again with " +
                "the adapter's pairing window open.\n\n" +
                "The adapter stays in this list, and its name and controller history are kept.",
            PrimaryButtonText = "Repair pairing",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };

        if (await dialog.ShowAsync() == ContentDialogResult.Primary)
        {
            await SafeAsync(() => adapters.RepairAsync(id));
        }
    }

    private async void OnRemoveRow(object sender, RoutedEventArgs e)
    {
        if (RowId(sender) is not { } id)
        {
            return;
        }

        // States both halves. This copy previously promised the Windows pairing
        // was untouched, which stopped being true when Remove was corrected to
        // match §16.2 — a confirmation that misdescribes its own action is worse
        // than none, because it is the one place the user is deciding.
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Remove this adapter?",
            Content =
                "This app forgets the adapter and its controller history, and Windows " +
                "drops its pairing with it. You will need to pair again, with the " +
                "adapter's pairing window open.\n\n" +
                "The controllers paired to the adapter itself are not affected.",
            PrimaryButtonText = "Remove and unpair",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        await SafeAsync(async () =>
        {
            var removal = await adapters.RemoveAsync(id);
            if (removal.LeftOrphanPairing)
            {
                throw new ManagementException(
                    "The adapter was removed from this app, but Windows could not drop its " +
                    "pairing. Remove it from Windows Bluetooth settings as well.");
            }
        });
    }

    /* ---------------------------------------------------------- controllers */

    private async void OnForgetPeer(object sender, RoutedEventArgs e)
    {
        if (PeerIdOf(sender) is not { } peerId)
        {
            return;
        }

        // The confirmation says what happens to the controller AND what happens to
        // the row, because "it disappeared entirely" is the surprise otherwise.
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Forget this controller?",
            Content =
                "The adapter deletes its pairing with the controller. If the controller is " +
                "connected it will disconnect.\n\n" +
                "It stays in this list under Recent, so you can see it was here.",
            PrimaryButtonText = "Forget",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        await SafeAsync(async () =>
        {
            var outcome = await adapters.ForgetControllerAsync(peerId);

            // already_absent is an idempotent success. incomplete stays visible
            // rather than being smoothed over: the adapter is telling us it did
            // not finish, and hiding that produces a list nobody can trust.
            if (outcome.Result == PeerForgetResult.Incomplete)
            {
                Report(
                    "The adapter reported that it could not fully remove that controller. " +
                    "Refresh and check the list.",
                    InfoBarSeverity.Warning);
            }
        });
    }

    private async void OnRemoveHistory(object sender, RoutedEventArgs e)
    {
        if (PeerIdOf(sender) is { } peerId)
        {
            await SafeAsync(() => adapters.RemoveFromHistoryAsync(peerId));
        }
    }

    private async void OnPairController(object sender, RoutedEventArgs e) =>
        await new ControllerPairingDialog(adapters) { XamlRoot = XamlRoot }.RunAsync();

    /* ------------------------------------------------------------- painting */

    private AdapterRecord? SelectedAdapter()
    {
        var registry = adapters.Registry.Value;
        return registry.ActiveId is { } id
            ? registry.Record(id)
            : registry.Records.FirstOrDefault();
    }

    private void Render()
    {
        var selected = SelectedAdapter();

        view = AdapterDashboard.Project(
            adapters.Snapshot.Value,
            adapters.Relationship.Value,
            adapters.Connection.Value,
            selected,
            AppServices.ControllerLinkStreaming);

        var radio = adapters.Radio.Value;
        RadioBar.IsOpen = radio.ManagementBlockedReason is not null;
        RadioBar.Message = radio.ManagementBlockedReason ?? string.Empty;

        // The silence rule, expressed as one assignment.
        ContractBar.IsOpen = view.Contract.Visible;
        ContractBar.Message = view.Contract.Summary;

        AdapterTitle.Text = view.Title;
        // Only a real message (why a connect failed). "Add one below." used to
        // sit here, repeating the Adapters tab's "Add an adapter" card.
        SetOptionalText(PhaseDetail, view.Connected ? null : view.PhaseDetail);
        SetOptionalText(FirmwareText, view.FirmwareLine);

        ControllerText.Text = view.ControllerLine;
        SetOptionalText(BatteryText, view.BatteryLine);

        // One of Connect / Disconnect at a time, as Play shows Start or Stop.
        ConnectButton.Visibility = view.Connected ? Visibility.Collapsed : Visibility.Visible;
        DisconnectButton.Visibility = view.Connected ? Visibility.Visible : Visibility.Collapsed;
        ConnectButton.IsEnabled = selected is not null && !view.Connected;
        DisconnectButton.IsEnabled = view.Connected;
        RefreshButton.IsEnabled = view.Connected;

        RenderPersonality(view.ControllerMode);
        RenderInput(view.ConsoleInput);

        RenderLists();
    }

    /// <summary>This PC's adapters, and the controllers paired to the adapter.</summary>
    private void RenderLists()
    {
        var rows = adapters.RememberedAdapters();
        AdapterList.ItemsSource = rows;
        // No "No adapters yet." line: the "Add an adapter" card already says so.

        var controllers = adapters.Controllers();
        PeersUnsupported.IsOpen = !controllers.Visible;
        PeersUnsupported.Message = controllers.HiddenReason ?? string.Empty;
        PeerSections.Visibility = controllers.Visible ? Visibility.Visible : Visibility.Collapsed;

        PairControllerButton.IsEnabled = controllers.CanPairNewController;
        PairControllerReason.Text = controllers.PairDisabledReason ?? string.Empty;
        PairControllerReason.Visibility = controllers.PairDisabledReason is null
            ? Visibility.Collapsed
            : Visibility.Visible;

        var inventory = controllers.Inventory;
        Fill(ConnectedSection, ConnectedList, inventory.Connected, controllers.CanForget);
        Fill(PairedSection, PairedList, inventory.Paired, controllers.CanForget);

        // Recent rows never offer Forget: there is no adapter credential left to
        // delete, only a local history row.
        Fill(RecentSection, RecentList, inventory.Recent, canForget: false);

        PeersEmpty.Visibility = inventory.HasControllers ? Visibility.Collapsed : Visibility.Visible;
    }

    private static void Fill(
        StackPanel section,
        ItemsControl list,
        IReadOnlyList<PeerListing> listings,
        bool canForget)
    {
        section.Visibility = listings.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        list.ItemsSource = PeerRows.Project(listings, canForget);
    }

    private static AdapterId? RowId(object sender) =>
        sender is FrameworkElement { Tag: string value } ? AdapterId.FromAddress(value) : null;

    private static string? PeerIdOf(object sender) =>
        sender is FrameworkElement { Tag: string value } && !string.IsNullOrWhiteSpace(value)
            ? value
            : null;

    private void RenderPersonality(ControllerModeSection section)
    {
        personalities.Clear();
        personalities.AddRange(section.Available);

        PersonalityBox.Items.Clear();
        foreach (var personality in personalities)
        {
            PersonalityBox.Items.Add(ControllerModeSection.Label(personality));
        }

        PersonalityBox.SelectedIndex = personalities.IndexOf(section.Current);
        Gate(section.Availability, PersonalityReason, PersonalityBox, PersonalityApply);
    }

    private void RenderInput(ConsoleInputSection section)
    {
        inputIds.Clear();
        InputBox.Items.Clear();
        foreach (var source in section.Sources)
        {
            inputIds.Add(source.Id);
            InputBox.Items.Add(source.IsActive ? $"{source.Label} (in use)" : source.Label);
        }

        InputBox.SelectedIndex = inputIds.IndexOf(section.ActiveId);
        InputTruncated.IsOpen = section.Truncated;
        Gate(section.Availability, InputReason, InputBox, InputApply);
    }

    /// <summary>
    /// Enable or disable a section, and show its reason when disabled.
    ///
    /// A disabled control with no reason is the worst of both worlds: the user can
    /// neither act nor find out why. The reason itself is decided in the
    /// projection, not here.
    /// </summary>
    private static void Gate(
        SectionAvailability availability,
        TextBlock reason,
        params Control[] controls)
    {
        foreach (var control in controls)
        {
            control.IsEnabled = availability.Enabled;
        }

        SetOptionalText(reason, availability.DisabledReason);
    }

    private static void SetOptionalText(TextBlock block, string? text)
    {
        block.Text = text ?? string.Empty;
        block.Visibility = string.IsNullOrWhiteSpace(text) ? Visibility.Collapsed : Visibility.Visible;
    }

    private void Report(string message, InfoBarSeverity severity)
    {
        OperationBar.Severity = severity;
        OperationBar.Message = message;
        OperationBar.IsOpen = true;
    }

    /// <summary>
    /// Run one adapter operation, reporting a failure instead of crashing the page.
    ///
    /// Every error is logged before it is rendered, and the text shown is the one
    /// the service produced — the layers below already word their failures for a
    /// person ("Bluetooth is turned off", "Repair pairing to continue"), and
    /// replacing them with "Something went wrong" would throw that away.
    /// </summary>
    private async Task SafeAsync(Func<Task> operation)
    {
        BusyRing.IsActive = true;
        OperationBar.IsOpen = false;
        try
        {
            await operation();
        }
        catch (Exception error)
        {
            var message = ManagementErrorText.Summarize(error);
            adapters.Diagnostics.Error("ui", message);
            Report(message, InfoBarSeverity.Error);
        }
        finally
        {
            BusyRing.IsActive = false;
            Render();
        }
    }
}
