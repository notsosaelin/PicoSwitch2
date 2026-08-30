using PicoSwitch.Companion.Services;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The Paired Controllers projection.
///
/// Two failures decide the shape of every test here: a peer list that silently
/// loses a row shows fewer saved controllers than the adapter holds, and a
/// management relationship rendered as a controller is a row the user would
/// eventually try to forget — which is their own PC.
/// </summary>
public sealed class ControllerInventoryTests
{
    private const long T0 = 1_000_000;

    private static PeerInfo Peer(
        string id,
        PeerRole role = PeerRole.PhysicalController,
        bool bonded = true,
        bool connected = false,
        string? name = null,
        string? classification = null,
        string address = "AA:BB:CC:DD:EE:FF") =>
        new(id, address, role, PeerTransportSet.Of(PeerTransport.Classic), bonded, connected, name, classification);

    private static PeerInventory Inventory(params PeerInfo[] peers) => new()
    {
        Peers = new ValueList<PeerInfo>(peers),
        Complete = true,
        Total = peers.Length,
    };

    [Fact]
    public void SectionsAreDecidedByBondAndConnectionNotByRole()
    {
        var view = ControllerInventory.Build(
            Inventory(
                Peer("p_conn", connected: true, name: "Live"),
                Peer("p_paired", name: "Saved"),
                Peer("p_none", bonded: false, name: "Neither")),
            new AdapterPeerHistory());

        Assert.Equal("p_conn", Assert.Single(view.Connected).PeerId);
        Assert.Equal("p_paired", Assert.Single(view.Paired).PeerId);
        Assert.Equal("p_none", Assert.Single(view.Unattributed).PeerId);
    }

    [Fact]
    public void AnUnidentifiedButBondedPeerIsStillAPairedController()
    {
        // Routing on role hid real hardware: after the adapter reboots EVERY paired
        // controller reads `unknown` until it reconnects, and the person who paired
        // it would open Paired Controllers and find it empty.
        var view = ControllerInventory.Build(
            Inventory(Peer("p_1", role: PeerRole.Unknown)),
            new AdapterPeerHistory());

        var listing = Assert.Single(view.Paired);
        Assert.Equal(PeerRole.Unknown, listing.Role);
        Assert.True(view.HasControllers);
    }

    [Fact]
    public void ThisPcNeverAppearsAsAController()
    {
        var view = ControllerInventory.Build(
            Inventory(
                Peer("p_mgmt", role: PeerRole.ManagementCompanion, connected: true),
                Peer("p_link", role: PeerRole.ControllerLink),
                Peer("p_pad", name: "DualSense")),
            new AdapterPeerHistory());

        Assert.Equal(["p_link", "p_mgmt"], view.Companion.Select(row => row.PeerId).Order().ToList());
        Assert.Empty(view.Connected);
        Assert.Equal("p_pad", Assert.Single(view.Paired).PeerId);
    }

    [Fact]
    public void RememberedCompanionEvidenceStillExcludesThePcAfterAReboot()
    {
        // The cost of being wrong here is a PC shown under "This PC"; the cost of
        // the opposite mistake is offering to forget the management relationship.
        var history = new AdapterPeerHistory().Observing(
            Inventory(Peer("p_mgmt", role: PeerRole.ManagementCompanion)),
            T0);

        var view = ControllerInventory.Build(
            Inventory(Peer("p_mgmt", role: PeerRole.Unknown)),
            history);

        Assert.Single(view.Companion);
        Assert.Empty(view.Paired);
    }

    [Fact]
    public void ASecondManagementClientIsNotAPairedController()
    {
        // The firmware-side half of the same rule, and the case local history
        // cannot cover: ANOTHER front end's management bond, which this PC has
        // never seen live and therefore remembers nothing about.
        //
        // Role used to be live evidence only, so that bond reported `unknown`
        // and arrived here bonded and disconnected -- indistinguishable from a
        // paired controller. The adapter now remembers management membership
        // durably and reports role=management for it even while it is offline,
        // which is what keeps it out of the controller list with no local
        // history at all.
        var view = ControllerInventory.Build(
            Inventory(Peer("p_other_pc",
                           role: PeerRole.ManagementCompanion,
                           connected: false)),
            new AdapterPeerHistory());

        Assert.Single(view.Companion);
        Assert.Empty(view.Paired);
        Assert.Empty(view.Connected);
    }

    [Fact]
    public void ALiveNameAlwaysBeatsARememberedOne()
    {
        var history = new AdapterPeerHistory().Observing(
            Inventory(Peer("p_1", name: "Old name")),
            T0);

        var view = ControllerInventory.Build(
            Inventory(Peer("p_1", classification: "Sony DualSense")),
            history);

        var listing = Assert.Single(view.Paired);
        Assert.Equal("Sony DualSense", listing.DisplayName);
        Assert.False(listing.IdentifiedFromHistory);
    }

    [Fact]
    public void ARememberedNameIsUsedOnlyWhenTheAdapterOffersNothingAndIsAttributed()
    {
        var history = new AdapterPeerHistory().Observing(
            Inventory(Peer("p_1", name: "DualSense Wireless Controller")),
            T0);

        var view = ControllerInventory.Build(
            Inventory(Peer("p_1", role: PeerRole.Unknown)),
            history);

        var listing = Assert.Single(view.Paired);
        Assert.Equal("DualSense Wireless Controller", listing.DisplayName);

        // Presentation MUST attribute it; a remembered identity shown as a live one
        // is exactly the promotion the protocol forbids.
        Assert.True(listing.IdentifiedFromHistory);
        Assert.Equal(PeerRole.Unknown, listing.Role);
        Assert.Equal(PeerRole.PhysicalController, listing.RememberedRole);
    }

    [Fact]
    public void RecentHoldsOnlyPeersTheAdapterNoLongerKnows()
    {
        var history = new AdapterPeerHistory()
            .Observing(Inventory(Peer("p_gone", name: "Old pad"), Peer("p_here", name: "Current")), T0)
            .Observing(Inventory(Peer("p_here", name: "Current")), T0 + 1);

        var view = ControllerInventory.Build(Inventory(Peer("p_here", name: "Current")), history);

        Assert.Equal("p_gone", Assert.Single(view.Recent).PeerId);
        Assert.True(Assert.Single(view.Recent).HistoryOnly);
        Assert.Equal("p_here", Assert.Single(view.Paired).PeerId);
    }

    [Fact]
    public void APcThisAppOnceManagedTheAdapterWithNeverAppearsUnderRecent()
    {
        // It is not a controller the user forgot; it would read as one.
        var history = new AdapterPeerHistory()
            .Observing(Inventory(Peer("p_mgmt", role: PeerRole.ManagementCompanion)), T0)
            .Observing(Inventory(), T0 + 1);

        var view = ControllerInventory.Build(Inventory(), history);
        Assert.Empty(view.Recent);
        Assert.True(view.IsEmpty);
    }

    [Fact]
    public void ARecentRowCarriesNoLiveClaimAtAll()
    {
        var history = new AdapterPeerHistory()
            .Observing(Inventory(Peer("p_gone", name: "Old pad")), T0)
            .Observing(Inventory(), T0 + 1);

        var listing = Assert.Single(ControllerInventory.Build(Inventory(), history).Recent);

        // The adapter has no opinion about a peer it no longer stores.
        Assert.Equal(PeerRole.Unknown, listing.Role);
        Assert.False(listing.Bonded);
        Assert.False(listing.Connected);
        Assert.True(listing.IdentifiedFromHistory);
    }

    [Fact]
    public void AnEmptyAdapterWithNoHistoryIsEmptyRatherThanMisleading()
    {
        var view = ControllerInventory.Build(Inventory(), new AdapterPeerHistory());
        Assert.True(view.IsEmpty);
        Assert.False(view.HasControllers);
        Assert.False(view.HasDiagnosticPeers);
    }
}
