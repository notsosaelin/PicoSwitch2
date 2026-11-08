using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Linux.Bluetooth;
using PicoSwitch.Companion.Platform.Bluetooth;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Linux.Tests;

/// <summary>
/// The management transport's rules, against a scripted BlueZ.
///
/// These are the rules the Windows transport learned on hardware and the Linux
/// one must keep: identity verified before Connected, per-attempt trust evidence
/// that survives teardown, bond proven only by a completed subscribe, one-session
/// teardown, and a Controller Link plane that cannot outlive its carrier.
/// </summary>
public sealed class BluezManagementTransportTests
{
    private static readonly string Address = BluetoothAddressFormat.ToText(FakeBluezBus.PicoAddress)!;

    private static BluezManagementTransport Build(FakeBluezBus bus) =>
        new(bus, () => Task.FromResult(BluezRadio.Describe(bus.Adapter!))) { LeRefreshMillis = 100 };

    private static BluezAdvertisement Fresh() =>
        new(FakeBluezBus.DevicePath, FakeBluezBus.PicoAddress, "PicoSwitch2", -50);

    private static async Task<bool> WaitForAsync(Func<bool> condition)
    {
        for (var i = 0; i < 200 && !condition(); i++)
        {
            await Task.Delay(10);
        }

        return condition();
    }

    [Fact]
    public async Task AConnectedSessionAnswersCommandsThroughNotifications()
    {
        var bus = new FakeBluezBus();
        bus.Replies["info"] = """{"id":"picoswitch","version":"2.0"}""";
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1, PairingState = "paired" });

        await transport.ConnectKnownAsync(Address);
        var reply = await transport.TransactAsync("info");

        Assert.Equal("""{"id":"picoswitch","version":"2.0"}""", reply);

        // Framed with the shared contract: the command plus its newline, written
        // WITH response (the RX characteristic is a Write Request).
        Assert.Equal("info\n"u8.ToArray(), bus.RxWrites.SelectMany(chunk => chunk).ToArray());
        Assert.True(transport.Trust.BondProven);
        Assert.True(transport.Trust.WindowsPaired);
    }

    [Fact]
    public async Task ConnectedIsWithheldUntilTheIdentityIsVerified()
    {
        var bus = new FakeBluezBus();
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });

        await transport.ConnectKnownAsync(Address);

        // A subscribed link is not yet a trusted session: the repository promotes
        // it only after the management identity reply is checked.
        Assert.Equal(ConnectionPhase.Connecting, transport.Connection.Value.Phase);
        transport.MarkValidated();
        Assert.Equal(ConnectionPhase.Connected, transport.Connection.Value.Phase);
    }

    [Fact]
    public async Task CommandsAreChunkedToTheNegotiatedMtu()
    {
        var bus = new FakeBluezBus { Mtu = 23 };
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });
        await transport.ConnectKnownAsync(Address);

        var command = "amiibo chunk " + new string('a', 40);
        bus.Replies[command] = "ok";
        await transport.TransactAsync(command);

        Assert.All(bus.RxWrites, chunk => Assert.True(chunk.Length <= BleManagementContract.AttPayloadFor(23)));
        Assert.True(bus.RxWrites.Count > 1);
    }

    [Fact]
    public async Task ARemoteDisconnectRetiresTheSessionImmediately()
    {
        var bus = new FakeBluezBus();
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });
        await transport.ConnectKnownAsync(Address);
        transport.MarkValidated();

        var plane = transport.TryCreateDataPlane();
        Assert.NotNull(plane);
        string? closedReason = null;
        plane.Closed += reason => closedReason = reason;

        bus.DropLink();

        Assert.True(await WaitForAsync(() => transport.Connection.Value.Phase == ConnectionPhase.Idle));
        Assert.Null(transport.TryCreateDataPlane());

        // Controller Link cannot outlive its carrier.
        Assert.NotNull(closedReason);
        await Assert.ThrowsAsync<ManagementException>(() => transport.TransactAsync("info"));
    }

    [Fact]
    public async Task DisconnectReleasesTheLinkAndEverySubscription()
    {
        var bus = new FakeBluezBus();
        var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });
        await transport.ConnectKnownAsync(Address);

        await transport.DisconnectAsync();

        // The adapter has room for one management session; a link BlueZ keeps
        // up after the app let go would hold it.
        Assert.Equal(0, bus.ActiveSubscriptions);
        Assert.Equal(1, bus.Disconnects);
        Assert.Equal(ConnectionPhase.Idle, transport.Connection.Value.Phase);
        await transport.DisposeAsync();
    }

    [Fact]
    public async Task ALateNotificationFromARetiredSessionIsIgnored()
    {
        var bus = new FakeBluezBus();
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });
        await transport.ConnectKnownAsync(Address);
        var stale = bus;

        await transport.ConnectKnownAsync(Address);
        bus.Replies["info"] = "fresh";

        // Nothing raised on a retired subscription may complete the live reply.
        stale.RaiseLateTx("stale\n"u8.ToArray());
        Assert.Equal("fresh", await transport.TransactAsync("info"));
    }

    [Fact]
    public async Task AnUnreachableSubscribeCountsAsALinkFailureNotAnAnswer()
    {
        var bus = new FakeBluezBus
        {
            SubscribeFailure = new BluezException(BluezErrors.Failed, "Not connected"),
        };
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 7 });

        var error = await Assert.ThrowsAsync<GattTransportException>(() => transport.ConnectKnownAsync(Address));

        Assert.Equal(GattFailureStage.Subscribe, error.Stage);
        Assert.Equal(GattCommunicationOutcome.Unreachable, error.Outcome);
        Assert.Equal(1, transport.Trust.LinkFailuresAfterResolve);
        Assert.False(transport.Trust.PeerAnsweredGatt);
        Assert.False(transport.Trust.BondProven);
    }

    /// <summary>
    /// A stale bond, exactly as captured on hardware (Bazzite, BlueZ 5.87,
    /// 2026-11-06): Connect fails with a generic local abort, then BlueZ says
    /// why. Before this, the abort read as Unreachable and Pair retried the dead
    /// key forever with no Repair offer.
    /// </summary>
    [Fact]
    public async Task AnAuthenticationDisconnectDuringConnectIsABondMismatch()
    {
        var bus = new FakeBluezBus
        {
            ConnectFailure = new BluezException(BluezErrors.Failed, "le-connection-abort-by-local"),
            ConnectFailureReason = BluezErrors.AuthenticationReason,
        };
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1, PairingState = "paired" });

        var error = await Assert.ThrowsAsync<GattTransportException>(() => transport.ConnectKnownAsync(Address));

        Assert.Equal(GattFailureStage.Connect, error.Stage);
        Assert.Equal(GattCommunicationOutcome.AccessDenied, error.Outcome);
        Assert.True(AdapterResetSignature.IsBondMismatch(error, transport.Trust));
    }

    /// <summary>
    /// Dual-mode BlueZ against the LE-only management service, as captured
    /// 2026-11-06: Connect fails with a bare "Failed" because the Classic half
    /// was refused, while the LE link is up. The session must continue, and is
    /// still proven the normal way (subscribe, then identity).
    /// </summary>
    [Fact]
    public async Task ARefusedClassicHalfDoesNotFailAnUpLeLink()
    {
        var bus = new FakeBluezBus
        {
            ConnectFailure = new BluezException(BluezErrors.Failed, "Failed"),
            ConnectFailureLeavesLinkUp = true,
        };
        bus.Replies["info"] = """{"id":"picoswitch","version":"2.0"}""";
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1, PairingState = "paired" });

        await transport.ConnectKnownAsync(Address);

        Assert.Equal("""{"id":"picoswitch","version":"2.0"}""", await transport.TransactAsync("info"));
        Assert.True(transport.Trust.BondProven);
    }

    /// <summary>
    /// Dual-mode BlueZ picks the bearer it last saw the adapter on. A fresh LE
    /// advertisement just before Connect steers it to LE (Ally, Game Mode,
    /// 2026-11-08: every connect went to BR/EDR and failed br-connection-refused).
    /// </summary>
    [Fact]
    public async Task AFreshLeAdvertisementBeforeConnectSteersBlueZToLe()
    {
        var bus = new FakeBluezBus { LastSeenClassic = true };
        bus.Advertisements.Add(Fresh());
        bus.Replies["info"] = """{"id":"picoswitch","version":"2.0"}""";
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1, PairingState = "paired" });

        await transport.ConnectKnownAsync(Address);

        Assert.Equal(1, bus.Connects);
        Assert.Equal("""{"id":"picoswitch","version":"2.0"}""", await transport.TransactAsync("info"));
    }

    /// <summary>A cached or connected device reported by discovery (no RSSI) is not an LE sighting.</summary>
    [Fact]
    public async Task ACachedDeviceReportDoesNotCountAsAnLeSighting()
    {
        var bus = new FakeBluezBus { LastSeenClassic = true };
        bus.Advertisements.Add(new BluezAdvertisement(FakeBluezBus.DevicePath, FakeBluezBus.PicoAddress, "PicoSwitch2", null));
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1, PairingState = "paired" });

        var error = await Assert.ThrowsAsync<GattTransportException>(() => transport.ConnectKnownAsync(Address));

        Assert.Equal(GattFailureStage.Connect, error.Stage);
    }

    /// <summary>
    /// A Classic link reads Connected with ServicesResolved (SDP) but has no
    /// GATT. It must not be reused, nor excuse a failed Connect: it is dropped
    /// and LE is asked for (Ally, 2026-11-08: "services resolved; continuing",
    /// then "did not expose its management service").
    /// </summary>
    [Fact]
    public async Task AConnectedClassicLinkIsDroppedAndReplacedByLe()
    {
        var bus = new FakeBluezBus { Connected = true, ClassicLink = true, LastSeenClassic = true };
        bus.Advertisements.Add(Fresh());
        bus.Replies["info"] = """{"id":"picoswitch","version":"2.0"}""";
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1, PairingState = "paired" });

        await transport.ConnectKnownAsync(Address);

        Assert.Equal(1, bus.Disconnects);
        Assert.Equal(1, bus.Connects);
        Assert.Equal("""{"id":"picoswitch","version":"2.0"}""", await transport.TransactAsync("info"));
    }

    /// <summary>A live LE link with the management service is reused as it is.</summary>
    [Fact]
    public async Task AConnectedLeLinkIsReusedWithoutReconnecting()
    {
        var bus = new FakeBluezBus { Connected = true };
        bus.Replies["info"] = """{"id":"picoswitch","version":"2.0"}""";
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1, PairingState = "paired" });

        await transport.ConnectKnownAsync(Address);

        Assert.Equal(0, bus.Connects);
        Assert.Equal(0, bus.Disconnects);
    }

    /// <summary>The same abort with no reason (or BlueZ before 5.82) stays a link failure, never a Repair.</summary>
    [Fact]
    public async Task ALocalAbortWithoutAReasonIsNotABondMismatch()
    {
        var bus = new FakeBluezBus
        {
            ConnectFailure = new BluezException(BluezErrors.Failed, "le-connection-abort-by-local"),
        };
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1, PairingState = "paired" });

        var error = await Assert.ThrowsAsync<GattTransportException>(() => transport.ConnectKnownAsync(Address));

        Assert.Equal(GattCommunicationOutcome.Unreachable, error.Outcome);
        Assert.False(AdapterResetSignature.IsBondMismatch(error, transport.Trust));
    }

    [Fact]
    public async Task ASecurityRefusalIsAnAttributeLayerAccessDenied()
    {
        var bus = new FakeBluezBus
        {
            SubscribeFailure = new BluezException(BluezErrors.Failed, "Operation failed with ATT error: 0x05"),
        };
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });

        var error = await Assert.ThrowsAsync<GattTransportException>(() => transport.ConnectKnownAsync(Address));

        Assert.Equal(GattCommunicationOutcome.AccessDenied, error.Outcome);
        Assert.Equal((byte)0x05, error.ProtocolError);
        Assert.True(transport.Trust.PeerAnsweredGatt);
    }

    [Fact]
    public async Task TrustEvidenceSurvivesTeardownAndResetsPerLogicalAttempt()
    {
        var bus = new FakeBluezBus
        {
            SubscribeFailure = new BluezException(BluezErrors.NotConnected, "gone"),
        };
        await using var transport = Build(bus);

        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });
        await Assert.ThrowsAsync<GattTransportException>(() => transport.ConnectKnownAsync(Address));
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1, Retry = 1 });
        await Assert.ThrowsAsync<GattTransportException>(() => transport.ConnectKnownAsync(Address));

        // The caller reads this AFTER the attempt is torn down; two separately
        // opened links agreeing is the corroboration the signature needs.
        Assert.Equal(2, transport.Trust.LinkFailuresAfterResolve);

        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 2 });
        Assert.Equal(0, transport.Trust.LinkFailuresAfterResolve);
    }

    [Fact]
    public async Task ARestrictedScanNeverSubstitutesAnotherAdapter()
    {
        var bus = new FakeBluezBus { DeviceKnown = false };
        bus.Advertisements.Add(new BluezAdvertisement("/org/bluez/hci0/dev_11_22_33_44_55_66", 0x112233445566, "Other", -40));
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });

        // Only someone else's Pico is advertising: the scan must time out rather
        // than connect to it.
        using var cancel = new CancellationTokenSource(TimeSpan.FromMilliseconds(300));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => transport.ScanAndConnectAsync(Address, cancel.Token));
        Assert.False(transport.Trust.PeerObserved);
        Assert.Equal(0, bus.Connects);
    }

    [Fact]
    public async Task DiscoveryRecordsThatTheExactPeerWasObserved()
    {
        var bus = new FakeBluezBus { DeviceKnown = false };
        bus.Advertisements.Add(new BluezAdvertisement(FakeBluezBus.DevicePath, FakeBluezBus.PicoAddress, "PicoSwitch2", -50));
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });

        var peer = await transport.DiscoverAsync();

        Assert.Equal(Address, peer.Address);
        Assert.Equal((short)-50, peer.SignalStrengthDbm);
        Assert.True(transport.Trust.PeerObserved);
    }

    [Fact]
    public async Task AnUnknownDeviceIsReportedRatherThanConnectedBlindly()
    {
        var bus = new FakeBluezBus { DeviceKnown = false };
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });

        var error = await Assert.ThrowsAsync<GattTransportException>(() => transport.ConnectKnownAsync(Address));
        Assert.Equal(GattFailureStage.Connect, error.Stage);
        Assert.Equal(0, bus.Connects);
    }

    [Fact]
    public async Task RadioOffIsReportedBeforeAnyScan()
    {
        var bus = new FakeBluezBus();
        bus.Adapter = bus.Adapter! with { Powered = false };
        await using var transport = Build(bus);

        var error = await Assert.ThrowsAsync<ManagementException>(() => transport.DiscoverAsync());
        Assert.Equal("Bluetooth is turned off.", error.Message);
    }

    [Fact]
    public async Task TheDataPlaneWritesWithoutResponseAndDecodesFeedback()
    {
        var bus = new FakeBluezBus();
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });
        await transport.ConnectKnownAsync(Address);

        await using var plane = transport.TryCreateDataPlane()!;
        Assert.True(await plane.OpenAsync());
        Assert.Equal(247, plane.AttMtu);

        byte[]? feedback = null;
        plane.OutputFrameReceived += frame => feedback = frame;

        var frame = new byte[ControllerLinkDataPlane.FrameBytes];
        plane.PublishInput(frame);
        Assert.True(await WaitForAsync(() => plane.FramesWritten == 1));
        Assert.Single(bus.ControllerLinkWrites);

        bus.RaiseControllerLinkOutput([ControllerLinkDataPlane.Version, ControllerLinkDataPlane.OpcodeOutput, 0x02]);
        Assert.NotNull(feedback);
        Assert.Equal(1, plane.OutputFramesReceived);
    }

    [Fact]
    public async Task FirmwareWithoutTheDataPlaneIsAnUpdateNotAFailure()
    {
        var bus = new FakeBluezBus { HasControllerLink = false };
        await using var transport = Build(bus);
        transport.PrepareConnection(new ManagementConnectionContext { LogicalAttempt = 1 });
        await transport.ConnectKnownAsync(Address);

        await using var plane = transport.TryCreateDataPlane()!;
        Assert.False(await plane.OpenAsync());
    }
}
