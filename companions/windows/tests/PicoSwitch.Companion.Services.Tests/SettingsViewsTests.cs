using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

public sealed class RememberedAdapterRowTests
{
    private static readonly AdapterId First = AdapterId.FromAddress("AA:BB:CC:DD:EE:01")!.Value;
    private static readonly AdapterId Second = AdapterId.FromAddress("AA:BB:CC:DD:EE:02")!.Value;

    private static AdapterRegistry Registry(params AdapterRecord[] records) =>
        records.Aggregate(new AdapterRegistry(), (registry, record) => registry.With(record));

    private static IReadOnlyList<RememberedAdapterRow> Project(
        AdapterRegistry registry,
        AdapterId? active = null,
        bool connected = false) =>
        RememberedAdapters.Project(registry, active, connected, _ => "yesterday");

    [Fact]
    public void EveryRememberedAdapterAppearsNotOnlyTheSelectedOne()
    {
        // The registry is many rows, not one singleton relationship (I1). A list
        // that showed only the active adapter would make the other one look lost.
        var rows = Project(Registry(
            AdapterRecord.Of("AA:BB:CC:DD:EE:01")!,
            AdapterRecord.Of("AA:BB:CC:DD:EE:02")!));

        Assert.Equal(2, rows.Count);
    }

    [Fact]
    public void TheFourStatesAreDistinguishable()
    {
        var registry = Registry(
            AdapterRecord.Of("AA:BB:CC:DD:EE:01")! with { LastConnectedAtMillis = 1 },
            AdapterRecord.Of("AA:BB:CC:DD:EE:02")!);

        // Active + live session.
        Assert.Equal(
            RememberedAdapterState.Connected,
            Project(registry, First, connected: true)[0].State);

        // Active, no session -- a different situation from "never selected".
        Assert.Equal(
            RememberedAdapterState.Selected,
            Project(registry, First)[0].State);

        // Known but not selected.
        Assert.Equal(
            RememberedAdapterState.Remembered,
            Project(registry, First)[1].State);

        // Broken trust outranks all of the above: it is the one state with an
        // action attached.
        var broken = Registry(AdapterRecord.Of("AA:BB:CC:DD:EE:01")! with { RepairRequired = true });
        Assert.Equal(
            RememberedAdapterState.RepairRequired,
            Project(broken, First, connected: true)[0].State);
    }

    [Fact]
    public void TheTitleFollowsTheAliasThenNameThenShortIdHierarchy()
    {
        Assert.Equal(
            "Living room",
            Project(Registry(AdapterRecord.Of("AA:BB:CC:DD:EE:01", "PicoSwitch2")! with
            {
                UserAlias = "Living room",
            }))[0].Title);

        Assert.Equal(
            "Front room unit",
            Project(Registry(AdapterRecord.Of("AA:BB:CC:DD:EE:01", "Front room unit")!))[0].Title);
    }

    [Fact]
    public void ANeverConnectedRowSaysSoRatherThanShowingNothing()
    {
        // "Never connected" and "last connected yesterday" call for different
        // actions; an empty detail line answers neither question.
        Assert.Equal("Never connected", Project(Registry(AdapterRecord.Of("AA:BB:CC:DD:EE:01")!))[0].Detail);
    }

    [Fact]
    public void RepairIsOfferedOnEveryRowIncludingHealthyOnes()
    {
        // A stale bond is not always detected before the user gives up. Hiding
        // Repair behind a diagnosis the app failed to make leaves no way out.
        Assert.All(
            Project(Registry(AdapterRecord.Of("AA:BB:CC:DD:EE:01")!), First, connected: true),
            row => Assert.True(row.CanRepair));
    }

    [Fact]
    public void ConnectIsOfferedOnEverythingExceptTheLiveOne()
    {
        var registry = Registry(
            AdapterRecord.Of("AA:BB:CC:DD:EE:01")!,
            AdapterRecord.Of("AA:BB:CC:DD:EE:02")!);
        var rows = Project(registry, First, connected: true);

        Assert.False(rows[0].CanConnect);

        // Selecting B starts the handoff; it never deletes A.
        Assert.True(rows[1].CanConnect);
        Assert.Equal(Second, rows[1].Id);
    }
}

/// <summary>
/// Paired Controllers, and how it degrades one family at a time.
/// </summary>
public sealed class ControllerListViewTests
{
    private static ControllerListView Project(
        AdapterCapabilities capabilities,
        bool connected = true) =>
        ControllerList.Project(
            new AdapterSnapshot { Capabilities = capabilities },
            new AdapterPeerHistory(),
            connected);

    [Fact]
    public void AnUnsupportedPeerListHidesTheCardButNotTheAdapter()
    {
        // "One missing capability must never hide the whole adapter."
        var view = Project(new AdapterCapabilities(Peers: CapabilityState.Unsupported));

        Assert.False(view.Visible);
        Assert.Contains("Everything else on this page still works", view.HiddenReason);
    }

    [Fact]
    public void AWorkingListWithUnsupportedForgetStaysVisibleAndReadOnly()
    {
        // `peers list` shipped a phase before `peers forget`. Treating them as one
        // capability would either hide a working list or offer a button that
        // answers `unknown command`.
        var view = Project(new AdapterCapabilities(
            Peers: CapabilityState.Available,
            PeerForget: CapabilityState.Unsupported));

        Assert.True(view.Visible);
        Assert.False(view.CanForget);
        Assert.Contains("read-only", view.ForgetDisabledReason);
    }

    [Fact]
    public void UnsupportedRemotePairingKeepsTheListAndPointsAtThePhysicalButton()
    {
        var view = Project(new AdapterCapabilities(
            Peers: CapabilityState.Available,
            RemotePairing: CapabilityState.Unsupported));

        Assert.True(view.Visible);
        Assert.False(view.CanPairNewController);
        Assert.Contains("pairing button", view.PairDisabledReason);
    }

    [Fact]
    public void UnknownIsNeverRenderedAsUnsupported()
    {
        // The rule that keeps a probe timeout from disabling working features.
        var view = Project(new AdapterCapabilities(
            Peers: CapabilityState.Unknown,
            PeerForget: CapabilityState.Unknown,
            RemotePairing: CapabilityState.Unknown));

        Assert.True(view.Visible);
        Assert.True(view.CanForget);
        Assert.True(view.CanPairNewController);
    }

    [Fact]
    public void BeingDisconnectedDisablesActionsWithoutClaimingTheFirmwareLacksThem()
    {
        var view = Project(
            new AdapterCapabilities(PeerForget: CapabilityState.Available),
            connected: false);

        Assert.False(view.CanForget);
        Assert.Equal(AdapterDashboard.NotConnected, view.ForgetDisabledReason);
    }
}

public sealed class RemotePairingViewTests
{
    [Fact]
    public void StorageFullPointsStraightAtSelectiveForget()
    {
        // The one blocked reason whose remedy is in this very card.
        var view = RemotePairing.Project(new PairingStatus(
            Operation: 3,
            State: PairingState.Blocked,
            Reason: PairingReason.StorageFull));

        Assert.True(view.PointsToForget);
        Assert.Contains("Forget one below", view.Detail);
    }

    [Theory]
    [InlineData(PairingReason.NoController, "no controller in pairing mode")]
    [InlineData(PairingReason.ManagementDisabled, "Management is turned off")]
    [InlineData(PairingReason.Busy, "busy with another operation")]
    [InlineData(PairingReason.LockedOut, "locked out")]
    public void EveryBlockedReasonKeepsItsOwnInstruction(PairingReason reason, string expected)
    {
        // Collapsing these into "pairing failed" throws away the only part that
        // tells the user what to do differently.
        var view = RemotePairing.Project(
            new PairingStatus(State: PairingState.Blocked, Reason: reason));

        Assert.Contains(expected, view.Detail);
        Assert.False(view.PointsToForget);
    }

    [Fact]
    public void AnActiveSearchShowsItsCountdownAndOffersCancel()
    {
        var view = RemotePairing.Project(new PairingStatus(
            Operation: 9,
            State: PairingState.Discovering,
            RemainingMillis: 12_400,
            Candidates: 2));

        Assert.True(view.Active);
        Assert.True(view.CanCancel);
        Assert.Equal("13 s left", view.RemainingText);
        Assert.Contains("2 controllers responding", view.Detail);
    }

    [Fact]
    public void CancelIsNotOfferedWhenNothingIsRunning()
    {
        // Firmware closes its own window; offering Cancel while idle would imply
        // the app owns it.
        var view = RemotePairing.Project(new PairingStatus(State: PairingState.Idle));

        Assert.False(view.CanCancel);
        Assert.Empty(view.RemainingText);
    }

    [Fact]
    public void TheOperationGenerationIsCarriedSoAStaleReplyCannotDriveTheView()
    {
        // Pinned to the adapter and its op generation: switching adapters must drop
        // the old view rather than letting a trailing reply repaint it.
        Assert.Equal(41, RemotePairing.Project(new PairingStatus(Operation: 41)).Operation);
    }

    [Theory]
    [InlineData(PairingState.Paired, "Controller paired")]
    [InlineData(PairingState.TimedOut, "No controller paired in time")]
    [InlineData(PairingState.Cancelled, "Pairing cancelled")]
    public void TerminalStatesReadAsOutcomesNotErrors(PairingState state, string headline) =>
        Assert.Equal(headline, RemotePairing.Project(new PairingStatus(State: state)).Headline);
}
