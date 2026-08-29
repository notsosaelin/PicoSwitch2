using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// How one controller reads in the list.
///
/// The rules here are protocol-meaningful rather than cosmetic: what counts as
/// Paired, what a remembered name may claim, and what a transport badge implies.
/// </summary>
public sealed class PeerRowTests
{
    private static PeerListing Listing(
        bool connected = false,
        bool bonded = false,
        bool historyOnly = false,
        bool fromHistory = false,
        PeerTransportSet transports = default,
        string name = "DualSense") => new()
    {
        PeerId = "p1",
        Address = "11:22:33:44:55:66",
        DisplayName = name,
        Section = PeerSection.Paired,
        Role = PeerRole.PhysicalController,
        RememberedRole = PeerRole.PhysicalController,
        Connected = connected,
        Bonded = bonded,
        HistoryOnly = historyOnly,
        IdentifiedFromHistory = fromHistory,
        Transports = transports,
    };

    private static PeerRow Row(PeerListing listing, bool canForget = true) =>
        PeerRows.Project([listing], canForget)[0];

    [Fact]
    public void AConnectionWithoutADurableCredentialIsCompletingPairingNotPaired()
    {
        // The bond may still fail. Showing it as done would have the user walk away
        // from a pairing that never finished.
        Assert.Equal(
            "Pairing · completing pairing",
            Row(Listing(connected: true, bonded: false)).Detail);
    }

    [Fact]
    public void AConnectedBondedPeerReadsAsConnected() =>
        Assert.StartsWith("Connected", Row(Listing(connected: true, bonded: true)).Detail);

    [Fact]
    public void AnOfflineBondedPeerIsStillPaired() =>
        Assert.StartsWith("Paired", Row(Listing(bonded: true)).Detail);

    [Fact]
    public void AHistoryOnlyPeerIsExplicitlyNotPaired()
    {
        // It is remembered locally but absent from the adapter's latest complete
        // inventory, so the adapter is no longer paired with it. Saying anything
        // softer would leave the user expecting it to reconnect.
        Assert.StartsWith("Not paired", Row(Listing(historyOnly: true)).Detail);
    }

    [Fact]
    public void AClassicOnlyPeerIsNoLessPairedThanAnLeOne()
    {
        // The badge is informational and must never read as a downgrade.
        var classic = Row(Listing(bonded: true, transports: PeerTransportSet.Of(PeerTransport.Classic)));
        var le = Row(Listing(bonded: true, transports: PeerTransportSet.Of(PeerTransport.Le)));

        Assert.StartsWith("Paired", classic.Detail);
        Assert.StartsWith("Paired", le.Detail);
        Assert.Contains("Bluetooth Classic", classic.Detail);
        Assert.Contains("Bluetooth LE", le.Detail);
    }

    [Fact]
    public void APeerOnBothTransportsSaysSo() =>
        Assert.Contains(
            "Bluetooth Classic and LE",
            Row(Listing(bonded: true, transports: PeerTransportSet.Of(PeerTransport.Classic, PeerTransport.Le))).Detail);

    [Fact]
    public void ARememberedNameIsAttributedRatherThanPresentedAsTheAdaptersAnswer()
    {
        // A remembered identity shown as a live one is exactly the promotion the
        // protocol forbids.
        Assert.Contains(
            "name remembered by this app",
            Row(Listing(bonded: true, fromHistory: true)).Detail);
    }

    [Fact]
    public void ALiveIdentityCarriesNoSuchCaveat() =>
        Assert.DoesNotContain("remembered", Row(Listing(bonded: true)).Detail);

    [Fact]
    public void ARecentRowNeverOffersForgetBecauseThereIsNoCredentialLeftToDelete() =>
        Assert.False(Row(Listing(historyOnly: true)).CanForget);

    [Fact]
    public void ForgetIsWithheldWhenTheAdapterCannotDoIt() =>
        Assert.False(Row(Listing(bonded: true), canForget: false).CanForget);

    [Fact]
    public void EveryRowActionCarriesTheControllerNameForScreenReaders()
    {
        // Every row renders the same verb, so a shared label announces
        // "Forget, Forget, Forget" and voice control cannot address one of them.
        var row = Row(Listing(bonded: true, name: "Pro Controller"));

        Assert.Equal("Forget Pro Controller", row.ForgetLabel);
        Assert.Equal("Remove Pro Controller from history", row.RemoveFromHistoryLabel);
    }
}

public sealed class RememberedAdapterAccessibilityTests
{
    [Fact]
    public void EveryAdapterRowActionCarriesTheAdapterName()
    {
        var row = RememberedAdapters.Project(
            new AdapterRegistry().With(
                AdapterRecord.Of("AA:BB:CC:DD:EE:01")! with { UserAlias = "Living room" }),
            activeId: null,
            connected: false,
            describeAge: _ => "yesterday")[0];

        Assert.Equal("Connect to Living room", row.ConnectLabel);
        Assert.Equal("Rename Living room", row.RenameLabel);
        Assert.Equal("Repair pairing with Living room", row.RepairLabel);
        Assert.Equal("Remove Living room", row.RemoveLabel);
    }
}
