using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Linux.App.Shell;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.App.Pages;

/// <summary>
/// The adapter as a device (docs/architecture/companion-shell-layout.md, "What
/// lives where"). A port of the WinUI AdapterPage, and bound by the same rule:
/// it paints <see cref="AdapterDashboardView"/> and calls
/// <see cref="AdapterConnectionService"/>, and decides nothing.
///
/// A header (name, firmware, Connect or Disconnect) over three tabs, so the
/// page fits without scrolling. The header carries no state words: the top bar
/// is the app's one connection indicator, and the same words a few pixels under
/// it read as two notifications for one fact.
///
/// - Console: the controller on the console, the emulated controller, the input
///   source.
/// - Controllers: the controllers paired to the ADAPTER (its credential store).
/// - Adapters: the adapters THIS COMPUTER remembers (BlueZ bonds).
///
/// The last two stay visibly separate tabs: different devices, different
/// credential stores, different destructive operations.
/// </summary>
internal sealed class AdapterPage : CompanionPage
{
    /// <summary>The tab last shown, kept across visits like Windows'.</summary>
    private static int? selectedTab;

    private readonly AdapterConnectionService adapters = AppServices.Adapters;
    private readonly List<long> inputIds = [];
    private readonly List<Personality> personalities = [];

    private readonly Notice radioBar = new(dismissible: false);
    private readonly Notice contractBar = new(dismissible: false);
    private readonly TextBlock adapterTitle = Ui.Title("No adapter");
    private readonly TextBlock phaseDetail = Ui.Caption();
    private readonly TextBlock firmwareText = Ui.Caption();
    private readonly Button connectButton;
    private readonly Button disconnectButton;
    private readonly Button refreshButton;

    private readonly TabBar tabs = new(("Console", Icons.Gamepad), ("Controllers", Icons.Link), ("Adapters", Icons.Bluetooth));

    // On a short window the tabs join the header row and the adapter's name,
    // which the top bar already shows, is dropped (OnShortWindowChanged).
    private readonly ContentControl headerTabsSlot = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly ContentControl pageTabsSlot = new();
    private readonly Control consolePanel;
    private readonly Control controllersPanel;
    private readonly Control adaptersPanel;

    private readonly TextBlock controllerValue = Ui.Value("No controller");
    private readonly TextBlock batteryText = Ui.Caption();

    private readonly ComboBox personalityBox = Ui.CardSelector(new ComboBox());
    private readonly Button personalityApply;
    private readonly TextBlock personalityReason = Ui.Caption();

    private readonly ComboBox inputBox = Ui.CardSelector(new ComboBox());
    private readonly Button inputApply;
    private readonly TextBlock inputReason = Ui.Caption();
    private readonly Notice inputTruncated = new(dismissible: false);

    private readonly Button pairControllerButton;
    private readonly TextBlock pairControllerReason = Ui.Caption();
    private readonly Notice peersUnsupported = new(dismissible: false);
    private readonly StackPanel peerSections = new() { Spacing = 4 };
    private readonly TextBlock peersEmpty = Ui.Caption("Connect an adapter to see its controllers.");

    private readonly Button pairButton;
    private readonly StackPanel adapterList = new() { Spacing = 4 };

    public AdapterPage(MainWindow shell)
        : base(shell)
    {
        connectButton = Ui.Button("Connect", async (_, _) => await OnConnectAsync(), accent: true);
        disconnectButton = Ui.Button("Disconnect", async (_, _) => await SafeAsync(adapters.DisconnectAsync, Render));
        refreshButton = Ui.IconButton(Icons.Refresh, "Refresh", async (_, _) => await SafeAsync(() => adapters.RefreshAsync(), Render));

        personalityApply = Ui.Button("Switch", async (_, _) => await OnSwitchPersonalityAsync());
        inputApply = Ui.Button("Use", async (_, _) => await OnSetInputAsync());
        inputTruncated.Show("Some sources were not listed", "The adapter reported more input sources than fit in one reply.", NoticeSeverity.Informational);

        pairControllerButton = Ui.Button("Pair", async (_, _) =>
            await new ControllerPairingPanel(adapters, Dialogs).RunAsync());
        pairButton = Ui.Button("Add", async (_, _) =>
            await SafeAsync(() => adapters.PairNewAdapterAsync(), Render), accent: true);

        // ToolTip: the console drops the controller briefly while it switches.
        ToolTip.SetTip(personalityBox, "The console briefly loses the controller while it switches.");
        ToolTip.SetTip(inputBox, "Exactly one source drives the console at a time.");

        consolePanel = Ui.Stack(
            Ui.Gap,
            Ui.Section(
                (string?)null,
                null,
                null,
                Ui.Setting(Ui.Text("Controller on the console"), batteryText, controllerValue),
                Ui.Setting("Emulated controller", null, personalityReason, personalityBox, personalityApply),
                Ui.Setting("Input source", null, inputReason, inputBox, inputApply)),
            inputTruncated);

        controllersPanel = Ui.Stack(
            Ui.Gap,
            Ui.Section(
                (string?)null,
                null,
                null,
                Ui.Setting("Pair a controller", null, pairControllerReason, pairControllerButton)),
            peersUnsupported,
            peersEmpty,
            peerSections);

        adaptersPanel = Ui.Stack(
            Ui.Gap,
            Ui.Section(
                (string?)null,
                null,
                null,
                Ui.Setting("Add an adapter", "Double-tap the adapter's pairing button first.", pairButton)),
            adapterList);

        tabs.SelectionChanged += index =>
        {
            selectedTab = index;
            ShowTab();
        };

        var header = new SettingRow(
            new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 16,
                Children = { headerTabsSlot, Ui.Stack(4, adapterTitle, firmwareText, phaseDetail) },
            },
            Ui.Row(Busy, connectButton, disconnectButton, refreshButton),
            minimumLabelWidth: 280)
        { Margin = new Thickness(0, 4, 0, 4) };

        Content = Ui.Page(
            header,
            radioBar,
            contractBar,
            Operation,
            pageTabsSlot,
            consolePanel,
            controllersPanel,
            adaptersPanel);
    }

    protected override async void OnShown()
    {
        adapters.Connection.Changed += OnStateChanged;
        adapters.Snapshot.Changed += OnStateChanged;
        adapters.Registry.Changed += OnStateChanged;
        adapters.Relationship.Changed += OnStateChanged;
        adapters.Radio.Changed += OnStateChanged;

        ShowTab();
        Render();
        await SafeAsync(() => adapters.ProbeRadioAsync(), Render);
    }

    protected override void OnHidden()
    {
        adapters.Connection.Changed -= OnStateChanged;
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Registry.Changed -= OnStateChanged;
        adapters.Relationship.Changed -= OnStateChanged;
        adapters.Radio.Changed -= OnStateChanged;
    }

    private void OnStateChanged() => AppServices.OnUiThread(Render);

    /// <summary>One row of chrome instead of three on a short window, as on Android.</summary>
    protected override void OnShortWindowChanged(bool isShort)
    {
        headerTabsSlot.Content = null;
        pageTabsSlot.Content = null;
        adapterTitle.IsVisible = !isShort;
        headerTabsSlot.IsVisible = isShort;
        pageTabsSlot.IsVisible = !isShort;
        if (isShort)
        {
            headerTabsSlot.Content = tabs;
        }
        else
        {
            pageTabsSlot.Content = tabs;
        }
    }

    /// <summary>Opens on Adapters while there is none to show on the other two.</summary>
    private void ShowTab()
    {
        selectedTab ??= adapters.Registry.Value.Records.Count == 0 ? 2 : 0;
        if (tabs.SelectedIndex != selectedTab)
        {
            tabs.Select(selectedTab.Value, raise: false);
        }

        consolePanel.IsVisible = selectedTab == 0;
        controllersPanel.IsVisible = selectedTab == 1;
        adaptersPanel.IsVisible = selectedTab == 2;
    }

    /* ------------------------------------------------------------- actions */

    private AdapterRecord? SelectedAdapter()
    {
        var registry = adapters.Registry.Value;
        return registry.ActiveId is { } id ? registry.Record(id) : registry.Records.FirstOrDefault();
    }

    private async Task OnConnectAsync()
    {
        if (SelectedAdapter() is { } record)
        {
            await SafeAsync(() => adapters.ConnectAsync(record.Id), Render);
        }
    }

    private async Task OnSwitchPersonalityAsync()
    {
        if (personalityBox.SelectedIndex < 0 || personalityBox.SelectedIndex >= personalities.Count)
        {
            return;
        }

        var personality = personalities[personalityBox.SelectedIndex];
        await SafeAsync(async () =>
        {
            var outcome = await adapters.SetPersonalityAsync(personality);
            if (outcome.Unchanged)
            {
                Report("The adapter was already in that mode.", NoticeSeverity.Informational);
            }
            else if (outcome.Reenumerated)
            {
                Report($"Switched to {ControllerModeSection.Label(outcome.Personality)}.", NoticeSeverity.Success);
            }
            else
            {
                // The user is looking at a console that still shows the old
                // controller; this needs its own instruction.
                Report(
                    $"The adapter switched to {ControllerModeSection.Label(outcome.Personality)}, " +
                    "but the console has not seen the change yet. Unplug the adapter and plug it back in.",
                    NoticeSeverity.Warning);
            }
        }, Render);
    }

    private async Task OnSetInputAsync()
    {
        if (inputBox.SelectedIndex < 0 || inputBox.SelectedIndex >= inputIds.Count)
        {
            return;
        }

        var id = inputIds[inputBox.SelectedIndex];
        await SafeAsync(() => adapters.SetActiveInputAsync(id), Render);
    }

    private async Task OnRenameAsync(AdapterId id)
    {
        if (adapters.Registry.Value.Record(id) is not { } record)
        {
            return;
        }

        var input = new TextBox
        {
            Text = record.UserAlias ?? string.Empty,
            Watermark = record.LastKnownName,
            MaxLength = 64,
            MinWidth = 320,
        };

        var choice = await Dialogs.ShowAsync("Rename adapter", input, ["Save", "Clear name", "Cancel"], primaryIndex: 0, defaultIndex: 0);
        if (choice is < 0 or 2)
        {
            return;
        }

        // A local nickname only. It never reaches the adapter and is never part
        // of its identity, which stays the Bluetooth address.
        var alias = choice == 0 ? input.Text : null;
        await SafeAsync(() => adapters.RenameAsync(id, alias), Render);
    }

    private async Task OnRepairAsync(AdapterId id)
    {
        // Unpairing destroys trust, so it is never automatic and the
        // confirmation names the consequence.
        if (await Dialogs.ConfirmAsync(
                "Repair pairing?",
                "This computer drops its pairing with this adapter, then you pair again with " +
                "the adapter's pairing window open.\n\n" +
                "The adapter stays in this list, and its name and controller history are kept.",
                "Repair pairing"))
        {
            await SafeAsync(() => adapters.RepairAsync(id), Render);
        }
    }

    private async Task OnRemoveAsync(AdapterId id)
    {
        if (!await Dialogs.ConfirmAsync(
                "Remove this adapter?",
                "This app forgets the adapter and its controller history, and this computer " +
                "drops its pairing with it. You will need to pair again, with the adapter's " +
                "pairing window open.\n\n" +
                "The controllers paired to the adapter itself are not affected.",
                "Remove and unpair"))
        {
            return;
        }

        await SafeAsync(async () =>
        {
            var removal = await adapters.RemoveAsync(id);
            if (removal.LeftOrphanPairing)
            {
                throw new ManagementException(
                    "The adapter was removed from this app, but this computer could not drop its " +
                    "pairing. Remove it in your Bluetooth settings as well.");
            }
        }, Render);
    }

    private async Task OnForgetAsync(string peerId)
    {
        if (!await Dialogs.ConfirmAsync(
                "Forget this controller?",
                "The adapter deletes its pairing with the controller. If the controller is " +
                "connected it will disconnect.\n\n" +
                "It stays in this list under Recent, so you can see it was here.",
                "Forget"))
        {
            return;
        }

        await SafeAsync(async () =>
        {
            var outcome = await adapters.ForgetControllerAsync(peerId);

            // Incomplete stays visible: the adapter is saying it did not finish.
            if (outcome.Result == PeerForgetResult.Incomplete)
            {
                Report(
                    "The adapter reported that it could not fully remove that controller. " +
                    "Refresh and check the list.",
                    NoticeSeverity.Warning);
            }
        }, Render);
    }

    /* ------------------------------------------------------------ painting */

    private void Render()
    {
        var selected = SelectedAdapter();
        var view = AdapterDashboard.Project(
            adapters.Snapshot.Value,
            adapters.Relationship.Value,
            adapters.Connection.Value,
            selected,
            AppServices.ControllerLinkStreaming);

        if (adapters.Radio.Value.ManagementBlockedReason is { } blocked)
        {
            radioBar.Show("Bluetooth", blocked, NoticeSeverity.Warning);
        }
        else
        {
            radioBar.Hide();
        }

        // The silence rule, as one branch.
        if (view.Contract.Visible)
        {
            contractBar.Show("Firmware compatibility", view.Contract.Summary, NoticeSeverity.Warning);
        }
        else
        {
            contractBar.Hide();
        }

        adapterTitle.Text = view.Title;

        // Only a real message (why a connect failed). "Add one below." used to
        // sit here, repeating the Adapters tab's "Add an adapter" row.
        Ui.SetOptional(phaseDetail, view.Connected ? null : view.PhaseDetail);
        Ui.SetOptional(firmwareText, view.FirmwareLine);

        controllerValue.Text = view.ControllerLine;
        Ui.SetOptional(batteryText, view.BatteryLine);

        // One of Connect / Disconnect at a time, as Play shows Start or Stop.
        connectButton.IsVisible = !view.Connected;
        disconnectButton.IsVisible = view.Connected;
        connectButton.IsEnabled = selected is not null && !view.Connected && adapters.Radio.Value.ManagementBlockedReason is null;
        refreshButton.IsEnabled = view.Connected;

        RenderPersonality(view.ControllerMode);
        RenderInput(view.ConsoleInput);
        RenderLists();
    }

    private void RenderPersonality(ControllerModeSection section)
    {
        personalities.Clear();
        personalities.AddRange(section.Available);
        personalityBox.ItemsSource = personalities.Select(ControllerModeSection.Label).ToArray();
        personalityBox.SelectedIndex = personalities.IndexOf(section.Current);
        Ui.Gate(section.Availability, personalityReason, personalityBox, personalityApply);
    }

    private void RenderInput(ConsoleInputSection section)
    {
        inputIds.Clear();
        inputIds.AddRange(section.Sources.Select(source => source.Id));
        inputBox.ItemsSource = section.Sources
            .Select(source => source.IsActive ? $"{source.Label} (in use)" : source.Label)
            .ToArray();
        inputBox.SelectedIndex = inputIds.IndexOf(section.ActiveId);
        inputTruncated.IsVisible = section.Truncated;
        Ui.Gate(section.Availability, inputReason, inputBox, inputApply);
    }

    /// <summary>This computer's adapters, and the controllers paired to the adapter.</summary>
    private void RenderLists()
    {
        var rows = adapters.RememberedAdapters();
        // No "No adapters yet." line: the "Add an adapter" row above already
        // says so.
        adapterList.IsVisible = rows.Count > 0;
        adapterList.Children.Clear();
        foreach (var row in rows)
        {
            adapterList.Children.Add(AdapterRow(row));
        }

        var controllers = adapters.Controllers();
        if (controllers.Visible)
        {
            peersUnsupported.Hide();
        }
        else
        {
            peersUnsupported.Show("Controller list unavailable", controllers.HiddenReason ?? string.Empty, NoticeSeverity.Informational);
        }

        pairControllerButton.IsEnabled = controllers.CanPairNewController;
        Ui.SetOptional(pairControllerReason, controllers.PairDisabledReason);

        var inventory = controllers.Inventory;
        peerSections.Children.Clear();
        AddPeerSection("Connected", inventory.Connected, controllers.CanForget);
        AddPeerSection("Paired", inventory.Paired, controllers.CanForget);

        // Recent rows never offer Forget: there is no adapter credential left,
        // only a local history row.
        AddPeerSection("Recent", inventory.Recent, canForget: false);
        peerSections.IsVisible = controllers.Visible && inventory.HasControllers;
        peersEmpty.IsVisible = controllers.Visible && !inventory.HasControllers;
    }

    private Control AdapterRow(RememberedAdapterRow row)
    {
        var detail = Ui.Caption(row.StateText);
        if (!string.IsNullOrWhiteSpace(row.Detail))
        {
            detail.Text = $"{row.StateText} · {row.Detail}";
        }

        var connect = Ui.Button("Connect", async (_, _) => await SafeAsync(() => adapters.ConnectAsync(row.Id), Render), row.ConnectLabel);
        connect.IsEnabled = row.CanConnect;
        var rename = Ui.Button("Rename", async (_, _) => await OnRenameAsync(row.Id), row.RenameLabel);
        var repair = Ui.Button("Repair", async (_, _) => await OnRepairAsync(row.Id), row.RepairLabel);
        var remove = Ui.Button("Remove", async (_, _) => await OnRemoveAsync(row.Id), row.RemoveLabel);

        return Ui.Card(Ui.Setting(Ui.Text(row.Title), detail, connect, rename, repair, remove));
    }

    private void AddPeerSection(string heading, IReadOnlyList<PeerListing> listings, bool canForget)
    {
        if (listings.Count == 0)
        {
            return;
        }

        var label = Ui.Caption(heading);
        label.FontWeight = Avalonia.Media.FontWeight.SemiBold;
        label.Margin = new Thickness(1, 8, 0, 0);
        peerSections.Children.Add(label);
        foreach (var row in PeerRows.Project(listings, canForget))
        {
            var action = row.CanForget
                ? Ui.Button("Forget", async (_, _) => await OnForgetAsync(row.PeerId), row.ForgetLabel)
                : Ui.Button("Remove", async (_, _) =>
                    await SafeAsync(() => adapters.RemoveFromHistoryAsync(row.PeerId), Render), row.RemoveFromHistoryLabel);
            peerSections.Children.Add(Ui.Card(Ui.Setting(Ui.Text(row.Title), Ui.Caption(row.Detail), action)));
        }
    }
}
