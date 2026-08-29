using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The stale-bond path, end to end through the recovery ladder.
///
/// ## What hardware changed here
///
/// The first version of these tests pinned a condition set that was reasoned from
/// the WinRT API surface: Windows still paired, the peer reachable, and an
/// <c>AccessDenied</c> or authentication <c>HRESULT</c> from an encrypted
/// attribute. On 2026-08-29 an adapter was reflashed with the Windows pairing left
/// in place, and Windows produced none of that. It produced
/// <c>services / Unreachable</c>, with no ATT byte and no <c>HRESULT</c>, four
/// times, and the classification correctly refused to fire — on a condition set
/// that could not describe what was happening.
///
/// The corrected signature recognises two shapes. These tests exercise both
/// THROUGH THE LADDER, because the ladder is where the difference shows: the
/// attribute-layer shape is conclusive at the first failure and stops there, and
/// the link-layer shape needs the fallback to run, because the fallback is what
/// produces the second independent observation.
/// </summary>
public sealed class StaleBondLadderTests
{
    private const string Info = """{"id":"picoswitch","version":"2.0"}""";

    [Fact]
    public async Task AConclusiveAttributeLayerRefusalStopsTheLadderImmediately()
    {
        // Neither a clean retry nor an address-restricted fallback scan can succeed
        // against an adapter that has no key for us, and an explicit AccessDenied
        // needs no corroboration. Running them spends the deadline and buries the
        // one diagnosis that leads somewhere.
        var transport = new TrustingTransport
        {
            WindowsPaired = true,
            PeerObserved = true,
            PeerAnsweredGatt = true,
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
        Assert.True(AdapterResetSignature.IsBondMismatch(error, transport.Trust));
    }

    [Fact]
    public async Task TheLinkLayerRefusalRunsTheFallbackBecauseTheFallbackIsTheEvidence()
    {
        // This is the shape the hardware produces. After ONE Unreachable there is
        // no way to tell a reflashed adapter from a momentary link failure, so
        // short-circuiting here would be guessing. The address-restricted fallback
        // resolves the device again, fails the same way, and THAT is what makes it
        // a diagnosis.
        var transport = new TrustingTransport
        {
            WindowsPaired = true,
            PeerObserved = true,
            FailDirectConnect = new GattTransportException(
                "unreachable",
                GattFailureStage.Services,
                GattCommunicationOutcome.Unreachable),
            FailScanConnect = new GattTransportException(
                "unreachable",
                GattFailureStage.Services,
                GattCommunicationOutcome.Unreachable),
        };

        var repository = new AdapterRepository(transport);
        var error = await Assert.ThrowsAsync<AggregateException>(
            () => repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01"));

        // The fallback ran, restricted to this address.
        Assert.Equal("AA:BB:CC:DD:EE:01", transport.ScannedAddress);

        // And only NOW does the evidence add up.
        Assert.Equal(2, transport.Trust.LinkFailuresAfterResolve);
        Assert.True(AdapterResetSignature.IsBondMismatch(error, transport.Trust));
    }

    [Fact]
    public async Task AnAdapterThatIsSimplyAbsentGetsTheFullLadderAndNoDiagnosis()
    {
        // The same status, from a switched-off adapter. Nothing was ever observed
        // advertising, so this must stay an ordinary connection failure however
        // many times it repeats.
        var transport = new TrustingTransport
        {
            WindowsPaired = true,
            PeerObserved = false,
            FailDirectConnect = new GattTransportException(
                "unreachable",
                GattFailureStage.Services,
                GattCommunicationOutcome.Unreachable),
            FailScanConnect = new ManagementException("nothing advertised"),
        };

        var repository = new AdapterRepository(transport);
        var error = await Assert.ThrowsAsync<AggregateException>(
            () => repository.ConnectKnownAsync("AA:BB:CC:DD:EE:01"));

        Assert.Equal("AA:BB:CC:DD:EE:01", transport.ScannedAddress);
        Assert.False(AdapterResetSignature.IsBondMismatch(error, transport.Trust));
    }

    [Fact]
    public async Task AConnectStageFailureStillGetsItsCleanRetry()
    {
        // The opposite case, so the shortcut above cannot quietly become "give up
        // on the first failure".
        var transport = new TrustingTransport
        {
            WindowsPaired = true,
            PeerObserved = false,
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
            PeerObserved = true,
            PeerAnsweredGatt = true,
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

        Assert.True(AdapterResetSignature.IsBondMismatch(
            aggregate,
            new TransportTrustSnapshot(true, PeerObserved: true, PeerAnsweredGatt: true)));
    }

    /* ---------------------------------------------------- through the service */

    [Fact]
    public async Task TheHardwareShapeReachesRepairRequiredThroughTheService()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");

        // Reflashed: Windows still holds a pairing, the adapter is still
        // advertising, and both routes to its GATT server return Unreachable.
        await fixture.ReflashAsync();

        await Assert.ThrowsAnyAsync<Exception>(
            () => fixture.Service.ConnectAsync(fixture.Id));

        Assert.Equal(
            AdapterRelationshipPhase.RepairRequired,
            fixture.Service.Relationship.Value.Phase);
        Assert.Equal(
            AdapterResetSignature.RepairMessage,
            fixture.Service.Relationship.Value.Message);

        // The row is flagged so the list can offer Repair, without losing the
        // relationship.
        var record = fixture.Service.Registry.Value.Record(fixture.Id);
        Assert.NotNull(record);
        Assert.True(record!.RepairRequired);
    }

    [Fact]
    public async Task TheAttributeShapeReachesItOnTheFirstAttemptWithNoFallbackAtAll()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");
        await fixture.ReflashWithAttributeRefusalAsync(GattFailureStage.Services);

        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        Assert.Equal(
            AdapterRelationshipPhase.RepairRequired,
            fixture.Service.Relationship.Value.Phase);

        // One attempt. No retry, no fallback scan.
        Assert.Equal(1, fixture.Transport.DirectConnects);
        Assert.Null(fixture.Transport.ScannedAddress);
    }

    [Fact]
    public async Task AnAbsentAdapterNeverReachesRepairRequiredThroughTheService()
    {
        // The negative that matters most at this level: offering to unpair a
        // working adapter because it happened to be switched off would destroy a
        // trust relationship to recover from a flat battery.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");

        await fixture.Service.DisconnectAsync();
        fixture.Transport.WindowsPaired = true;
        fixture.Transport.PeerObserved = false;
        fixture.Transport.FailDirectConnect = new GattTransportException(
            "unreachable",
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable);
        fixture.Transport.FailScanConnect = new ManagementException("nothing advertised");

        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        Assert.NotEqual(
            AdapterRelationshipPhase.RepairRequired,
            fixture.Service.Relationship.Value.Phase);
        Assert.False(fixture.Service.Registry.Value.Record(fixture.Id)!.RepairRequired);
    }

    [Fact]
    public async Task RepairIsNeverAutomatic()
    {
        // A bond mismatch flags the row and reports the message. It must NOT unpair
        // anything: unpairing destroys a trust relationship and is an explicit user
        // action, every time.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");
        await fixture.ReflashAsync();

        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        Assert.Equal(
            AdapterRelationshipPhase.RepairRequired,
            fixture.Service.Relationship.Value.Phase);
        Assert.Equal(0, fixture.Pairing.Unpairs);
    }

    [Fact]
    public async Task TheFailureDiagnosticNamesEveryPredicateThatFedTheDecision()
    {
        // The 2026-08-29 run had to be reconstructed from a log that reported only
        // two of the facts. The line must now carry the WinRT detail and the whole
        // classification, or the next hardware session costs another flash cycle
        // and answers nothing.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");
        await fixture.ReflashAsync();

        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        var line = Assert.Single(
            fixture.Diagnostics.Snapshot(),
            entry => entry.Source == "connect" &&
                entry.Message.StartsWith("failed", StringComparison.Ordinal));

        Assert.Contains("stage=services", line.Message);
        Assert.Contains("GattCommunicationStatus=Unreachable", line.Message);
        Assert.Contains("paired=True", line.Message);
        Assert.Contains("observed=True", line.Message);
        Assert.Contains("linkFailures=2/2", line.Message);
        Assert.Contains("BOND MISMATCH", line.Message);
    }
}

/// <summary>
/// A transport whose trust evidence is scriptable, for the ladder tests.
///
/// <see cref="LinkFailuresAfterResolve"/> is ACCUMULATED rather than set, exactly
/// as the real transport accumulates it: one increment per resolved device that
/// fails with <c>Unreachable</c> at or after service discovery. Scripting it as a
/// fixed number would let a ladder test pass without the fallback ever running,
/// which is the specific thing these tests exist to check.
/// </summary>
public sealed class TrustingTransport : FakeTransportBase
{
    public bool WindowsPaired { get; set; }

    public bool PeerObserved { get; set; }

    public bool PeerAnsweredGatt { get; set; }

    public int LinkFailuresAfterResolve { get; set; }

    public override TransportTrustSnapshot Trust =>
        new(WindowsPaired, PeerObserved, PeerAnsweredGatt, LinkFailuresAfterResolve);

    public override Task ConnectKnownAsync(string address, CancellationToken cancellationToken = default)
    {
        Count(FailDirectConnect);
        return base.ConnectKnownAsync(address, cancellationToken);
    }

    public override Task ScanAndConnectAsync(
        string? expectedAddress = null,
        CancellationToken cancellationToken = default)
    {
        Count(FailScanConnect);
        return base.ScanAndConnectAsync(expectedAddress, cancellationToken);
    }

    private void Count(Exception? failure)
    {
        if (failure is GattTransportException
            {
                Outcome: GattCommunicationOutcome.Unreachable,
            } tagged && tagged.Stage != GattFailureStage.Connect)
        {
            LinkFailuresAfterResolve += 1;
        }
    }
}
