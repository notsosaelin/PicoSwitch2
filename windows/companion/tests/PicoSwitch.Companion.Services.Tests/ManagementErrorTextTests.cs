using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// What the user is actually told when a connect fails.
///
/// Both cases here come from the 2026-08-29 retest, which confirmed the
/// classification and then showed the user something else.
/// </summary>
public sealed class ManagementErrorTextTests
{
    [Fact]
    public void TwoBranchesThatFailedIdenticallyAreSaidOnce()
    {
        // The observed line was:
        //   "The adapter did not expose its management service. · The adapter did
        //    not expose its management service."
        // Saying it twice reads as two distinct problems.
        var aggregate = new AggregateException(
            "both routes failed",
            new ManagementException("The adapter did not expose its management service."),
            new ManagementException("The adapter did not expose its management service."));

        Assert.Equal(
            "The adapter did not expose its management service.",
            ManagementErrorText.Summarize(aggregate));
    }

    [Fact]
    public void TwoBranchesThatFailedDIFFERENTLYAreBothKept()
    {
        // The dedupe must not become "show only the first". When the direct
        // connect and the fallback fail for different reasons, both are evidence.
        var aggregate = new AggregateException(
            "both routes failed",
            new ManagementException("No PicoSwitch2 adapter answered."),
            new ManagementException("The adapter did not expose its management service."));

        Assert.Equal(
            "No PicoSwitch2 adapter answered. · The adapter did not expose its management service.",
            ManagementErrorText.Summarize(aggregate));
    }

    [Fact]
    public void AnOrdinaryFailureIsPassedThroughUnchanged() =>
        Assert.Equal("radio is off", ManagementErrorText.Summarize(new ManagementException("radio is off")));

    [Fact]
    public void AnEmptyAggregateFallsBackToItsOwnMessage() =>
        Assert.Equal(
            "both routes failed",
            ManagementErrorText.Summarize(new AggregateException("both routes failed")));
}

/// <summary>
/// A classified bond mismatch must reach the caller as the DIAGNOSIS.
/// </summary>
public sealed class BondMismatchReportingTests
{
    [Fact]
    public async Task TheThrownMessageIsTheActionableOneNotTheTransportSymptom()
    {
        // 2026-08-29: the relationship correctly reached RepairRequired and the
        // banner said "Repair pairing to continue", while the error surface said
        // "The adapter did not expose its management service" -- twice. The
        // diagnosis was right and the user was told something else.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");
        await fixture.ReflashAsync();

        var error = await Assert.ThrowsAsync<AdapterBondMismatchException>(
            () => fixture.Service.ConnectAsync(fixture.Id));

        Assert.Equal(AdapterResetSignature.RepairMessage, error.Message);
        Assert.Equal(AdapterResetSignature.RepairMessage, ManagementErrorText.Summarize(error));
    }

    [Fact]
    public async Task TheOriginalTaggedFailureSurvivesAsTheInnerException()
    {
        // Replacing the message must not destroy the evidence: the stage, the
        // status and any HRESULT still have to be reachable from the exception.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");
        await fixture.ReflashAsync();

        var error = await Assert.ThrowsAsync<AdapterBondMismatchException>(
            () => fixture.Service.ConnectAsync(fixture.Id));

        var tagged = Assert.IsType<GattTransportException>(
            ((AggregateException)error.InnerException!).InnerExceptions
                .First(branch => branch is GattTransportException));
        Assert.Equal(GattFailureStage.Services, tagged.Stage);
        Assert.Equal(GattCommunicationOutcome.Unreachable, tagged.Outcome);

        // And the classifier can still reach it through the wrapper, which is what
        // actually matters: re-wrapping must not hide the evidence from the policy.
        Assert.True(AdapterResetSignature.IsBondMismatch(error, fixture.Transport.Trust));
    }

    [Fact]
    public async Task AnOrdinaryConnectFailureIsStillReportedVerbatim()
    {
        // The substitution is scoped to a CLASSIFIED bond mismatch. Replacing an
        // ordinary failure's message would hide the real reason behind an offer to
        // unpair, which is the opposite mistake.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync("AA:BB:CC:DD:EE:01");

        await fixture.Service.DisconnectAsync();
        fixture.Transport.PeerObserved = false;
        fixture.Transport.FailDirectConnect = new GattTransportException(
            "unreachable",
            GattFailureStage.Services,
            GattCommunicationOutcome.Unreachable);
        fixture.Transport.FailScanConnect = new ManagementException("No PicoSwitch2 adapter answered.");

        var error = await Assert.ThrowsAnyAsync<Exception>(
            () => fixture.Service.ConnectAsync(fixture.Id));

        Assert.IsNotType<AdapterBondMismatchException>(error);
        Assert.Contains("No PicoSwitch2 adapter answered.", ManagementErrorText.Summarize(error));
    }
}
