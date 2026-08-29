using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The stale-bond path, end to end through the recovery ladder.
///
/// These exist because an audit before hardware testing found that
/// <see cref="AdapterResetSignature"/> could never fire in production, for three
/// independent reasons — and every one of them would have looked like "the
/// hypothesis was wrong" if it had been discovered on the bench instead:
///
/// 1. thrown WinRT failures were not wrapped, so the <c>HRESULT</c> half of the
///    signature had nothing to inspect;
/// 2. the "Windows still paired" fact was read off the live connection, which is
///    already disposed by the time a failure is classified;
/// 3. the "peer answered" fact was set only on the scan path and cleared by
///    teardown — and a remembered adapter connects directly, without a scan.
///
/// The tests below pin the corrected wiring. They do NOT establish that the
/// condition set is the right one; only hardware can do that.
/// </summary>
public sealed class StaleBondLadderTests
{
    private const string Info = """{"id":"picoswitch","version":"2.0"}""";

    [Fact]
    public async Task AConclusiveBondMismatchStopsTheLadderImmediately()
    {
        // Neither a clean retry nor an address-restricted fallback scan can succeed
        // against an adapter that has no key for us. Running them spends the
        // deadline and buries the one diagnosis that leads somewhere.
        var transport = new TrustingTransport
        {
            WindowsPaired = true,
            PeerReachable = true,
            FailDirectConnect = new GattTransportException(
                "refused",
                GattFailureStage.Services,
                hresult: GattStatusFormatter.EBluetoothAttInsufficientAuthentication),
        };

        var repository = new AdapterRepository(transport);
        var error = await Assert.ThrowsAsync<GattTransportException>(
            () => repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01"));

        Assert.Equal(1, transport.DirectConnects);
        Assert.Null(transport.ScannedAddress);
        Assert.True(AdapterResetSignature.IsBondMismatch(error, true, true));
    }

    [Fact]
    public async Task AnUnreachableAdapterStillGetsTheFullLadder()
    {
        // The opposite case, so the shortcut above cannot quietly become "give up
        // on the first failure".
        var transport = new TrustingTransport
        {
            WindowsPaired = true,
            PeerReachable = false,
            FailDirectConnect = new GattTransportException(
                "unreachable",
                GattFailureStage.Connect,
                GattCommunicationOutcome.Unreachable),
        };
        transport.Replies["info"] = Info;

        var repository = new AdapterRepository(transport);
        await repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01");

        Assert.Equal(2, transport.DirectConnects);
        Assert.Equal("AA:BB:CC:DD:EE:01", transport.ScannedAddress);
    }

    [Fact]
    public async Task AnUnpairedAdapterIsNotABondMismatchHoweverItFails()
    {
        // Without a held pairing this is simply "not paired", which is a different
        // message and a different flow. It must keep its ordinary ladder.
        var transport = new TrustingTransport
        {
            WindowsPaired = false,
            PeerReachable = true,
            FailDirectConnect = new GattTransportException(
                "refused",
                GattFailureStage.Services,
                GattCommunicationOutcome.AccessDenied),
        };
        transport.Replies["info"] = Info;

        var repository = new AdapterRepository(transport);
        await repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01");

        Assert.NotNull(transport.ScannedAddress);
    }

    [Theory]
    [InlineData(GattFailureStage.Services)]
    [InlineData(GattFailureStage.Subscribe)]
    [InlineData(GattFailureStage.Command)]
    public void TheSignatureIsRecognisedWhereverEncryptionIsFirstRequired(GattFailureStage stage)
    {
        // Android matches HCI 0x05/0x06 at the CONNECT stage. Windows exposes no
        // HCI to user mode, and the refusal surfaces wherever the stack first needs
        // the key — which may be service discovery, the CCC write, or the first
        // command. Pinning all three stops a future "tidy-up" from reintroducing
        // Android's stage restriction.
        var failure = new GattTransportException(
            "refused",
            stage,
            hresult: GattStatusFormatter.EBluetoothAttInsufficientAuthentication);

        Assert.True(AdapterResetSignature.IsBondMismatch(failure, true, true));
    }

    [Fact]
    public void TheSignatureIsFoundThroughTheLaddersAggregateReport()
    {
        // When both routes fail, the ladder reports them together and the
        // bond-mismatch evidence is in the DIRECT branch. Walking only
        // InnerException would inspect whichever branch happened to be first.
        var direct = new GattTransportException(
            "refused",
            GattFailureStage.Subscribe,
            GattCommunicationOutcome.AccessDenied);
        var aggregate = new AggregateException(
            "both routes failed",
            new ManagementException("nothing advertised"),
            direct);

        Assert.True(AdapterResetSignature.IsBondMismatch(aggregate, true, true));
    }

    [Fact]
    public async Task TheConnectionServiceTurnsTheSignatureIntoRepairRequiredOnTheFirstAttempt()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");

        // Now the adapter has been reflashed: Windows still holds a pairing, the
        // adapter is present, and it refuses encrypted access.
        await fixture.ReflashAsync(GattFailureStage.Services);

        await Assert.ThrowsAnyAsync<Exception>(
            () => fixture.Service.ConnectAsync(fixture.Id));

        Assert.Equal(
            AdapterRelationshipPhase.RepairRequired,
            fixture.Service.Relationship.Value.Phase);
        Assert.Equal(
            AdapterResetSignature.RepairMessage,
            fixture.Service.Relationship.Value.Message);

        // One attempt. No retry, no fallback scan.
        Assert.Equal(1, fixture.Transport.DirectConnects);
        Assert.Null(fixture.Transport.ScannedAddress);

        // And the row is flagged so the list can offer Repair, without losing the
        // relationship.
        var record = fixture.Service.Registry.Value.Record(fixture.Id);
        Assert.NotNull(record);
        Assert.True(record!.RepairRequired);
    }

    [Fact]
    public async Task RepairClearsTheFlagAndRetainsTheRowItsAliasAndItsHistory()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");
        await fixture.Service.RenameAsync(fixture.Id, "Living room");

        await fixture.ReflashAsync(GattFailureStage.Services);
        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));
        Assert.True(fixture.Service.Registry.Value.Record(fixture.Id)!.RepairRequired);

        await fixture.Service.RepairAsync(fixture.Id);

        var record = fixture.Service.Registry.Value.Record(fixture.Id);
        Assert.NotNull(record);
        Assert.False(record!.RepairRequired);

        // Repair replaces the Windows-side trust ONLY. The user's alias and the
        // adapter's identity survive it.
        Assert.Equal("Living room", record.UserAlias);
        Assert.Equal("AA:BB:CC:DD:EE:01", record.Address);
    }

    [Fact]
    public async Task RepairIsNeverAutomatic()
    {
        // A bond mismatch flags the row and reports the message. It must NOT unpair
        // anything: unpairing destroys a trust relationship and is an explicit user
        // action, every time.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");

        await fixture.ReflashAsync(GattFailureStage.Subscribe);

        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        // The row still carries the Windows device path it had; nothing was
        // unpaired behind the user's back.
        Assert.Equal(
            AdapterRelationshipPhase.RepairRequired,
            fixture.Service.Relationship.Value.Phase);
    }
}

/// <summary>A transport whose trust facts are scriptable, for the ladder tests.</summary>
public sealed class TrustingTransport : FakeTransportBase
{
    public bool WindowsPaired { get; set; }

    public bool PeerReachable { get; set; }

    public override TransportTrustSnapshot Trust => new(WindowsPaired, PeerReachable);
}
