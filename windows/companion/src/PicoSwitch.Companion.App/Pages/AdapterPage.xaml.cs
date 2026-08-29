using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using PicoSwitch.Companion.Services;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// Phase 2's test surface.
///
/// Deliberately not the Phase 3 dashboard. What this page exists to do is
/// exercise the parts of Phase 2 that can only be judged against real hardware:
/// the radio probe, discovery, the Windows pairing ceremony, the identity gate,
/// the recovery ladder, the peer inventory, and the repair path. Phase 3 replaces
/// the layout with the real dashboard and keeps the same service calls.
///
/// It calls <see cref="AdapterConnectionService"/> and nothing below it — no GATT
/// object, no management command string.
/// </summary>
public sealed partial class AdapterPage : Page
{
    private readonly AdapterConnectionService adapters = AppServices.Adapters;

    public AdapterPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        adapters.Connection.Changed += OnStateChanged;
        adapters.Snapshot.Changed += OnStateChanged;
        adapters.Registry.Changed += OnStateChanged;
        adapters.Relationship.Changed += OnStateChanged;
        adapters.Radio.Changed += OnStateChanged;

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
    }

    protected override void OnNavigatedTo(NavigationEventArgs e) => Render();

    // Every observable this page watches is written from a WinRT pool thread.
    private void OnStateChanged() => AppServices.OnUiThread(Render);

    private async void OnPair(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.PairNewAdapterAsync());

    private async void OnRefresh(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.RefreshAsync());

    private async void OnDisconnect(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.DisconnectAsync());

    private async void OnProbeRadio(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.ProbeRadioAsync());

    private async void OnConnectRow(object sender, RoutedEventArgs e)
    {
        if (RowId(sender) is { } id)
        {
            await SafeAsync(() => adapters.ConnectAsync(id));
        }
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
                "Windows will forget its pairing with this adapter. You will need to " +
                "double-tap the adapter's pairing button and pair again. The adapter's " +
                "name and its remembered controllers are kept.",
            PrimaryButtonText = "Repair",
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

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Remove this adapter?",
            Content =
                "It is removed from this app only. The Windows pairing and the adapter's " +
                "own paired controllers are untouched.",
            PrimaryButtonText = "Remove",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };

        if (await dialog.ShowAsync() == ContentDialogResult.Primary)
        {
            await SafeAsync(() => adapters.RemoveAsync(id));
        }
    }

    private static AdapterId? RowId(object sender) =>
        sender is FrameworkElement { Tag: string value } ? AdapterId.FromAddress(value) : null;

    /// <summary>
    /// Run one adapter operation, reporting a failure instead of crashing the page.
    ///
    /// Every error is logged before it is rendered, and the text shown is the one
    /// the service produced — the layers below already word their failures for a
    /// person ("Bluetooth is turned off", "Double-tap its pairing button"), and
    /// replacing them with "Something went wrong" would throw that away.
    /// </summary>
    private async Task SafeAsync(Func<Task> operation)
    {
        SetBusy(true);
        OperationBar.IsOpen = false;
        try
        {
            await operation();
        }
        catch (Exception error)
        {
            var message = Unwrap(error);
            adapters.Diagnostics.Error("ui", message);
            OperationBar.Severity = InfoBarSeverity.Error;
            OperationBar.Message = message;
            OperationBar.IsOpen = true;
        }
        finally
        {
            SetBusy(false);
            Render();
        }
    }

    private static string Unwrap(Exception error) => error switch
    {
        AggregateException aggregate =>
            string.Join(" · ", aggregate.InnerExceptions.Select(inner => inner.Message)),
        _ => error.Message,
    };

    private void SetBusy(bool busy)
    {
        PairProgress.IsActive = busy;
        PairButton.IsEnabled = !busy;
        RefreshButton.IsEnabled = !busy;
        DisconnectButton.IsEnabled = !busy;
        ProbeRadioButton.IsEnabled = !busy;
    }

    private void Render()
    {
        var radio = adapters.Radio.Value;
        RadioBar.Message = radio.ManagementBlockedReason ??
            $"Ready. {(radio.PeripheralRoleSupported ? "Peripheral role available." : "No peripheral role, so Controller Link is unavailable on this PC.")}";
        RadioBar.Severity = radio.ManagementBlockedReason is null
            ? InfoBarSeverity.Success
            : InfoBarSeverity.Warning;

        var connection = adapters.Connection.Value;
        var relationship = adapters.Relationship.Value;
        SessionPhase.Text = $"{relationship.Phase} · carrier {connection.Phase}";
        SessionDetail.Text = relationship.Message ?? connection.Message ??
            (connection.Connected ? "Connected." : "No adapter connected.");

        var snapshot = adapters.Snapshot.Value;
        SnapshotDetail.Text = snapshot.Firmware.Id.Length == 0
            ? string.Empty
            : $"id={snapshot.Firmware.Id}  version={snapshot.Firmware.Version}  " +
              $"build={snapshot.Firmware.Build}  bridgeContract={snapshot.Firmware.BridgeContract}\n" +
              $"personality={snapshot.Personality.Current.WireName()}  " +
              $"controller={snapshot.Controller.Name}  " +
              $"battery={(snapshot.Controller.BatteryValid ? snapshot.Controller.BatteryPercent + "%" : "n/a")}\n" +
              $"capabilities: peers={snapshot.Capabilities.Peers} forget={snapshot.Capabilities.PeerForget} " +
              $"pairing={snapshot.Capabilities.RemotePairing} kbm={snapshot.Capabilities.Kbm} " +
              $"amiibo={snapshot.Capabilities.Amiibo}";

        var registry = adapters.Registry.Value;
        var rows = registry.Records.Select(record => new Row(
            record.Id.Value,
            registry.NeedsShortLabel(record)
                ? $"{record.DisplayName} · {record.Id.ShortLabel}"
                : record.DisplayName,
            $"{record.Address}" +
            (registry.ActiveId == record.Id ? "  · selected" : string.Empty) +
            (record.RepairRequired ? "  · repair required" : string.Empty) +
            (record.LastFirmwareVersion is { } version ? $"  · last seen on {version}" : string.Empty)))
            .ToList();
        RegistryList.ItemsSource = rows;
        RegistryEmpty.Visibility = rows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

        var inventory = adapters.Inventory;
        var peers = new List<Row>();
        peers.AddRange(inventory.Connected.Select(peer => Peer(peer, "Connected")));
        peers.AddRange(inventory.Paired.Select(peer => Peer(peer, "Paired")));
        peers.AddRange(inventory.Recent.Select(peer => Peer(peer, "Recent")));
        peers.AddRange(inventory.Companion.Select(peer => Peer(peer, "This PC")));
        peers.AddRange(inventory.Unattributed.Select(peer => Peer(peer, "Unattributed")));
        PeerList.ItemsSource = peers;
        PeersEmpty.Visibility = peers.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private static Row Peer(PeerListing listing, string section) => new(
        listing.PeerId,
        listing.DisplayName,
        $"{section} · {listing.PeerId} · {listing.Address} · transports {listing.Transports}" +
        // A remembered identity shown as a live one is the promotion the protocol
        // forbids, so the row says which it is.
        (listing.IdentifiedFromHistory ? " · name from this app's history" : string.Empty) +
        (listing.Role == PeerRole.Unknown ? " · not yet identified by the adapter" : string.Empty));

    private sealed record Row(string Id, string Title, string Detail);
}
