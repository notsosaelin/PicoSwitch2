using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The application-level operation surface, over a fake transport.
///
/// No radio required (WINDOWS_PASS.md §26.3). What is proven here is the ORDER
/// and the GATING: what the repository does before it will call an adapter
/// connected, what it does when a connect fails, and what it refuses to conclude
/// from a capability probe.
/// </summary>
public sealed class AdapterRepositoryTests
{
    private const string Info = """{"id":"picoswitch","version":"2.0","bridge_contract":4}""";
    private const string NotAPico = """{"id":"something-else","version":"1.0"}""";

    [Fact]
    public async Task ConnectedRequiresOneRealProtocolExchangeNotJustALink()
    {
        var transport = new FakeTransport();
        transport.Replies["info"] = Info;
        var repository = new AdapterRepository(transport);

        await repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01");

        Assert.True(transport.Validated);
        Assert.Equal("picoswitch", repository.Snapshot.Value.Firmware.Id);
        Assert.Equal(CapabilityState.Available, repository.Snapshot.Value.Capabilities.Core);
    }

    [Fact]
    public async Task ADeviceThatIsNotAPicoSwitch2IsDisconnectedAndNeverValidated()
    {
        var transport = new FakeTransport();
        transport.Replies["info"] = NotAPico;
        var repository = new AdapterRepository(transport);

        await Assert.ThrowsAsync<AdapterIdentityException>(
            () => repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01"));

        Assert.False(transport.Validated);
        Assert.True(transport.Disconnects > 0);
        Assert.Equal(string.Empty, repository.Snapshot.Value.Firmware.Id);

        // Terminal, not retried: the address was reached and something answered,
        // so a retry and a fallback scan can only waste the deadline. One connect
        // attempt, no scan.
        Assert.Equal(1, transport.DirectConnects);
        Assert.Null(transport.ScannedAddress);
    }

    [Fact]
    public async Task ASlowOptionalReplyCannotRejectAHealthyIdentityVerifiedCarrier()
    {
        // The lean validation boundary exists precisely so an optional
        // personality/controller probe cannot reject an adapter that already
        // proved what it is.
        var transport = new FakeTransport();
        transport.Replies["info"] = Info;
        transport.Failures["personality"] = new ManagementException("timed out");
        transport.Failures["device"] = new ManagementException("timed out");

        var repository = new AdapterRepository(transport);
        await repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01");

        Assert.True(transport.Validated);
        Assert.Equal(CapabilityState.Available, repository.Snapshot.Value.Capabilities.Core);
    }

    [Fact]
    public async Task PersonalityAndControllerAreReadOnceAfterValidation()
    {
        // Both are adapter truth and both used to be read only by the manual
        // Refresh button, so a freshly connected session showed "Acting as
        // Unknown" with no controller until the user pressed Refresh.
        var transport = new FakeTransport();
        transport.Replies["info"] = Info;
        transport.Replies["personality"] = """{"current":"pro2","available":["pro2","gc"]}""";
        transport.Replies["device"] = """{"name":"DualSense","vid":1356,"pid":3302}""";

        var repository = new AdapterRepository(transport);
        await repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01");

        Assert.Equal(Personality.Pro2, repository.Snapshot.Value.Personality.Current);
        Assert.Equal("DualSense", repository.Snapshot.Value.Controller.Name);
        Assert.Equal(["info", "personality", "device"], transport.Sent);
    }

    [Fact]
    public async Task TheFallbackScanIsRestrictedToTheSelectedAdaptersAddress()
    {
        // Discovering another valid Pico nearby is not permission to silently
        // replace the user's relationship.
        var transport = new FakeTransport
        {
            FailDirectConnect = new GattTransportException(
                "unreachable",
                GattFailureStage.Connect,
                GattCommunicationOutcome.Unreachable),
        };
        transport.Replies["info"] = Info;

        var repository = new AdapterRepository(transport);
        await repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01");

        Assert.Equal("AA:BB:CC:DD:EE:01", transport.ScannedAddress);
        Assert.NotNull(transport.ScannedAddress);
    }

    [Fact]
    public async Task BothFailuresAreReportedWhenTheFallbackAlsoFails()
    {
        var direct = new GattTransportException(
            "direct unreachable",
            GattFailureStage.Connect,
            GattCommunicationOutcome.Unreachable);
        var transport = new FakeTransport
        {
            FailDirectConnect = direct,
            FailScanConnect = new ManagementException("nothing advertised"),
        };

        var repository = new AdapterRepository(transport);
        var error = await Assert.ThrowsAsync<AggregateException>(
            () => repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01"));

        // A support bundle must show why the FIRST route failed, not only the last.
        Assert.Contains(error.InnerExceptions, inner => inner.Message.Contains("nothing advertised"));
        Assert.Contains(error.InnerExceptions, inner => ReferenceEquals(inner, direct));
    }

    [Fact]
    public async Task ANonRetryableFailureSkipsTheCleanRetryAndGoesStraightToTheFallback()
    {
        // A protocol error at the services stage is neither transient (so no clean
        // retry) nor the bond-mismatch shape (so the ladder is not short-circuited).
        // It goes straight to the one address-restricted fallback scan.
        //
        // AccessDenied deliberately does NOT belong here any more: while Windows is
        // paired and the peer answered, that IS the bond-mismatch shape and
        // StaleBondLadderTests owns it.
        var transport = new FakeTransport
        {
            FailDirectConnect = new GattTransportException(
                "malformed",
                GattFailureStage.Services,
                GattCommunicationOutcome.ProtocolError,
                protocolError: 0x0D),
        };
        transport.Replies["info"] = Info;

        var repository = new AdapterRepository(transport);
        await repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01");

        Assert.Equal(1, transport.DirectConnects);
        Assert.Equal("AA:BB:CC:DD:EE:01", transport.ScannedAddress);
    }

    [Fact]
    public async Task AnIncompletePeerReadIsAFailureRatherThanAShorterList()
    {
        var transport = new FakeTransport();
        transport.Replies["info"] = Info;
        transport.Replies["peers list"] = """{"v":1,"total":2,"peers":[{"id":"p_1","addr":"AA","tr":1}],"next":null}""";

        var repository = new AdapterRepository(transport);
        await Assert.ThrowsAsync<ManagementPaginationException>(
            () => repository.RefreshPeersAsync());

        // Nothing partial was published.
        Assert.False(repository.Snapshot.Value.Peers.Complete);
        Assert.Empty(repository.Snapshot.Value.Peers.Peers);
    }

    [Fact]
    public async Task AnAdapterWithoutPeerSupportDegradesRatherThanFailing()
    {
        var transport = new FakeTransport();
        transport.Replies["peers list"] = """{"error":"unknown command"}""";

        var repository = new AdapterRepository(transport);
        var inventory = await repository.RefreshPeersAsync();

        Assert.Empty(inventory.Peers);
        Assert.Equal(CapabilityState.Unsupported, repository.Snapshot.Value.Capabilities.Peers);
    }

    [Fact]
    public async Task TheInventoryIsReReadEvenWhenAForgetReportsAnError()
    {
        // The adapter is authoritative about what remains, and the observed
        // `bonded` field plus a fresh list outrank optimistic client state.
        var transport = new FakeTransport();
        transport.Replies["peers forget p_1A2B3C4D"] =
            """{"ok":false,"id":"p_1A2B3C4D","result":"incomplete","bonded":true,"tr":1}""";
        transport.Replies["peers list"] = """{"v":1,"total":0,"peers":[],"next":null}""";

        var repository = new AdapterRepository(transport);
        var outcome = await repository.ForgetPeerAsync("p_1A2B3C4D");

        Assert.Equal(PeerForgetResult.Incomplete, outcome.Result);
        Assert.True(outcome.StillBonded);
        Assert.Contains("peers list", transport.Sent);
    }

    [Fact]
    public async Task TheThreePeerFamiliesDegradeIndependently()
    {
        var transport = new FakeTransport();
        transport.Replies["peers list"] = """{"v":1,"total":0,"peers":[],"next":null}""";
        transport.Replies["peers forget p_00000000"] = """{"error":"unknown command"}""";
        transport.Failures["pairing status"] = new ManagementException("timed out");

        var repository = new AdapterRepository(transport);
        await repository.ProbeManagementCapabilitiesAsync();

        var capabilities = repository.Snapshot.Value.Capabilities;
        Assert.Equal(CapabilityState.Available, capabilities.Peers);
        Assert.Equal(CapabilityState.Unsupported, capabilities.PeerForget);

        // A transport failure leaves Unknown. "The probe did not establish an
        // answer" and "this firmware lacks the command" are different statements,
        // and only the second may disable a feature.
        Assert.Equal(CapabilityState.Unknown, capabilities.RemotePairing);
    }

    [Fact]
    public async Task DisconnectingClearsTheSnapshotSoTheNextSessionStartsFromAdapterTruth()
    {
        var transport = new FakeTransport();
        transport.Replies["info"] = Info;

        var repository = new AdapterRepository(transport);
        await repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01");
        Assert.Equal("picoswitch", repository.Snapshot.Value.Firmware.Id);

        await repository.DisconnectAsync();
        Assert.Equal(string.Empty, repository.Snapshot.Value.Firmware.Id);
        Assert.Equal(CapabilityState.Unknown, repository.Snapshot.Value.Capabilities.Core);
    }
}

public sealed class ManagementOwnerTests : IDisposable
{
    public ManagementOwnerTests() => ManagementOwner.ResetForTest();

    public void Dispose() => ManagementOwner.ResetForTest();

    [Fact]
    public void TheRepositoryIsCreatedAtMostOncePerProcess()
    {
        // The entire invariant. Two transports meant two GATT sessions to an
        // adapter that admits exactly one management client, and Disconnect
        // appearing to do nothing because the UI reported its own instance while
        // another kept the real session alive.
        var creations = 0;
        AdapterRepository Create()
        {
            creations++;
            return new AdapterRepository(new FakeTransport());
        }

        var first = ManagementOwner.Get(null, Create);
        var second = ManagementOwner.Get(new DiagnosticLog(), Create);

        Assert.Same(first, second);
        Assert.Equal(1, creations);
        Assert.True(ManagementOwner.HasRepository);
    }

    [Fact]
    public void TheFirstCallersDiagnosticsAreAdopted()
    {
        var log = new DiagnosticLog();
        ManagementOwner.Get(log, () => new AdapterRepository(new FakeTransport()));
        ManagementOwner.Get(new DiagnosticLog(), () => new AdapterRepository(new FakeTransport()));
        Assert.Same(log, ManagementOwner.Diagnostics);
    }

    [Fact]
    public void ReleasingTheSessionDisconnectsWithoutDisposingTheTransport()
    {
        // Disposal would retire the transport's lifecycle permanently, making the
        // singleton unusable for the rest of the process.
        var transport = new FakeTransport();
        ManagementOwner.Get(null, () => new AdapterRepository(transport));

        ManagementOwner.ReleaseSession();
        SpinWait.SpinUntil(() => transport.Disconnects > 0, TimeSpan.FromSeconds(2));

        Assert.True(transport.Disconnects > 0);
        Assert.Equal(0, transport.Disposals);
    }
}

/// <summary>
/// A management transport with no radio.
///
/// Scripted by COMMAND rather than by sequence, because most of these tests care
/// which commands were sent and in what order, not about a fixed conversation.
/// </summary>
public sealed class FakeTransport : FakeTransportBase
{
    public override TransportTrustSnapshot Trust => new(WindowsPaired: true, PeerReachable: true);
}

public abstract class FakeTransportBase : IManagementTransport
{
    private readonly StateValue<ConnectionState> connection = new(new ConnectionState());

    public Dictionary<string, string> Replies { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, Exception> Failures { get; } = new(StringComparer.Ordinal);

    public List<string> Sent { get; } = [];

    // Settable rather than init-only: a stale-bond test changes the adapter's
    // behaviour MID-FIXTURE, which is exactly the situation being modelled -- the
    // adapter worked, then it was reflashed.
    public Exception? FailDirectConnect { get; set; }

    public Exception? FailScanConnect { get; set; }

    public int DirectConnects { get; private set; }

    public int Disconnects { get; private set; }

    public int Disposals { get; private set; }

    public string? ScannedAddress { get; private set; }

    public bool Validated { get; private set; }

    public ManagementConnectionContext? LastContext { get; private set; }

    public IReadOnlyStateValue<ConnectionState> Connection => connection;

    public abstract TransportTrustSnapshot Trust { get; }

    public void PrepareConnection(ManagementConnectionContext context) => LastContext = context;

    public Task<DiscoveredManagementPeer> DiscoverAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(new DiscoveredManagementPeer(0xAABBCCDDEE01, "AA:BB:CC:DD:EE:01"));

    public Task ScanAndConnectAsync(
        string? expectedAddress = null,
        CancellationToken cancellationToken = default)
    {
        ScannedAddress = expectedAddress;
        return FailScanConnect is { } failure ? Task.FromException(failure) : Task.CompletedTask;
    }

    public Task ConnectKnownAsync(string address, CancellationToken cancellationToken = default)
    {
        DirectConnects++;
        return FailDirectConnect is { } failure ? Task.FromException(failure) : Task.CompletedTask;
    }

    public Task DisconnectAsync()
    {
        Disconnects++;
        Validated = false;
        return Task.CompletedTask;
    }

    public void MarkValidated() => Validated = true;

    /// <summary>
    /// Forget what happened during fixture setup.
    ///
    /// A test that asserts "exactly one connect attempt" means one attempt in the
    /// scenario, not one since the object was constructed -- and seeding a
    /// remembered adapter necessarily connects once.
    /// </summary>
    public void ResetCounters()
    {
        DirectConnects = 0;
        Disconnects = 0;
        ScannedAddress = null;
        Sent.Clear();
    }

    public Task<string> TransactAsync(
        string command,
        long timeoutMillis = ManagementChannel.DefaultTimeoutMillis,
        CancellationToken cancellationToken = default)
    {
        Sent.Add(command);
        if (Failures.TryGetValue(command, out var failure))
        {
            return Task.FromException<string>(failure);
        }

        return Replies.TryGetValue(command, out var reply)
            ? Task.FromResult(reply)
            : Task.FromException<string>(
                new ManagementException($"No scripted reply for '{command}'"));
    }

    public ValueTask DisposeAsync()
    {
        Disposals++;
        return ValueTask.CompletedTask;
    }
}
