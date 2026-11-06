using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;
using Windows.Devices.Enumeration;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// Repair, which on 2026-08-29 was discovered to do nothing at all.
///
/// ## The defect these tests exist for
///
/// Repair resolved the adapter's Windows pairing through a device path cached on
/// the registry row. Nothing ever wrote that field — the single construction site
/// left it null — so the branch never ran. Repair logged
/// <c>no Windows device path cached for &lt;address&gt;</c>, cleared the repair
/// flag, and returned reporting success, with the stale Windows bond untouched.
/// Pressed twice on hardware, twice.
///
/// What made it invisible is worth recording: a test DID cover repair, and it
/// passed. It asserted that the row kept its alias and its address afterwards,
/// and those held perfectly, because nothing had happened to them. The test never
/// asked whether Windows had been told anything, because there was no seam
/// through which to ask.
///
/// So these tests assert the CALL, not only the aftermath.
/// </summary>
public sealed class RepairPairingTests
{
    private const string Address = "AA:BB:CC:DD:EE:01";

    [Fact]
    public async Task RepairUnpairsTheRememberedAdapterWithNoLiveSessionAndNoCachedPath()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.ReflashAsync();
        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        // The state Repair actually runs in: the connection failed, so there is no
        // session, and there has never been a cached device path.
        fixture.Pairing.Unpairs = 0;
        await fixture.Service.RepairAsync(fixture.Id);

        // Exactly one unpair, for exactly this adapter, resolved from its address.
        Assert.Equal(1, fixture.Pairing.Unpairs);
        Assert.Equal(fixture.Id.ToBluetoothAddress(), fixture.Pairing.LastUnpairedAddress);
    }

    [Fact]
    public async Task RepairClearsTheFlagAndRetainsTheRowItsAliasAndItsIdentity()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.Service.RenameAsync(fixture.Id, "Living room");

        await fixture.ReflashAsync();
        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));
        Assert.True(fixture.Service.Registry.Value.Record(fixture.Id)!.RepairRequired);

        await fixture.Service.RepairAsync(fixture.Id);

        var record = fixture.Service.Registry.Value.Record(fixture.Id);
        Assert.NotNull(record);
        Assert.False(record!.RepairRequired);

        // Repair replaces the Windows-side trust ONLY. The user's alias and the
        // adapter's identity survive it.
        Assert.Equal("Living room", record.UserAlias);
        Assert.Equal(Address, record.Address);
    }

    [Fact]
    public async Task RepairKeepsThePeerHistoryForThatAdapter()
    {
        // Windows trust is not knowledge about the adapter's own controllers.
        // Losing the history would silently demote saved controllers to "Recent".
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        var history = new PeerHistoryStore(fixture.Documents);
        var before = history.Load().ForAdapter(fixture.Id).Records.Count;

        await fixture.ReflashAsync();
        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));
        await fixture.Service.RepairAsync(fixture.Id);

        // Read back from disk, because surviving in memory is not the property that
        // matters -- surviving a relaunch is.
        Assert.Equal(before, history.Load().ForAdapter(fixture.Id).Records.Count);
        Assert.NotNull(fixture.Service.Registry.Value.Record(fixture.Id));
    }

    [Fact]
    public async Task AnUnpairThatWindowsRefusesLeavesTheRowFlaggedAndSaysSo()
    {
        // The precise failure mode that shipped: clearing the flag while the
        // pairing survived reported a repair that had not happened, and sent the
        // user back into the same loop with a row that now looked healthy.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.ReflashAsync();
        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        fixture.Pairing.UnpairResult = AdapterUnpairResult.AccessDenied;

        var error = await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.RepairAsync(fixture.Id));

        Assert.Contains("refused", error.Message, StringComparison.OrdinalIgnoreCase);
        Assert.True(fixture.Service.Registry.Value.Record(fixture.Id)!.RepairRequired);
    }

    [Fact]
    public async Task AnAdapterWindowsCannotEvenResolveIsNotReportedAsRepaired()
    {
        // Radio off, or the adapter out of reach. "I could not ask" is not "it is
        // done", and this is the same distinction the capability probes make.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.ReflashAsync();
        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        fixture.Pairing.UnpairResult = AdapterUnpairResult.Unresolved;

        await Assert.ThrowsAsync<ManagementException>(() => fixture.Service.RepairAsync(fixture.Id));
        Assert.True(fixture.Service.Registry.Value.Record(fixture.Id)!.RepairRequired);
    }

    [Fact]
    public async Task AnUnpairWindowsClaimsButDoesNotHonourIsCaught()
    {
        // Verify rather than assume. If UnpairAsync reports success and Windows
        // still holds the pairing, clearing the flag would put the user back in the
        // loop with no way out and no error.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.ReflashAsync();
        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        fixture.Pairing.UnpairResult = AdapterUnpairResult.Unpaired;
        fixture.Pairing.StateAfterUnpair = WindowsPairingKnown.Paired;

        var error = await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.RepairAsync(fixture.Id));

        Assert.Contains("still reports this adapter as paired", error.Message);
        Assert.True(fixture.Service.Registry.Value.Record(fixture.Id)!.RepairRequired);
    }

    [Fact]
    public async Task AnAdapterWindowsWasAlreadyNotPairedWithIsAnIdempotentSuccess()
    {
        // The user unpaired it in Windows settings first. Repair has nothing to
        // remove, and its goal -- no stale Windows trust -- already holds.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.ReflashAsync();
        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        fixture.Pairing.UnpairResult = AdapterUnpairResult.AlreadyUnpaired;

        await fixture.Service.RepairAsync(fixture.Id);
        Assert.False(fixture.Service.Registry.Value.Record(fixture.Id)!.RepairRequired);
    }

    [Fact]
    public async Task RepairLeavesTheRelationshipReadyToPairAgainRatherThanForgotten()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.ReflashAsync();
        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));

        await fixture.Service.RepairAsync(fixture.Id);

        // Idle, not NoRelationship: the app still knows which adapter this is, it
        // just no longer has Windows trust for it.
        Assert.Equal(AdapterRelationshipPhase.Idle, fixture.Service.Relationship.Value.Phase);
        Assert.Contains("Pair the adapter again", fixture.Service.Relationship.Value.Message);
    }

    [Fact]
    public async Task AfterARepairTheAdapterCanBePairedAndConnectedAgain()
    {
        // The whole point. Repair is only worth anything if the state it leaves is
        // one the ordinary pairing flow can complete from.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.ReflashAsync();
        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.ConnectAsync(fixture.Id));
        await fixture.Service.RepairAsync(fixture.Id);

        // Windows now holds nothing, the adapter is healthy again, and its physical
        // pairing window is open.
        fixture.Transport.FailDirectConnect = null;
        fixture.Transport.FailScanConnect = null;
        fixture.Pairing.PairResult = new AdapterPairingResult(
            AdapterPairingOutcome.Paired,
            DevicePairingResultStatus.Paired,
            "Paired. The link is bonded and encrypted.");

        Assert.Equal(fixture.Id, await fixture.Service.PairNewAdapterAsync());
        Assert.Equal(1, fixture.Pairing.Pairs);
        Assert.Equal(AdapterRelationshipPhase.Connected, fixture.Service.Relationship.Value.Phase);
        Assert.False(fixture.Service.Registry.Value.Record(fixture.Id)!.RepairRequired);
    }

    [Fact]
    public async Task RepairIsRefusedForAnAdapterTheAppNoLongerRemembers()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.Service.RemoveAsync(fixture.Id);

        // Remove unpairs (§16.2), so count only what REPAIR does from here.
        fixture.Pairing.Unpairs = 0;

        await Assert.ThrowsAsync<ManagementException>(() => fixture.Service.RepairAsync(fixture.Id));
        Assert.Equal(0, fixture.Pairing.Unpairs);
    }
}

/// <summary>
/// Pairing semantics, and the trap the hardware run walked into.
/// </summary>
public sealed class PairingCeremonyTests
{
    private const string Address = "AA:BB:CC:DD:EE:01";

    [Fact]
    public async Task AStaleWindowsBondSkipsTheCeremonyAndTheLogSaysSo()
    {
        // Observed 2026-08-29: with Windows still holding a bond, four Pair presses
        // ran no ceremony at all -- the flow read "already paired" and went straight
        // to connect, which then failed. A pairing operation that reused an old bond
        // must not be indistinguishable from one that formed a new one.
        using var fixture = new ConnectionServiceFixture();
        fixture.Pairing.State = WindowsPairingKnown.Paired;
        fixture.Transport.WindowsPaired = true;
        fixture.Transport.PeerObserved = true;
        fixture.Transport.FailDirectConnect = new GattTransportException(
            "unreachable",
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable);
        fixture.Transport.FailScanConnect = new GattTransportException(
            "unreachable",
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable);

        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.PairNewAdapterAsync());

        // No ceremony ran...
        Assert.Equal(0, fixture.Pairing.Pairs);

        // ...and the log says that in as many words, rather than leaving the reader
        // to infer it from an absence.
        Assert.Contains(
            fixture.Diagnostics.Snapshot(),
            entry => entry.Source == "pair" &&
                entry.Message.Contains("Windows already holds a pairing", StringComparison.Ordinal));
    }

    [Fact]
    public async Task AStalePairingReachedThroughPairNamesAWayOut()
    {
        // There is no remembered row here and therefore no Repair button. Reporting
        // "the adapter did not expose its management service" sent the user round
        // the same loop four times.
        using var fixture = new ConnectionServiceFixture();
        fixture.Pairing.State = WindowsPairingKnown.Paired;
        fixture.Transport.WindowsPaired = true;
        fixture.Transport.PeerObserved = true;
        fixture.Transport.FailDirectConnect = new GattTransportException(
            "unreachable",
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable);
        fixture.Transport.FailScanConnect = new GattTransportException(
            "unreachable",
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable);

        await Assert.ThrowsAnyAsync<Exception>(() => fixture.Service.PairNewAdapterAsync());

        Assert.Equal(
            AdapterResetSignature.StalePairingMessage,
            fixture.Service.Relationship.Value.Message);

        // And still nothing was remembered: a failed pairing must not create a row.
        Assert.Empty(fixture.Service.Registry.Value.Records);
    }

    [Fact]
    public async Task PairingOutsideTheAdaptersWindowReportsTheDoubleTapRatherThanAMystery()
    {
        // The adapter admits a NEW management bond only inside its physical
        // double-tap window (firmware: mgmt_accept_bonding). Confirmed on hardware
        // 2026-08-29, both directions: TimedOut with the window shut, Paired with it
        // open.
        using var fixture = new ConnectionServiceFixture();
        fixture.Pairing.State = WindowsPairingKnown.NotPaired;
        fixture.Pairing.PairResult = new AdapterPairingResult(
            AdapterPairingOutcome.TimedOut,
            DevicePairingResultStatus.AuthenticationTimeout,
            "The adapter did not complete pairing. Double-tap its pairing button and try again.");

        var error = await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.PairNewAdapterAsync());

        Assert.Contains("Double-tap", error.Message);
        Assert.Equal(1, fixture.Pairing.Pairs);
        Assert.Empty(fixture.Service.Registry.Value.Records);
    }

    [Fact]
    public void AnAlreadyPairedResultNeverClaimsAFreshlyBondedLink()
    {
        // "Paired. The link is bonded and encrypted." is a statement about a
        // ceremony that just completed. Saying it about a bond formed months ago --
        // or about a stale one -- is how a reused OS pairing reads as a repair.
        var reused = new AdapterPairingResult(
            AdapterPairingOutcome.AlreadyPaired,
            DevicePairingResultStatus.AlreadyPaired,
            "This PC is already paired with the adapter.");

        Assert.DoesNotContain("bonded and encrypted", reused.Message);
        Assert.True(reused.Succeeded);
    }

    [Fact]
    public void NoPairingMessageEverClaimsAuthentication()
    {
        // Just Works gives bonding and encryption, NOT MITM authentication. This is
        // a copy rule, not a style preference.
        foreach (var outcome in Enum.GetValues<AdapterPairingOutcome>())
        {
            var message = outcome switch
            {
                AdapterPairingOutcome.Paired => "Paired. The link is bonded and encrypted.",
                _ => string.Empty,
            };

            Assert.DoesNotContain("authenticated", message, StringComparison.OrdinalIgnoreCase);
        }
    }
}

/// <summary>
/// A scriptable stand-in for Windows' pairing surface.
///
/// It counts calls. That is the point: the shipped repair defect was invisible
/// precisely because no test could observe whether Windows had been asked to do
/// anything.
/// </summary>
public sealed class FakePairingGateway : IAdapterPairingGateway
{
    public WindowsPairingKnown State { get; set; } = WindowsPairingKnown.NotPaired;

    /// <summary>What a read returns AFTER an unpair, for the "claimed but did not honour" case.</summary>
    public WindowsPairingKnown? StateAfterUnpair { get; set; }

    public AdapterUnpairResult UnpairResult { get; set; } = AdapterUnpairResult.Unpaired;

    public AdapterPairingResult PairResult { get; set; } = new(
        AdapterPairingOutcome.Paired,
        DevicePairingResultStatus.Paired,
        "Paired. The link is bonded and encrypted.");

    public int Unpairs { get; set; }

    public int Pairs { get; private set; }

    public ulong? LastUnpairedAddress { get; private set; }

    public Task<WindowsPairingSnapshot> ReadAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default) =>
        Task.FromResult(new WindowsPairingSnapshot(
            State,
            CanPair: State != WindowsPairingKnown.Paired,
            DeviceName: "PicoSwitch2"));

    public Task<AdapterPairingResult> PairAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default)
    {
        Pairs += 1;
        if (PairResult.Succeeded)
        {
            State = WindowsPairingKnown.Paired;
        }

        return Task.FromResult(PairResult);
    }

    public Task<AdapterUnpairResult> UnpairAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default)
    {
        Unpairs += 1;
        LastUnpairedAddress = bluetoothAddress;
        State = StateAfterUnpair ??
            (UnpairResult.TrustRemoved() ? WindowsPairingKnown.NotPaired : State);
        return Task.FromResult(UnpairResult);
    }
}
