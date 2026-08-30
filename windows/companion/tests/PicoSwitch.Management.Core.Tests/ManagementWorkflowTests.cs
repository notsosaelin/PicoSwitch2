using Xunit;

namespace PicoSwitch.Management.Tests;

public sealed class ManagementWorkflowTests
{
    [Fact]
    public async Task KbmPaginationAssemblesOneCompleteMapping()
    {
        var channel = new ScriptedChannel(
            ("kbm map kb 0", """{"profile":"kb","profileId":1,"cursor":0,"total":2,"bindings":[{"src":"key:04","dst":"a","custom":false}],"next":1}"""),
            ("kbm map kb 1", """{"profile":"kb","profileId":1,"cursor":1,"total":2,"bindings":[{"src":"key:05","dst":"b","custom":true}],"next":null}"""));

        var mapping = await new ManagementClient(channel).LoadKbmMappingAsync(KbmLayout.Keyboard);
        Assert.Equal(2, mapping.Bindings.Count);
        Assert.Equal(1, mapping.CustomCount);
        channel.AssertDrained();
    }

    [Fact]
    public async Task KbmPaginationRejectsAChangingTotal()
    {
        var channel = new ScriptedChannel(
            ("kbm map kb 0", """{"profile":"kb","profileId":1,"cursor":0,"total":2,"bindings":[{"src":"key:04","dst":"a","custom":false}],"next":1}"""),
            ("kbm map kb 1", """{"profile":"kb","profileId":1,"cursor":1,"total":3,"bindings":[{"src":"key:05","dst":"b","custom":false}],"next":null}"""));

        await Assert.ThrowsAsync<ManagementPaginationException>(
            () => new ManagementClient(channel).LoadKbmMappingAsync(KbmLayout.Keyboard));
    }

    [Fact]
    public async Task MutationUsesAuthoritativeKbmReadback()
    {
        var channel = new ScriptedChannel(
            ("kbm mode keyboard", """{"ok":true,"mode":"keyboard"}"""),
            ("kbm status", KbmStatusReply("keyboard", "keyboard", "kb")),
            ("kbm counters", KbmCountersReply()));

        var status = await new ManagementClient(channel).SetKbmModeAsync(KbmMode.Keyboard);
        Assert.Equal(KbmMode.Keyboard, status.Mode);
        channel.AssertDrained();
    }

    [Fact]
    public async Task BondPagingFollowsFirmwareCursorsAndChecksCompleteness()
    {
        var channel = new ScriptedChannel(
            ("bonds list", """{"error":"response_too_large","code":413}"""),
            ("bonds list v2", """{"v":2,"total":2,"bonds":[{"i":0,"type":0,"addr":"001122334455"}],"next":3}"""),
            ("bonds list v2 3", """{"v":2,"total":2,"bonds":[{"i":3,"type":1,"addr":"AABBCCDDEEFF"}],"next":null}"""));

        var bonds = await new ManagementClient(channel).ListBondsAsync();
        Assert.True(bonds.Complete);
        Assert.Equal([0, 3], bonds.Entries.Select(entry => entry.Index).ToList());
        channel.AssertDrained();
    }

    [Fact]
    public async Task LegacyBondReplyIsExplicitlyIncomplete()
    {
        var channel = new ScriptedChannel(
            ("bonds list", """{"bonds":[{"index":0,"address":"001122334455"}]}"""));

        var bonds = await new ManagementClient(channel).ListBondsAsync();
        Assert.False(bonds.Complete);
        Assert.Null(bonds.Total);
    }

    [Fact]
    public async Task WirelessSaveReportsQueuedNotDurable()
    {
        var channel = new ScriptedChannel(("save", """{"ok":true,"queued":true}"""));
        var acknowledgement = await new ManagementClient(channel).SaveAsync();
        Assert.Equal(PersistenceState.Queued, acknowledgement.State);
    }

    [Fact]
    public async Task ColorMutationSavesThenReadsAuthoritativeConfiguration()
    {
        var channel = new ScriptedChannel(
            ("body 1 2 3", """{"ok":true}"""),
            ("save", """{"ok":true,"queued":true}"""),
            ("get", """{"body_color":[1,2,3],"joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}"""));

        var (config, save) = await new ManagementClient(channel)
            .SetColorAsync(ColorTarget.Body, new RgbColor(1, 2, 3));
        Assert.Equal(new RgbColor(1, 2, 3), config.BodyColor);
        Assert.Equal(PersistenceState.Queued, save!.State);
    }

    [Fact]
    public async Task AmiiboUploadFailureCancelsTransaction()
    {
        var data = new byte[540];
        var channel = new ScriptedChannel(
            ("amiibo status", AmiiboStatusReply(loaded: false)),
            ("amiibo begin 540 CAB5ECE6", """{"ok":true}"""),
            ($"amiibo chunk 0 {new string('0', 64)}", """{"error":"bad chunk","code":4}"""),
            ("amiibo cancel", """{"ok":true}"""));

        await Assert.ThrowsAsync<AdapterCommandException>(
            () => new ManagementClient(channel).UploadAmiiboAsync(data));
        channel.AssertDrained();
    }

    /// <summary>
    /// The CRC in the command above is not a magic constant: it is what
    /// <c>java.util.zip.CRC32</c> produces for 540 zero bytes, which is what the
    /// Kotlin client sends. If the C# CRC ever diverged, the upload would be
    /// rejected by real firmware — so the vector is pinned directly rather than
    /// only implied by the scripted exchange.
    /// </summary>
    [Fact]
    public void Crc32MatchesTheJavaImplementationForTheUploadVector()
    {
        Assert.Equal("CAB5ECE6", Crc32.Hex(new byte[540]));
        Assert.Equal("00000000", Crc32.Hex([]));
        Assert.Equal("CBF43926", Crc32.Hex("123456789"u8));
    }

    [Fact]
    public async Task RefreshComposesAvailableAndUnsupportedCapabilitiesIndependently()
    {
        var channel = new ScriptedChannel(
            ("info", """{"id":"picoswitch","version":"2.0","bridge_contract":4}"""),
            ("get", """{"body_color":[0,0,0],"joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}"""),
            ("device", "{}"),
            ("personality", """{"current":"pro2","available":["pro2","gc"]}"""),
            ("amiibo status", """{"error":"unknown command"}"""),
            ("mgmt status", """{"enabled":true}"""),
            ("bonds list", """{"v":2,"total":0,"bonds":[],"next":null}"""),
            ("input sources", """{"active":0,"pending":0,"explicit":false,"fresh":false,"transitions":0,"sources":[],"more":false}"""),
            ("kbm status", KbmStatusReply("kbmouse", "auto", "kbm")),
            ("kbm counters", KbmCountersReply()),
            ("kbm mouse", KbmMouseReply()));

        var refresh = await new ManagementClient(channel).RefreshAllAsync();
        var capabilities = refresh.Snapshot.Capabilities;
        Assert.Equal(CapabilityState.Available, capabilities.Core);
        Assert.Equal(CapabilityState.Available, capabilities.Personality);
        Assert.Equal(CapabilityState.Unsupported, capabilities.Amiibo);
        Assert.Equal(CapabilityState.Available, capabilities.Bonds);
        Assert.Equal(CapabilityState.Available, capabilities.Kbm);

        // Peers, selective forget and remote pairing are NOT probed by a refresh:
        // they are three independent families the application layer probes
        // separately, and a refresh that guessed at them would be the thing
        // WINDOWS_PASS.md §13.2 rule 8 forbids.
        Assert.Equal(CapabilityState.Unknown, capabilities.Peers);
        Assert.Equal(CapabilityState.Unknown, capabilities.PeerForget);
        Assert.Equal(CapabilityState.Unknown, capabilities.RemotePairing);
        channel.AssertDrained();
    }

    [Fact]
    public async Task ThreePeerFamiliesProbeIndependently()
    {
        // peers list works, selective forget does not, pairing does. All three
        // combinations must be expressible; collapsing them would either hide a
        // working list or offer a button that answers `unknown command`.
        var channel = new ScriptedChannel(
            ("peers forget p_00000000", """{"error":"unknown command"}"""),
            ("pairing status", """{"ok":true,"op":0,"state":"idle"}"""));

        var client = new ManagementClient(channel);
        Assert.Equal(CapabilityState.Unsupported, await client.ProbePeerForgetAsync());
        Assert.Equal(CapabilityState.Available, await client.ProbeRemotePairingAsync());
        channel.AssertDrained();
    }

    [Fact]
    public async Task ReenumerationMustBeConfirmedByTheAdapter()
    {
        var confirmed = new ScriptedChannel(("reenumerate", """{"ok":true,"reenumerating":true}"""));
        await new ManagementClient(confirmed).ReenumerateUsbAsync();

        var silent = new ScriptedChannel(("reenumerate", """{"ok":true}"""));
        await Assert.ThrowsAsync<ManagementProtocolException>(
            () => new ManagementClient(silent).ReenumerateUsbAsync());
    }

    // Product state and counters are two replies because ONE reply carrying both
    // is 729 bytes worst case and the wireless slot is 512
    // (CONFIG_WIRELESS_RESPONSE_CAPACITY). A single oversized reply is refused
    // whole with `response_too_large`, which took the entire Keyboard & Mouse
    // page down rather than degrading -- see ns2_kbm_status.c.
    internal static string KbmStatusReply(string mode, string modeOverride, string profile) =>
        "{\"mode\":\"" + mode + "\",\"override\":\"" + modeOverride + "\",\"profile\":\"" + profile +
        "\",\"keyboard\":true,\"mouse\":false,\"nativeMouse\":false,\"keyboardConn\":1,\"mouseConn\":0}";

    internal static string KbmCountersReply() =>
        """{"keyboardReports":0,"mouseReports":0,"rejectedMode":0,"rejectedDuplicate":0,"rejectedNotOwner":0,"rejectedNoPeerKey":0,"rejectedUnclassified":0,"rejectedNoRole":0,"undecodedReports":0,"rollover":0,"roleLosses":0,"mapGeneration":0,"neutralizations":0,"publishes":0,"recenters":0}""";

    internal static string KbmMouseReply() =>
        """{"sensitivityX":512,"sensitivityY":512,"recenterMs":12,"invertX":false,"invertY":false,"antiDeadzone":0,"sensitivityMin":16,"sensitivityMax":8192,"recenterMinMs":1,"recenterMaxMs":100,"antiDeadzoneMax":50}""";

    internal static string AmiiboStatusReply(bool loaded) =>
        "{\"loaded\":" + (loaded ? "true" : "false") +
        ",\"dirty\":false,\"presented\":false,\"v3loaded\":false,\"persisted\":false," +
        "\"persistPending\":false,\"size\":0,\"signature\":false,\"hasSave2\":false," +
        "\"usingSave2\":false,\"generation\":0,\"payloadCrc\":\"00000000\",\"uid\":\"\"," +
        "\"figureId\":\"\",\"upload\":{\"active\":false,\"received\":0,\"size\":0}}";
}
