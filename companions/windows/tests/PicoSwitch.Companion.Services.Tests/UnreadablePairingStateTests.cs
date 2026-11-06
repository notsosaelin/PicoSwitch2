using PicoSwitch.Companion.Windows.Bluetooth;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// What the app does when it cannot read Windows' pairing state.
///
/// "The probe did not establish an answer" and "the pairing is gone" are
/// different statements, and only the second may destroy a trust relationship.
/// This distinction is the same one the capability probes make, applied to
/// pairing.
/// </summary>
public sealed class UnreadablePairingStateTests
{
    private static readonly AdapterRelationship Adapter = new("AA:BB:CC:DD:EE:01", "Living room");

    [Fact]
    public void AnUnreadablePairingStateAttemptsTheConnectionRatherThanDemandingRepair()
    {
        // An adapter that is merely out of range reads exactly like this. Offering
        // to unpair on the strength of a status read that failed would destroy a
        // working relationship to recover from a question nobody answered.
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        var decision = coordinator.RequestReconnect(
            Adapter,
            AdapterConnectReason.Manual,
            WindowsPairingState.Unknown);

        Assert.IsType<AdapterLifecycleDecision.Connect>(decision);
        Assert.Equal(AdapterRelationshipPhase.Connecting, coordinator.Status.Phase);
    }

    [Fact]
    public void AKnownMissingPairingIsStillRepairRequired()
    {
        // The distinction only means something if the definite case still fires.
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        Assert.IsType<AdapterLifecycleDecision.RepairRequired>(
            coordinator.RequestReconnect(
                Adapter,
                AdapterConnectReason.Manual,
                WindowsPairingState.NotPaired));
        Assert.Equal(AdapterRelationshipPhase.RepairRequired, coordinator.Status.Phase);
    }

    [Fact]
    public void TheConnectRemainsTheAuthorityOverAnUnknownPairing()
    {
        // Having attempted it, a real bond mismatch still classifies correctly --
        // so proceeding on Unknown does not lose the diagnosis, it defers to it.
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        var attempt = Assert.IsType<AdapterLifecycleDecision.Connect>(
            coordinator.RequestReconnect(
                Adapter,
                AdapterConnectReason.Manual,
                WindowsPairingState.Unknown)).Attempt;

        coordinator.ConnectionFailed(attempt.Generation, "refused", bondMismatch: true);
        Assert.Equal(AdapterRelationshipPhase.RepairRequired, coordinator.Status.Phase);
        Assert.Equal(AdapterResetSignature.RepairMessage, coordinator.Status.Message);
    }
}

/// <summary>
/// The diagnostic line the stale-bond experiment reads.
///
/// If this renders wrongly, the experiment costs a flash cycle and produces
/// nothing. Pinned as data rather than trusted.
/// </summary>
public sealed class FailureDiagnosticTests
{
    [Fact]
    public void ATaggedFailureRendersEveryNamespaceItCarries()
    {
        var described = GattStatusFormatter.Describe(
            GattFailureStage.Subscribe,
            GattCommunicationOutcome.AccessDenied,
            protocolError: 0x05,
            hresult: GattStatusFormatter.EBluetoothAttInsufficientAuthentication);

        Assert.Equal(
            "stage=subscribe GattCommunicationStatus=AccessDenied " +
            "ATT=0x05 INSUFFICIENT_AUTHENTICATION " +
            "HRESULT=0x80650005 E_BLUETOOTH_ATT_INSUFFICIENT_AUTHENTICATION",
            described);
    }

    [Fact]
    public void AnUnrecognisedHresultIsStillPrintedInFull()
    {
        // The whole point of the experiment is that the real value may not be one
        // of the ones guessed in advance. An unknown code must arrive legible, not
        // be swallowed.
        var described = GattStatusFormatter.Describe(
            GattFailureStage.Services,
            outcome: null,
            protocolError: null,
            hresult: unchecked((int)0x80650099));

        Assert.Contains("HRESULT=0x80650099 UNKNOWN", described);
    }

    [Fact]
    public void AnUnrecognisedAttErrorIsAlsoPrintedInFull() =>
        Assert.Contains(
            "ATT=0x7F UNKNOWN",
            GattStatusFormatter.Describe(
                GattFailureStage.Command,
                GattCommunicationOutcome.ProtocolError,
                protocolError: 0x7F,
                hresult: null));
}
