using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// One remembered adapter, as the Settings list shows it.
///
/// The four states are history-qualified and mutually exclusive (§16.2). They
/// exist separately because a user deciding whether to press Repair needs to know
/// which one they are looking at, and "not connected" covers three very different
/// situations.
/// </summary>
public enum RememberedAdapterState
{
    /// <summary>Live management session right now.</summary>
    Connected,

    /// <summary>The selected adapter, without a session.</summary>
    Selected,

    /// <summary>Known, remembered, not selected.</summary>
    Remembered,

    /// <summary>Windows holds a pairing the adapter no longer recognises.</summary>
    RepairRequired,
}

public sealed record RememberedAdapterRow
{
    public required AdapterId Id { get; init; }

    /// <summary>The normalized address, which is what a view round-trips as a tag.</summary>
    public string Address => Id.Value;

    /// <summary>Alias, then last known name, then <c>PicoSwitch2 · &lt;short-id&gt;</c>.</summary>
    public required string Title { get; init; }

    public required RememberedAdapterState State { get; init; }

    public required string StateText { get; init; }

    public string? Detail { get; init; }

    /// <summary>
    /// Repair is offered on every row, not only broken ones.
    ///
    /// A stale bond is not always detected before the user gives up — and a repair
    /// on a healthy row costs one re-pair, while hiding it behind a diagnosis the
    /// app failed to make leaves no way out at all. It stays behind a confirmation
    /// because it destroys trust.
    /// </summary>
    public bool CanRepair => true;

    public bool CanConnect => State != RememberedAdapterState.Connected;

    /*
     * Accessible names for the row actions.
     *
     * Every row renders the same four words, so a screen reader announces
     * "Connect, Connect, Connect" down a list of adapters and voice control
     * cannot address one of them. The visible label stays short; the accessible
     * name carries the row identity, which is what makes the control usable
     * without sight of the layout.
     */
    public string ConnectLabel => $"Connect to {Title}";

    public string RenameLabel => $"Rename {Title}";

    public string RepairLabel => $"Repair pairing with {Title}";

    public string RemoveLabel => $"Remove {Title}";
}

public static class RememberedAdapters
{
    public static IReadOnlyList<RememberedAdapterRow> Project(
        AdapterRegistry registry,
        AdapterId? activeId,
        bool connected,
        Func<long, string> describeAge) =>
        registry.Records
            .Select(record => Row(record, activeId, connected, describeAge))
            .ToArray();

    private static RememberedAdapterRow Row(
        AdapterRecord record,
        AdapterId? activeId,
        bool connected,
        Func<long, string> describeAge)
    {
        var isActive = activeId == record.Id;
        var state =
            record.RepairRequired ? RememberedAdapterState.RepairRequired
            : isActive && connected ? RememberedAdapterState.Connected
            : isActive ? RememberedAdapterState.Selected
            : RememberedAdapterState.Remembered;

        return new RememberedAdapterRow
        {
            Id = record.Id,
            Title = record.DisplayName,
            State = state,
            StateText = state switch
            {
                RememberedAdapterState.Connected => "Connected",
                RememberedAdapterState.Selected => "Selected, not connected",
                RememberedAdapterState.RepairRequired => "Repair required",
                _ => "Remembered",
            },

            // Last-connected is the fact that makes an offline row meaningful:
            // "yesterday" and "never" call for different actions.
            Detail = record.LastConnectedAtMillis is { } millis
                ? $"Last connected {describeAge(millis)}"
                : "Never connected",
        };
    }
}

/// <summary>
/// The Paired Controllers card, including how it degrades on older firmware.
///
/// The projection of peers into Connected / Paired / Recent already lives in
/// <see cref="ControllerInventory"/>. What this adds is the capability gating,
/// which has its own rule: families degrade INDEPENDENTLY and one missing
/// capability never hides the whole card.
/// </summary>
public sealed record ControllerListView
{
    public required ControllerInventoryView Inventory { get; init; }

    /// <summary>False only when the adapter explicitly reports no peer list.</summary>
    public required bool Visible { get; init; }

    public string? HiddenReason { get; init; }

    /// <summary>Forget appears on Connected and Paired rows unless explicitly unsupported.</summary>
    public required bool CanForget { get; init; }

    public string? ForgetDisabledReason { get; init; }

    public required bool CanPairNewController { get; init; }

    public string? PairDisabledReason { get; init; }
}

public static class ControllerList
{
    public static ControllerListView Project(
        AdapterSnapshot snapshot,
        AdapterPeerHistory history,
        bool connected)
    {
        var capabilities = snapshot.Capabilities;
        var inventory = ControllerInventory.Build(snapshot.Peers, history);

        // Only an EXPLICIT unsupported hides the card. Unknown means the probe did
        // not establish an answer, and hiding a working list because a probe timed
        // out is the same mistake as disabling a working control.
        var listSupported = capabilities.Peers != CapabilityState.Unsupported;

        return new ControllerListView
        {
            Inventory = inventory,
            Visible = listSupported,
            HiddenReason = listSupported
                ? null
                : "This adapter's firmware cannot list its paired controllers. " +
                  "Everything else on this page still works.",
            CanForget = connected && capabilities.PeerForget != CapabilityState.Unsupported,
            ForgetDisabledReason =
                capabilities.PeerForget == CapabilityState.Unsupported
                    ? "This adapter's firmware cannot forget an individual controller. " +
                      "The list is read-only."
                    : connected ? null : AdapterDashboard.NotConnected,
            CanPairNewController = connected && capabilities.RemotePairing != CapabilityState.Unsupported,
            PairDisabledReason =
                capabilities.RemotePairing == CapabilityState.Unsupported
                    ? "This adapter's firmware cannot start pairing from the app. " +
                      "Use the adapter's own pairing button."
                    : connected ? null : AdapterDashboard.NotConnected,
        };
    }
}

/// <summary>
/// A remote controller-pairing attempt, as the dialog shows it.
///
/// The operation is pinned to its <c>op</c> generation so a reply from a previous
/// attempt — or from the adapter the user just switched away from — cannot drive
/// this view. That is the same generation discipline the transport uses, applied
/// to a long-running protocol operation.
/// </summary>
public sealed record RemotePairingView(
    long Operation,
    bool Active,
    string Headline,
    string? Detail,
    int Candidates,
    long RemainingMillis,
    bool CanCancel,
    bool PointsToForget)
{
    public string RemainingText => RemainingMillis <= 0
        ? string.Empty
        : $"{Math.Ceiling(RemainingMillis / 1000.0):0} s left";
}

public static class RemotePairing
{
    public static RemotePairingView Project(PairingStatus status) => new(
        Operation: status.Operation,
        Active: status.Active,
        Headline: Headline(status),
        Detail: Detail(status),
        Candidates: status.Candidates,
        RemainingMillis: status.RemainingMillis,

        // Cancel is a courtesy: the firmware closes its own window. Offering it
        // while nothing is running would imply the app owns the window.
        CanCancel: status.Active,

        // storage_full is the one blocked reason with a direct remedy in this very
        // card, so it points there instead of just naming the failure.
        PointsToForget: status.Reason == PairingReason.StorageFull);

    private static string Headline(PairingStatus status) => status.State switch
    {
        PairingState.Discovering => "Put the controller in pairing mode",
        PairingState.Connecting => "Connecting to the controller",
        PairingState.Paired => "Controller paired",
        PairingState.TimedOut => "No controller paired in time",
        PairingState.Cancelled => "Pairing cancelled",
        PairingState.Blocked => "The adapter could not start pairing",
        PairingState.Idle => "Ready to pair a controller",
        _ => "Pairing status unknown",
    };

    /// <summary>
    /// Every blocked reason stays distinct.
    ///
    /// Collapsing them into "pairing failed" throws away the only part of the
    /// message that tells the user what to do differently, and they call for four
    /// genuinely different actions.
    /// </summary>
    private static string? Detail(PairingStatus status) => status.Reason switch
    {
        PairingReason.StorageFull =>
            "The adapter has no room for another controller. Forget one below, then try again.",
        PairingReason.NoController =>
            "The adapter found no controller in pairing mode.",
        PairingReason.ManagementDisabled =>
            "Management is turned off on the adapter.",
        PairingReason.Busy =>
            "The adapter is busy with another operation. Try again in a moment.",
        PairingReason.LockedOut =>
            "The adapter is temporarily locked out after too many attempts.",
        PairingReason.Unknown =>
            "The adapter reported a reason this build does not recognise.",
        _ => status.Candidates > 0
            ? $"{status.Candidates} controller{(status.Candidates == 1 ? "" : "s")} responding"
            : null,
    };
}


/// <summary>
/// One controller row, ready to render.
///
/// The label hierarchy and the attribution rules are here because both are
/// protocol-meaningful and easy to get subtly wrong in XAML.
/// </summary>
public sealed record PeerRow
{
    public required string PeerId { get; init; }

    public required string Title { get; init; }

    public required string Detail { get; init; }

    public required bool CanForget { get; init; }

    /// <summary>See the note on the adapter row: one shared verb per list is
    /// unusable by screen reader or by voice.</summary>
    public string ForgetLabel => $"Forget {Title}";

    public string RemoveFromHistoryLabel => $"Remove {Title} from history";
}

public static class PeerRows
{
    public static IReadOnlyList<PeerRow> Project(
        IReadOnlyList<PeerListing> listings,
        bool canForget) =>
        listings.Select(listing => Row(listing, canForget)).ToArray();

    private static PeerRow Row(PeerListing listing, bool canForget) => new()
    {
        PeerId = listing.PeerId,
        Title = listing.DisplayName,
        Detail = Detail(listing),

        // Recent rows have no adapter credential left to delete; their only action
        // is to leave local history.
        CanForget = canForget && !listing.HistoryOnly,
    };

    private static string Detail(PeerListing listing)
    {
        var parts = new List<string> { StateText(listing) };

        if (Transports(listing) is { } transports)
        {
            // Informational only. A Classic-only peer is no less Paired than an
            // LE-only one, and the badge must never read as a downgrade.
            parts.Add(transports);
        }

        if (listing.IdentifiedFromHistory)
        {
            // A remembered identity shown as a live one is exactly the promotion
            // the protocol forbids, so it is attributed rather than presented as
            // the adapter's own answer.
            parts.Add("name remembered by this app");
        }

        if (listing.Classification is { } classification && !string.IsNullOrWhiteSpace(classification))
        {
            parts.Add(classification);
        }

        return string.Join(" · ", parts);
    }

    /// <summary>
    /// A connection without a durable credential is **Completing pairing**, never
    /// silently treated as Paired: the bond may still fail, and showing it as done
    /// would have the user walk away from a pairing that never finished.
    /// </summary>
    private static string StateText(PeerListing listing) => listing switch
    {
        { Connected: true, Bonded: false } => "Pairing · completing pairing",
        { Connected: true } => "Connected",
        { HistoryOnly: true } => "Not paired",
        { Bonded: true } => "Paired",
        _ => "Not paired",
    };

    private static string? Transports(PeerListing listing)
    {
        var classic = listing.Transports.Contains(PeerTransport.Classic);
        var le = listing.Transports.Contains(PeerTransport.Le);
        return (classic, le) switch
        {
            (true, true) => "Bluetooth Classic and LE",
            (true, false) => "Bluetooth Classic",
            (false, true) => "Bluetooth LE",
            _ => null,
        };
    }
}
