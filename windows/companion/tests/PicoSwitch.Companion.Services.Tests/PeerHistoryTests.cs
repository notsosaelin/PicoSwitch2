using PicoSwitch.Companion.Services;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// What an adapter has KNOWN, as opposed to what it has stored.
///
/// The rule under test throughout: history supplies a remembered label and a
/// remembered role ALONGSIDE the adapter's live answer, and never rewrites it.
/// The management protocol requires that <c>unknown</c> render as unidentified
/// and never be promoted to <c>controller</c>.
/// </summary>
public sealed class PeerHistoryTests
{
    private const long T0 = 1_000_000;

    private static PeerInfo Peer(
        string id,
        string address = "AA:BB:CC:DD:EE:FF",
        PeerRole role = PeerRole.PhysicalController,
        bool bonded = true,
        bool connected = false,
        string? name = null,
        string? classification = null,
        int vid = 0,
        int pid = 0,
        PeerTransport transport = PeerTransport.Classic) =>
        new(id, address, role, PeerTransportSet.Of(transport), bonded, connected, name, classification, vid, pid);

    private static PeerInventory Inventory(params PeerInfo[] peers) => new()
    {
        Peers = new ValueList<PeerInfo>(peers),
        Complete = true,
        Total = peers.Length,
    };

    [Fact]
    public void OnlyACompleteReadMayBeFoldedIntoHistory()
    {
        // A missing row would be indistinguishable from a peer the adapter has
        // forgotten, and history would then mark a live saved controller as
        // historical.
        var partial = new PeerInventory { Peers = new ValueList<PeerInfo>([Peer("p_1")]), Complete = false };
        Assert.Throws<ArgumentException>(() => new AdapterPeerHistory().Observing(partial, T0));
    }

    [Fact]
    public void AFirstReadRemembersWhatTheAdapterCouldSay()
    {
        var history = new AdapterPeerHistory().Observing(
            Inventory(Peer("p_1", name: "DualSense", classification: "Sony DualSense", vid: 1356, pid: 3302, connected: true)),
            T0);

        var record = Assert.Single(history.Records);
        Assert.Equal("Sony DualSense", record.Classification);
        Assert.Equal(PeerRole.PhysicalController, record.ProvenRole);
        Assert.Equal(T0, record.FirstSeenAtMillis);
        Assert.Equal(T0, record.LastConnectedAtMillis);
        Assert.True(record.Bonded);
    }

    [Fact]
    public void AnAdapterRebootMustNotErasetheRoleItOnceProved()
    {
        // Role is live evidence only: after a reboot a saved controller reads
        // `unknown` until it reconnects. Taking that as evidence it stopped being a
        // controller is what would offer to forget the user's own PC.
        var history = new AdapterPeerHistory()
            .Observing(Inventory(Peer("p_1", role: PeerRole.PhysicalController, name: "DualSense")), T0)
            .Observing(Inventory(Peer("p_1", role: PeerRole.Unknown)), T0 + 1000);

        var record = Assert.Single(history.Records);
        Assert.Equal(PeerRole.PhysicalController, record.ProvenRole);
        Assert.Equal("DualSense", record.LastKnownName);
    }

    [Fact]
    public void TransportsAccumulateAcrossReads()
    {
        // A peer seen only over LE this session still holds the Classic key the
        // adapter reported last session.
        var history = new AdapterPeerHistory()
            .Observing(Inventory(Peer("p_1", transport: PeerTransport.Classic)), T0)
            .Observing(Inventory(Peer("p_1", transport: PeerTransport.Le)), T0 + 1);

        var record = Assert.Single(history.Records);
        Assert.True(record.Transports.Contains(PeerTransport.Classic));
        Assert.True(record.Transports.Contains(PeerTransport.Le));
    }

    [Fact]
    public void APeerAbsentFromACompleteReadStopsBeingASavedPairing()
    {
        var history = new AdapterPeerHistory()
            .Observing(Inventory(Peer("p_1", name: "DualSense")), T0)
            .Observing(Inventory(), T0 + 1);

        var record = Assert.Single(history.Records);
        Assert.False(record.Bonded);

        // Kept, not deleted -- that is the whole point of history.
        Assert.Equal("DualSense", record.LastKnownName);
        Assert.Single(history.Forgotten);
    }

    [Fact]
    public void PruningNeverEvictsSomethingTheAdapterStillHoldsAKeyFor()
    {
        var history = new AdapterPeerHistory();

        // Fill history well past its cap with forgotten peers.
        for (var index = 0; index < AdapterPeerHistory.MaxRecords + 20; index++)
        {
            history = history.Observing(Inventory(Peer($"p_old{index}")), T0 + index);
        }

        // One bonded peer, plus every prior peer now absent.
        history = history.Observing(Inventory(Peer("p_live", bonded: true)), T0 + 10_000);

        Assert.True(history.Records.Count <= AdapterPeerHistory.MaxRecords);
        Assert.Contains(history.Records, record => record.PeerId == "p_live" && record.Bonded);
    }

    [Fact]
    public void TheStrongerRoleWinsAndIsNotDerivedFromEnumOrder()
    {
        Assert.Equal(
            PeerRole.ManagementCompanion,
            PeerRoleStrength.Stronger(PeerRole.ControllerLink, PeerRole.ManagementCompanion));
        Assert.Equal(
            PeerRole.ControllerLink,
            PeerRoleStrength.Stronger(PeerRole.PhysicalController, PeerRole.ControllerLink));
        Assert.Equal(
            PeerRole.PhysicalController,
            PeerRoleStrength.Stronger(PeerRole.Unknown, PeerRole.PhysicalController));
        Assert.Equal(
            PeerRole.PhysicalController,
            PeerRoleStrength.Stronger(PeerRole.PhysicalController, PeerRole.Unknown));
    }

    [Fact]
    public void HistoryIsPerAdapterAndDroppedWithIt()
    {
        var a = AdapterId.FromAddress("AA:BB:CC:DD:EE:01")!.Value;
        var b = AdapterId.FromAddress("AA:BB:CC:DD:EE:02")!.Value;
        var book = new PeerHistoryBook()
            .With(a, new AdapterPeerHistory().Observing(Inventory(Peer("p_1")), T0))
            .With(b, new AdapterPeerHistory().Observing(Inventory(Peer("p_2")), T0));

        Assert.Single(book.ForAdapter(a).Records);
        Assert.Single(book.ForAdapter(b).Records);

        book = book.Without(a);
        Assert.Empty(book.ForAdapter(a).Records);
        Assert.Single(book.ForAdapter(b).Records);
    }

    [Fact]
    public void NoFieldOnAHistoryRecordCanCarryKeyMaterial()
    {
        // Structural: a stored record has no key field, so a reply cannot introduce
        // one without this test having to change.
        var names = typeof(PeerHistoryRecord).GetProperties()
            .Select(property => property.Name.ToLowerInvariant())
            .ToList();
        Assert.DoesNotContain(
            names,
            name => name.Contains("key") || name.Contains("ltk") || name.Contains("irk"));
    }

    /* ------------------------------------------------------------- codec */

    [Fact]
    public void ADocumentRoundTripsEveryRememberedField()
    {
        var id = AdapterId.FromAddress("AA:BB:CC:DD:EE:01")!.Value;
        var book = new PeerHistoryBook().With(
            id,
            new AdapterPeerHistory().Observing(
                Inventory(Peer(
                    "p_1A2B3C4D",
                    role: PeerRole.PhysicalController,
                    name: "Wireless Controller",
                    classification: "Sony DualSense",
                    vid: 1356,
                    pid: 3302,
                    connected: true)),
                T0));

        var decoded = PeerHistoryCodec.Decode(PeerHistoryCodec.Encode(book));
        var record = Assert.Single(decoded.ForAdapter(id).Records);
        Assert.Equal("p_1A2B3C4D", record.PeerId);
        Assert.Equal("Sony DualSense", record.Classification);
        Assert.Equal(1356, record.VendorId);
        Assert.Equal(PeerRole.PhysicalController, record.ProvenRole);
        Assert.Equal(T0, record.LastConnectedAtMillis);
        Assert.True(record.Bonded);
    }

    [Fact]
    public void DecodingIsTotal()
    {
        Assert.Empty(PeerHistoryCodec.Decode(null).ByAdapter);
        Assert.Empty(PeerHistoryCodec.Decode("nonsense").ByAdapter);
        Assert.Empty(PeerHistoryCodec.Decode("""{"schema":99,"adapters":[]}""").ByAdapter);
    }

    [Fact]
    public void OneUnreadableRowDoesNotFailItsAdapterAndOneAdapterDoesNotFailTheDocument()
    {
        var decoded = PeerHistoryCodec.Decode(
            """
            {"schema":1,"adapters":[
              {"adapter":"not-an-address","peers":[]},
              {"adapter":"AA:BB:CC:DD:EE:01","peers":[{"nothing":true},{"id":"p_1","addr":"AA"}]}
            ]}
            """);

        var id = AdapterId.FromAddress("AA:BB:CC:DD:EE:01")!.Value;
        Assert.Equal("p_1", Assert.Single(decoded.ForAdapter(id).Records).PeerId);
    }

    [Fact]
    public void RemoteNamesAreReSanitizedOnRead()
    {
        // These strings originated as untrusted remote Bluetooth names, and they go
        // straight back into the UI and the diagnostic log.
        var decoded = PeerHistoryCodec.Decode(
            "{\"schema\":1,\"adapters\":[{\"adapter\":\"AA:BB:CC:DD:EE:01\"," +
            "\"peers\":[{\"id\":\"p_1\",\"addr\":\"AA\",\"name\":\"Evil\\nName\"}]}]}");

        var id = AdapterId.FromAddress("AA:BB:CC:DD:EE:01")!.Value;
        Assert.Equal("Evil Name", Assert.Single(decoded.ForAdapter(id).Records).LastKnownName);
    }
}
