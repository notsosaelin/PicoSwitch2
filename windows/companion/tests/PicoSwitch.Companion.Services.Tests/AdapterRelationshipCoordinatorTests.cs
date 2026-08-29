using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Windows.Bluetooth;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The lifecycle of the selected registry row.
///
/// Every rule here is load-bearing, and most were written from a defect: a stale
/// callback advancing a retired attempt, a bond mismatch retried six times over
/// fourteen minutes, and a link reported as Connected before anything proved it
/// was a PicoSwitch2.
/// </summary>
public sealed class AdapterRelationshipCoordinatorTests
{
    private static readonly AdapterRelationship Adapter =
        new("AA:BB:CC:DD:EE:01", "Living room");

    [Fact]
    public void WithNoSavedAdapterTheAppStartsWithNoRelationship() =>
        Assert.Equal(
            AdapterRelationshipPhase.NoRelationship,
            new AdapterRelationshipCoordinator(null).Status.Phase);

    [Fact]
    public void ASavedAdapterStartsIdleRatherThanConnected() =>
        Assert.Equal(
            AdapterRelationshipPhase.Idle,
            new AdapterRelationshipCoordinator(Adapter).Status.Phase);

    [Fact]
    public void DiscoveryNamesThePhysicalRequirement()
    {
        var coordinator = new AdapterRelationshipCoordinator(null);
        coordinator.BeginDiscovery();
        Assert.Equal(AdapterRelationshipPhase.Discovering, coordinator.Status.Phase);

        // A new management-client bond is admitted only while the adapter's
        // double-tap window is open. Omitting that turns a firmware admission rule
        // into a mysterious timeout.
        Assert.Contains("pairing button", coordinator.Status.Message);
    }

    [Fact]
    public void AnAlreadyPairedDeviceGoesStraightToConnecting()
    {
        var coordinator = new AdapterRelationshipCoordinator(null);
        var generation = coordinator.BeginDiscovery();

        var decision = coordinator.DeviceDiscovered(generation, Adapter, WindowsPairingState.Paired);
        Assert.IsType<AdapterLifecycleDecision.Connect>(decision);
        Assert.Equal(AdapterRelationshipPhase.Connecting, coordinator.Status.Phase);
    }

    [Fact]
    public void AnUnpairedDeviceWaitsForTheWindowsCeremony()
    {
        var coordinator = new AdapterRelationshipCoordinator(null);
        var generation = coordinator.BeginDiscovery();

        var decision = Assert.IsType<AdapterLifecycleDecision.AwaitPairing>(
            coordinator.DeviceDiscovered(generation, Adapter, WindowsPairingState.NotPaired));
        Assert.True(decision.StartPairing);
        Assert.Equal(AdapterRelationshipPhase.Pairing, coordinator.Status.Phase);

        Assert.IsType<AdapterLifecycleDecision.Connect>(
            coordinator.PairingCompleted(generation, WindowsPairingState.Paired));
        Assert.Equal(AdapterRelationshipPhase.Connecting, coordinator.Status.Phase);
    }

    [Fact]
    public void ARepeatedDiscoveryResultNeverStartsASecondAttempt()
    {
        // A watcher can report the same advertisement again while the first result
        // is still being acted on.
        var coordinator = new AdapterRelationshipCoordinator(null);
        var generation = coordinator.BeginDiscovery();
        coordinator.DeviceDiscovered(generation, Adapter, WindowsPairingState.Paired);

        var second = coordinator.DeviceDiscovered(generation, Adapter, WindowsPairingState.Paired);
        Assert.IsType<AdapterLifecycleDecision.Ignored>(second);
    }

    [Fact]
    public void AStaleGenerationIsInert()
    {
        var coordinator = new AdapterRelationshipCoordinator(null);
        var stale = coordinator.BeginDiscovery();
        coordinator.BeginDiscovery();

        Assert.IsType<AdapterLifecycleDecision.Ignored>(
            coordinator.DeviceDiscovered(stale, Adapter, WindowsPairingState.Paired));
    }

    [Fact]
    public void ConnectedCanOnlyBeEnteredThroughValidation()
    {
        var coordinator = new AdapterRelationshipCoordinator(null);
        var generation = coordinator.BeginDiscovery();
        coordinator.DeviceDiscovered(generation, Adapter, WindowsPairingState.Paired);

        // A working carrier is not a verified adapter. Skipping validation must not
        // reach Connected.
        Assert.Null(coordinator.IdentityValidated(generation));
        Assert.Equal(AdapterRelationshipPhase.Connecting, coordinator.Status.Phase);

        Assert.True(coordinator.CarrierReady(generation));
        Assert.Equal(AdapterRelationshipPhase.Validating, coordinator.Status.Phase);

        Assert.NotNull(coordinator.IdentityValidated(generation));
        Assert.Equal(AdapterRelationshipPhase.Connected, coordinator.Status.Phase);
    }

    [Fact]
    public void ADeviceThatIsNotAPicoSwitch2FailsWithoutAdoptingIt()
    {
        var coordinator = new AdapterRelationshipCoordinator(null);
        var generation = coordinator.BeginDiscovery();
        coordinator.DeviceDiscovered(generation, Adapter, WindowsPairingState.Paired);
        coordinator.CarrierReady(generation);

        coordinator.IdentityRejected(generation);
        Assert.Equal(AdapterRelationshipPhase.Failed, coordinator.Status.Phase);
        Assert.Equal(AdapterRelationshipCoordinator.IdentityRejectedMessage, coordinator.Status.Message);

        // Discovering *a* device is not permission to adopt it: nothing was saved,
        // so a later reconnect has no relationship to return to.
        Assert.Equal(AdapterRelationshipPhase.Failed, coordinator.Status.Phase);
    }

    [Fact]
    public void ABondMismatchIsTerminalAndNotRetried()
    {
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        var decision = coordinator.RequestReconnect(
            Adapter,
            AdapterConnectReason.ForegroundAuto,
            WindowsPairingState.Paired);
        var attempt = Assert.IsType<AdapterLifecycleDecision.Connect>(decision).Attempt;

        var failure = coordinator.ConnectionFailed(
            attempt.Generation,
            "refused",
            bondMismatch: true);

        // Retrying cannot succeed: the adapter has no key to authenticate with.
        // Six such attempts over fourteen minutes were observed before the OS
        // dropped its own bond and repair finally triggered.
        Assert.IsType<AdapterLifecycleDecision.RepairRequired>(failure);
        Assert.Equal(AdapterRelationshipPhase.RepairRequired, coordinator.Status.Phase);
        Assert.Equal(AdapterResetSignature.RepairMessage, coordinator.Status.Message);

        // And the message names an action the app performs, not a trip to Settings.
        Assert.Contains("Repair pairing", coordinator.Status.Message);
    }

    [Fact]
    public void AnOrdinaryConnectFailureIsRetryable()
    {
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        var attempt = Assert.IsType<AdapterLifecycleDecision.Connect>(
            coordinator.RequestReconnect(
                Adapter,
                AdapterConnectReason.Manual,
                WindowsPairingState.Paired)).Attempt;

        coordinator.ConnectionFailed(attempt.Generation, "out of range");
        Assert.Equal(AdapterRelationshipPhase.Failed, coordinator.Status.Phase);

        // Failed is retryable: a second request is accepted.
        Assert.IsType<AdapterLifecycleDecision.Connect>(
            coordinator.RequestReconnect(
                Adapter,
                AdapterConnectReason.Manual,
                WindowsPairingState.Paired));
    }

    [Fact]
    public void AReconnectIsInertWhileAnAttemptIsActiveOrConnected()
    {
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        coordinator.RequestReconnect(Adapter, AdapterConnectReason.Manual, WindowsPairingState.Paired);
        Assert.IsType<AdapterLifecycleDecision.Ignored>(
            coordinator.RequestReconnect(Adapter, AdapterConnectReason.Manual, WindowsPairingState.Paired));

        var generation = coordinator.Status.Generation;
        coordinator.CarrierReady(generation);

        // Validating counts as active: a second attempt underneath would race the
        // identity check.
        Assert.IsType<AdapterLifecycleDecision.Ignored>(
            coordinator.RequestReconnect(Adapter, AdapterConnectReason.Manual, WindowsPairingState.Paired));

        coordinator.IdentityValidated(generation);
        Assert.IsType<AdapterLifecycleDecision.Ignored>(
            coordinator.RequestReconnect(Adapter, AdapterConnectReason.Manual, WindowsPairingState.Paired));
    }

    [Fact]
    public void ReconnectingWithoutAWindowsPairingIsRepairNotAFailedConnect()
    {
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        var decision = coordinator.RequestReconnect(
            Adapter,
            AdapterConnectReason.ForegroundAuto,
            WindowsPairingState.NotPaired);

        Assert.IsType<AdapterLifecycleDecision.RepairRequired>(decision);
        Assert.Equal(AdapterRelationshipPhase.RepairRequired, coordinator.Status.Phase);
    }

    [Fact]
    public void LosingTheLinkRetainsTheRelationship()
    {
        var coordinator = new AdapterRelationshipCoordinator(null);
        var generation = coordinator.BeginDiscovery();
        coordinator.DeviceDiscovered(generation, Adapter, WindowsPairingState.Paired);
        coordinator.CarrierReady(generation);
        coordinator.IdentityValidated(generation);

        Assert.True(coordinator.ConnectionEnded("link lost"));

        // Idle, not NoRelationship: losing a link never deletes relationship truth.
        Assert.Equal(AdapterRelationshipPhase.Idle, coordinator.Status.Phase);
        Assert.Equal("link lost", coordinator.Status.Message);
    }

    [Fact]
    public void CancellingRetainsTheRelationshipToo()
    {
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        coordinator.RequestReconnect(Adapter, AdapterConnectReason.Manual, WindowsPairingState.Paired);
        coordinator.CancelAndRetainRelationship("cancelled");
        Assert.Equal(AdapterRelationshipPhase.Idle, coordinator.Status.Phase);
    }

    [Fact]
    public void OnlyAnExplicitForgetRemovesTheRelationship()
    {
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        coordinator.Forget();
        Assert.Equal(AdapterRelationshipPhase.NoRelationship, coordinator.Status.Phase);

        // And a cancel after a forget cannot resurrect it.
        coordinator.CancelAndRetainRelationship();
        Assert.Equal(AdapterRelationshipPhase.NoRelationship, coordinator.Status.Phase);
    }

    [Fact]
    public void WindowsDroppingItsPairingWhileIdleIsRepairRequired()
    {
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        var decision = coordinator.PairingLost(Adapter.Address);
        Assert.IsType<AdapterLifecycleDecision.RepairRequired>(decision);
        Assert.Equal(AdapterRelationshipPhase.RepairRequired, coordinator.Status.Phase);
    }

    [Fact]
    public void APairingLossForSomeOtherDeviceIsIgnored()
    {
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        Assert.IsType<AdapterLifecycleDecision.Ignored>(
            coordinator.PairingLost("AA:BB:CC:DD:EE:99"));
        Assert.Equal(AdapterRelationshipPhase.Idle, coordinator.Status.Phase);
    }

    [Fact]
    public void ARefusedPairingIsFailedRatherThanRepairRequired()
    {
        // Nothing was ever paired, so there is no stale trust to tear down. Offering
        // Repair would offer to unpair something that does not exist.
        var coordinator = new AdapterRelationshipCoordinator(null);
        var generation = coordinator.BeginDiscovery();
        coordinator.DeviceDiscovered(generation, Adapter, WindowsPairingState.NotPaired);

        coordinator.PairingCompleted(generation, WindowsPairingState.NotPaired);
        Assert.Equal(AdapterRelationshipPhase.Failed, coordinator.Status.Phase);
        Assert.Equal(AdapterRelationshipCoordinator.PairingFailedMessage, coordinator.Status.Message);
    }

    [Fact]
    public void RestoreDoesNotDisturbAnAttemptInFlight()
    {
        var coordinator = new AdapterRelationshipCoordinator(Adapter);
        coordinator.RequestReconnect(Adapter, AdapterConnectReason.Manual, WindowsPairingState.Paired);

        coordinator.Restore(Adapter, WindowsPairingState.Paired);
        Assert.Equal(AdapterRelationshipPhase.Connecting, coordinator.Status.Phase);
    }
}
