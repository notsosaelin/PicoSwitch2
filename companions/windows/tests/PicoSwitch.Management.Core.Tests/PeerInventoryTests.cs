using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// The peer-inventory wire contract, from the client's side.
///
/// Two properties are worth more than the rest here. A peer list that silently
/// loses a row shows the user fewer saved controllers than the adapter actually
/// holds, and they will act on that. And a management or Controller Link
/// relationship rendered as a controller is a row the user would eventually try
/// to forget — which is their own PC.
/// </summary>
public sealed class PeerInventoryTests
{
    private static string Page(int? total = null, string next = "null", params string[] peers) =>
        "{\"v\":1,\"total\":" + (total ?? peers.Length) +
        ",\"peers\":[" + string.Join(",", peers) + "],\"next\":" + next + "}";

    private static string Peer(
        string id,
        string addr = "AABBCCDDEEFF",
        string role = "controller",
        int tr = 1,
        bool conn = false,
        string? name = null,
        string? classification = null,
        int? vid = null,
        int? pid = null)
    {
        var text = new System.Text.StringBuilder();
        text.Append("{\"id\":\"").Append(id)
            .Append("\",\"addr\":\"").Append(addr)
            .Append("\",\"tr\":").Append(tr)
            .Append(",\"role\":\"").Append(role)
            .Append("\",\"bonded\":true,\"conn\":").Append(conn ? "true" : "false");
        if (name is not null)
        {
            text.Append(",\"name\":\"").Append(name).Append('"');
        }

        if (classification is not null)
        {
            text.Append(",\"class\":\"").Append(classification).Append('"');
        }

        if (vid is not null && pid is not null)
        {
            text.Append(",\"vid\":").Append(vid).Append(",\"pid\":").Append(pid);
        }

        text.Append('}');
        return text.ToString();
    }

    /* ------------------------------------------------------------- parsing */

    [Fact]
    public void APageDecodesIdentityRoleTransportAndLiveState()
    {
        var decoded = ManagementProtocol.PeersPage(
            "peers list",
            Page(peers: [Peer("p_1", role: "controller", tr: 1, conn: true, name: "DualSense")]));

        var entry = Assert.Single(decoded.Entries);
        Assert.Equal("p_1", entry.Id);
        Assert.Equal(PeerRole.PhysicalController, entry.Role);
        Assert.Equal(PeerTransportSet.Of(PeerTransport.Classic), entry.Transports);
        Assert.True(entry.Connected);
        Assert.True(entry.Bonded);
        Assert.Equal("DualSense", entry.Name);
        Assert.Null(decoded.Next);
    }

    [Fact]
    public void APeerWithBothKeyRecordsIsOneRowOnTwoTransports()
    {
        // The management client's normal shape once Controller Link has run.
        var decoded = ManagementProtocol.PeersPage("peers list", Page(peers: [Peer("p_1", tr: 3)]));
        var entry = Assert.Single(decoded.Entries);
        Assert.Equal(PeerTransportSet.Of(PeerTransport.Classic, PeerTransport.Le), entry.Transports);
        Assert.True(entry.MultiTransport);
    }

    [Fact]
    public void AnUnrecognisedRoleIsUnknownRatherThanAParseFailure()
    {
        // A newer adapter is allowed to know roles this build does not. Refusing
        // the page would hide every peer on it.
        var decoded = ManagementProtocol.PeersPage(
            "peers list",
            Page(peers: [Peer("p_1", role: "headset")]));
        Assert.Equal(PeerRole.Unknown, Assert.Single(decoded.Entries).Role);
    }

    [Fact]
    public void AnUnknownTransportBitIsDroppedRatherThanHidingThePeer()
    {
        var decoded = ManagementProtocol.PeersPage("peers list", Page(peers: [Peer("p_1", tr: 0x0B)]));
        var entry = Assert.Single(decoded.Entries);
        Assert.Equal(PeerTransportSet.Of(PeerTransport.Classic, PeerTransport.Le), entry.Transports);
    }

    [Fact]
    public void AnAbsentNameIsAbsentNotAnEmptyString()
    {
        var decoded = ManagementProtocol.PeersPage("peers list", Page(peers: [Peer("p_1")]));
        Assert.Null(Assert.Single(decoded.Entries).Name);
    }

    [Fact]
    public void APeerWithNoIdIsRejected()
    {
        // The id is the app's only stable handle; a row without one cannot be
        // acted on later and must not be silently displayed as if it could.
        AssertShapeRejected("""{"v":1,"total":1,"peers":[{"addr":"AA","tr":1}],"next":null}""");
        AssertShapeRejected("""{"v":1,"total":1,"peers":[{"id":"","addr":"AA","tr":1}],"next":null}""");
    }

    [Fact]
    public void ARepeatedIdInsideOnePageIsRejected() =>
        AssertShapeRejected(Page(total: 2, peers: [Peer("p_1"), Peer("p_1")]));

    [Fact]
    public void AWrongEnvelopeVersionIsRejected() =>
        AssertShapeRejected("""{"v":2,"total":0,"peers":[],"next":null}""");

    [Fact]
    public void AMissingNextCursorIsRejected() =>
        AssertShapeRejected("""{"v":1,"total":0,"peers":[]}""");

    [Fact]
    public void APageClaimingMoreEntriesThanTheTotalIsRejected() =>
        AssertShapeRejected(Page(total: 1, peers: [Peer("p_1"), Peer("p_2")]));

    [Fact]
    public void AnEmptyPageWithANonNullCursorIsRejected() =>
        // It would make a client loop on the same cursor forever.
        AssertShapeRejected("""{"v":1,"total":3,"peers":[],"next":1}""");

    [Fact]
    public void AnEmptyInventoryIsAValidCompletePage()
    {
        var decoded = ManagementProtocol.PeersPage(
            "peers list",
            """{"v":1,"total":0,"peers":[],"next":null}""");
        Assert.Empty(decoded.Entries);
        Assert.Equal(0, decoded.Total);
        Assert.Null(decoded.Next);
    }

    private static void AssertShapeRejected(string reply)
    {
        var error = Record.Exception(() => ManagementProtocol.PeersPage("peers list", reply));
        Assert.True(
            error is ManagementException,
            $"expected a protocol rejection for {reply}, got {error?.GetType().Name ?? "nothing"}");
    }

    /* ---------------------------------------------------------- pagination */

    [Fact]
    public async Task PaginationFollowsTheCursorAndAssemblesEveryPeerOnce()
    {
        var channel = new ScriptedChannel(
            ("peers list", Page(total: 4, next: "2", peers: [Peer("p_1"), Peer("p_2")])),
            ("peers list 2", Page(total: 4, peers: [Peer("p_3"), Peer("p_4")])));

        var inventory = await new ManagementClient(channel).ListPeersAsync();
        Assert.Equal(["p_1", "p_2", "p_3", "p_4"], inventory.Peers.Select(peer => peer.Id).ToList());
        Assert.Equal(4, inventory.Total);
        Assert.True(inventory.Complete);
        channel.AssertDrained();
    }

    [Fact]
    public Task ATotalThatChangesMidPaginationIsAFailureNotAShorterList() =>
        AssertPaginationFailure(new ScriptedChannel(
            ("peers list", Page(total: 3, next: "1", peers: [Peer("p_1")])),
            ("peers list 1", Page(total: 2, peers: [Peer("p_2")]))));

    [Fact]
    public Task ANonProgressingCursorIsAFailure() =>
        AssertPaginationFailure(new ScriptedChannel(
            ("peers list", Page(total: 2, next: "0", peers: [Peer("p_1")]))));

    [Fact]
    public Task APeerRepeatedAcrossPagesIsAFailure() =>
        AssertPaginationFailure(new ScriptedChannel(
            ("peers list", Page(total: 2, next: "1", peers: [Peer("p_1")])),
            ("peers list 1", Page(total: 2, peers: [Peer("p_1")]))));

    [Fact]
    public Task AFinalCountShortOfTheDeclaredTotalIsAFailure() =>
        // The row that never arrived is a saved controller the user would
        // conclude is already gone.
        AssertPaginationFailure(new ScriptedChannel(
            ("peers list", Page(total: 2, peers: [Peer("p_1")]))));

    private static Task AssertPaginationFailure(ScriptedChannel channel) =>
        Assert.ThrowsAsync<ManagementPaginationException>(
            () => new ManagementClient(channel).ListPeersAsync());

    /* ----------------------------------------------------------- filtering */

    [Fact]
    public void ThisPcNeverAppearsInTheControllerList()
    {
        var inventory = new PeerInventory
        {
            Peers = new ValueList<PeerInfo>(
            [
                new PeerInfo("p_m", "AA", PeerRole.ManagementCompanion, PeerTransportSet.Of(PeerTransport.Le), Bonded: true),
                new PeerInfo("p_l", "BB", PeerRole.ControllerLink, PeerTransportSet.Of(PeerTransport.Classic), Bonded: true),
                new PeerInfo("p_c", "CC", PeerRole.PhysicalController, PeerTransportSet.Of(PeerTransport.Classic), Bonded: true),
                new PeerInfo("p_u", "DD", PeerRole.Unknown, PeerTransportSet.Of(PeerTransport.Classic), Bonded: true),
            ]),
            Complete = true,
            Total = 4,
        };

        // The gate: the management bond is not presented as a controller.
        Assert.Equal(["p_c"], inventory.Controllers.Select(peer => peer.Id).ToList());

        // Unknown is also kept out of the controller list. It is not a claim that
        // it isn't one -- it is a refusal to claim that it is.
        Assert.Equal(
            ["p_m", "p_l", "p_u"],
            inventory.CompanionsAndUnknown.Select(peer => peer.Id).ToList());
    }

    /* --------------------------------------------------------- classification */

    [Fact]
    public void AClassifiedPeerDecodesItsDerivedIdentity()
    {
        var entry = Assert.Single(ManagementProtocol.PeersPage(
            "peers list",
            Page(peers:
            [
                Peer("p_1", name: "Wireless Controller", classification: "Sony DualSense", vid: 1356, pid: 3302),
            ])).Entries);

        Assert.Equal("Sony DualSense", entry.Classification);
        Assert.Equal(1356, entry.VendorId);
        Assert.Equal(3302, entry.ProductId);
        Assert.True(entry.HasUsbIdentity);
    }

    [Fact]
    public void AnUnclassifiedPeerReportsNullRatherThanAnEmptyLabel()
    {
        var entry = Assert.Single(
            ManagementProtocol.PeersPage("peers list", Page(peers: [Peer("p_1")])).Entries);

        // Absent must stay distinguishable from blank: one means the adapter
        // cannot say, the other would render as a name made of nothing.
        Assert.Null(entry.Classification);
        Assert.False(entry.HasUsbIdentity);
    }

    [Fact]
    public void APageFromFirmwareThatPredatesClassificationStillDecodes()
    {
        // The envelope version did not move for the classification additions, so
        // an older adapter's page must remain readable rather than becoming a
        // version mismatch that hides every peer on it.
        var decoded = ManagementProtocol.PeersPage(
            "peers list",
            """{"v":1,"total":1,"peers":[{"id":"p_1","addr":"AABBCCDDEEFF","tr":1,"role":"controller","bonded":true,"conn":false}],"next":null}""");
        Assert.Single(decoded.Entries);
        Assert.Null(decoded.Entries[0].Classification);
    }

    /* ------------------------------------------------------------- naming */

    [Fact]
    public void AUserAliasOutranksEverythingTheAdapterCanSay() =>
        Assert.Equal(
            "Player two",
            PeerNaming.Label(
                address: "AABBCCDDEEFF",
                alias: "Player two",
                classification: "Sony DualSense",
                name: "Wireless Controller",
                vendorId: 1356,
                productId: 3302));

    [Fact]
    public void ADerivedClassificationOutranksTheNameTheDeviceClaims() =>
        // The remote name is whatever the device says it is and its owner can
        // change it; the classification is what this adapter worked out.
        Assert.Equal(
            "Sony DualSense",
            PeerNaming.Label(
                address: "AABBCCDDEEFF",
                classification: "Sony DualSense",
                name: "definitely not a controller"));

    [Fact]
    public void ADeviceWithOnlyAUsbIdentityIsNamedByItNotByItsAddress() =>
        Assert.Equal(
            "Device 054C:0CE6",
            PeerNaming.Label(address: "AABBCCDDEEFF", vendorId: 0x054C, productId: 0x0CE6));

    [Fact]
    public void APeerTheAdapterCannotNameAtAllFallsBackToAShortSuffix()
    {
        var label = PeerNaming.Label(address: "AABBCCDDEEFF");
        Assert.Equal("Controller • EEFF", label);

        // Never the bare address: an address rendered where a name belongs reads
        // as a name, and this one is not one.
        Assert.DoesNotContain("AABBCC", label);
    }

    [Fact]
    public void ABlankNameIsTreatedAsNoNameAtAll() =>
        Assert.Equal(
            "Controller • EEFF",
            PeerNaming.Label(address: "AABBCCDDEEFF", alias: "  ", name: ""));

    /* ------------------------------------------------------ selective forget */

    [Fact]
    public void AForgetReplyCarriesTheAdaptersVerifiedState()
    {
        var outcome = ManagementProtocol.PeersForget(
            "peers forget p_5E6F7A8B",
            """{"ok":true,"id":"p_5E6F7A8B","result":"removed","bonded":false,"tr":0}""");
        Assert.Equal("p_5E6F7A8B", outcome.PeerId);
        Assert.Equal(PeerForgetResult.Removed, outcome.Result);
        Assert.False(outcome.StillBonded);
        Assert.True(outcome.Transports.IsEmpty);
        Assert.True(outcome.Result.Succeeded());
    }

    [Fact]
    public void AlreadyAbsentIsASuccessNotAFailure()
    {
        // A management reply can be lost after the command has already run. A
        // retry must not tell the user the forget failed.
        var outcome = ManagementProtocol.PeersForget(
            "peers forget p_5E6F7A8B",
            """{"ok":true,"id":"p_5E6F7A8B","result":"already_absent","bonded":false,"tr":0}""");
        Assert.Equal(PeerForgetResult.AlreadyAbsent, outcome.Result);
        Assert.True(outcome.Result.Succeeded());
    }

    [Fact]
    public void ARefusedManagementPeerIsReportedAsSuchNotAsAGenericError()
    {
        var outcome = ManagementProtocol.PeersForget(
            "peers forget p_1A2B3C4D",
            """{"ok":false,"id":"p_1A2B3C4D","result":"management_peer","bonded":true,"tr":3}""");
        Assert.Equal(PeerForgetResult.ManagementPeer, outcome.Result);
        Assert.False(outcome.Result.Succeeded());
        Assert.True(outcome.StillBonded);
    }

    [Fact]
    public void APartialDeleteSurfacesAsIncompleteWithWhatRemains()
    {
        var outcome = ManagementProtocol.PeersForget(
            "peers forget p_5E6F7A8B",
            """{"ok":false,"id":"p_5E6F7A8B","result":"incomplete","bonded":true,"tr":1}""");
        Assert.Equal(PeerForgetResult.Incomplete, outcome.Result);
        Assert.False(outcome.Result.Succeeded());
        Assert.Equal(PeerTransportSet.Of(PeerTransport.Classic), outcome.Transports);
    }

    [Fact]
    public void AnUnrecognisedOutcomeStillYieldsTheVerifiedBondState()
    {
        // A newer adapter may name an outcome this build does not know. Rejecting
        // the reply would leave the client unable to say whether the delete
        // happened; `bonded` is the part that decides what is shown.
        var outcome = ManagementProtocol.PeersForget(
            "peers forget p_5E6F7A8B",
            """{"ok":false,"id":"p_5E6F7A8B","result":"deferred","bonded":true,"tr":2}""");
        Assert.Equal(PeerForgetResult.Unknown, outcome.Result);
        Assert.True(outcome.StillBonded);
        Assert.False(outcome.Result.Succeeded());
    }

    [Fact]
    public void AForgetReplyWithoutTheVerifiedStateIsRejected()
    {
        // Without `bonded` there is nothing to trust over the app's optimism.
        AssertForgetRejected("""{"ok":true,"id":"p_5E6F7A8B","result":"removed"}""");
        AssertForgetRejected("""{"ok":true,"result":"removed","bonded":false}""");
        AssertForgetRejected("""{"ok":true,"id":"","result":"removed","bonded":false}""");
    }

    [Fact]
    public void TheForgetCommandNamesThePeerByItsOpaqueId()
    {
        Assert.Equal("peers forget p_1A2B3C4D", ManagementCommands.PeersForget("p_1A2B3C4D"));

        // The id is the adapter's, not something the client may compose.
        foreach (var bad in new[] { "p_1a2b3c4d", "p_123", "1A2B3C4D", "", "p_1A2B3C4D " })
        {
            Assert.Throws<ArgumentException>(() => ManagementCommands.PeersForget(bad));
        }
    }

    private static void AssertForgetRejected(string reply)
    {
        var error = Record.Exception(
            () => ManagementProtocol.PeersForget("peers forget p_5E6F7A8B", reply));
        Assert.True(
            error is ManagementException,
            $"expected a protocol rejection for {reply}, got {error?.GetType().Name ?? "nothing"}");
    }

    [Fact]
    public void NoFieldOnAPeerCanCarryKeyMaterial()
    {
        // Structural: PeerInfo has no key field, so a reply cannot introduce one
        // without this test having to change.
        var members = typeof(PeerInfo)
            .GetProperties()
            .Select(property => property.Name.ToLowerInvariant())
            .ToList();
        Assert.DoesNotContain(
            members,
            name => name.Contains("key") || name.Contains("ltk") || name.Contains("irk"));
    }

    /* ----------------------------------------------------------- commands */

    [Fact]
    public void TheCommandSpellsItsCursorAsAPeerIndex()
    {
        Assert.Equal("peers list", ManagementCommands.PeersPage());
        Assert.Equal("peers list 0", ManagementCommands.PeersPage(0));
        Assert.Equal("peers list 7", ManagementCommands.PeersPage(7));
        Assert.Throws<ArgumentOutOfRangeException>(() => ManagementCommands.PeersPage(-1));
    }
}
